#include "content_setup.h"
#include "content_patch.h"
#include "disc_image.h"
#include "svod_image.h"
#include "em1_vfs.h"
#include "demo_setup.h"
#include <rex/kernel/kernel_state.h>
#include <windows.h>
#include <shlobj.h>
#include <winioctl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <thread>
#include <mutex>
#include <vector>
#include <immintrin.h>
#include <rex/logging.h>

namespace em2 {
namespace {
namespace fs=std::filesystem;
constexpr auto& patches=kPatchedGameArchives;
bool OriginalFolder(const fs::path& source) {
    return fs::is_regular_file(source/"default.xex") && fs::is_regular_file(source/"ConfigFiles.ini") && fs::is_directory(source/"packfiles");
}
std::wstring Wide(const std::string& value) {
    if(value.empty())return {};
    int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0);
    if(!size)return L"Game content setup failed. See beetleworx.log for details.";
    std::wstring result(size,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),int(value.size()),result.data(),size);return result;
}
fs::path Marker(const fs::path& content,const char* filename="source-game-directory.txt") {
    try {
    std::ifstream stream(content/filename,std::ios::binary);
    std::array<char,32770> line{};stream.getline(line.data(),line.size());
    if(stream.fail() && !stream.eof())return {};
    if(!stream.gcount())return {};
    const auto count=size_t(stream.gcount())-(stream.eof()?0:1);
    std::string source(line.data(),count);
    if(source.find('\0')!=std::string::npos)return {};
    if(!source.empty() && source.back()=='\r')source.pop_back();
    if(source.size()>=3 && source.substr(0,3)=="\xEF\xBB\xBF")source.erase(0,3);
    return source.size()>32768?fs::path{}:fs::path(std::u8string(source.begin(),source.end()));
    }catch(const std::exception&){return {};}
}
bool StoreMarker(const fs::path& content,const char* filename,const fs::path& source) {
    const auto encoded=fs::canonical(source).u8string();
    static std::atomic<uint64_t> nonce{0};
    const auto temporary=content/(L".em2-marker-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++nonce)+L".tmp");
    const auto file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    const auto text=std::string(reinterpret_cast<const char*>(encoded.data()),encoded.size())+'\n';DWORD written=0;
    const bool ready=WriteFile(file,text.data(),DWORD(text.size()),&written,nullptr) && written==text.size() && FlushFileBuffers(file);
    CloseHandle(file);
    const bool result=ready && MoveFileExW(temporary.c_str(),(content/filename).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    if(!result)DeleteFileW(temporary.c_str());
    return result;
}
std::mutex library_mutex;
fs::path library_directory;
std::atomic<int> em1_library_status{0};
std::atomic<bool> library_selecting{false};
fs::path ChooseSource(bool em1=false);
fs::path ChooseEm1Source(){return ChooseSource(true);}

bool Nested(const fs::path& candidate,const fs::path& parent) {
    const auto child=fs::weakly_canonical(candidate),base=fs::weakly_canonical(parent);
    auto a=child.begin(),b=base.begin();
    for(;b!=base.end();++a,++b)if(a==child.end() || CompareStringOrdinal(a->c_str(),-1,b->c_str(),-1,TRUE)!=CSTR_EQUAL)return false;
    return true;
}
bool Junction(const fs::path& target,const fs::path& source) {
    if(fs::exists(target))return fs::is_directory(target);
    const auto absolute=fs::canonical(source).wstring();
    const std::wstring substitute=L"\\??\\"+absolute;
    if(substitute.size()+absolute.size()>15000)return false;
    // Mount-point reparse buffers do not require the symbolic-link privilege.
    struct Header {DWORD tag;WORD length,reserved,substitute_offset,substitute_length,print_offset,print_length;};
    static_assert(sizeof(Header)==16);
    const size_t bytes=sizeof(Header)+(substitute.size()+absolute.size()+2)*sizeof(wchar_t);
    std::vector<uint8_t> buffer(bytes,0);auto* header=reinterpret_cast<Header*>(buffer.data());
    header->tag=IO_REPARSE_TAG_MOUNT_POINT;header->length=WORD(bytes-8);
    header->substitute_length=WORD(substitute.size()*2);header->print_offset=WORD((substitute.size()+1)*2);header->print_length=WORD(absolute.size()*2);
    auto* paths=reinterpret_cast<wchar_t*>(buffer.data()+sizeof(Header));
    std::copy(substitute.begin(),substitute.end(),paths);std::copy(absolute.begin(),absolute.end(),paths+substitute.size()+1);
    if(!CreateDirectoryW(target.c_str(),nullptr))return false;
    const auto handle=CreateFileW(target.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(handle==INVALID_HANDLE_VALUE){RemoveDirectoryW(target.c_str());return false;}
    DWORD returned=0;const bool result=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,buffer.data(),DWORD(buffer.size()),nullptr,0,&returned,nullptr)!=0;
    CloseHandle(handle);
    // Only this newly created, empty directory may be removed on failure.
    if(!result)RemoveDirectoryW(target.c_str());
    return result;
}
class SourcePickerEvents final:public IFileDialogEvents,public IFileDialogControlEvents {
    ULONG references_=1;
public:
    bool folder_requested=false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** value) override {
        if(!value)return E_POINTER;*value=nullptr;
        if(IsEqualIID(iid,IID_IUnknown) || IsEqualIID(iid,__uuidof(IFileDialogEvents)))*value=static_cast<IFileDialogEvents*>(this);
        else if(IsEqualIID(iid,__uuidof(IFileDialogControlEvents)))*value=static_cast<IFileDialogControlEvents*>(this);
        else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++references_;}
    ULONG STDMETHODCALLTYPE Release()override{const auto remaining=--references_;if(!remaining)delete this;return remaining;}
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*,IShellItem*)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*,IShellItem*,FDE_SHAREVIOLATION_RESPONSE* response)override{*response=FDESVR_DEFAULT;return S_OK;}
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*,IShellItem*,FDE_OVERWRITE_RESPONSE* response)override{*response=FDEOR_DEFAULT;return S_OK;}
    HRESULT STDMETHODCALLTYPE OnItemSelected(IFileDialogCustomize*,DWORD,DWORD)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnCheckButtonToggled(IFileDialogCustomize*,DWORD,BOOL)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnControlActivating(IFileDialogCustomize*,DWORD)override{return S_OK;}
    HRESULT STDMETHODCALLTYPE OnButtonClicked(IFileDialogCustomize* customize,DWORD id)override {
        if(id==100){folder_requested=true;IFileDialog* dialog=nullptr;if(SUCCEEDED(customize->QueryInterface(IID_PPV_ARGS(&dialog)))){dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));dialog->Release();}}
        return S_OK;
    }
};
fs::path ChooseSource(bool em1) {
    fs::path selected;
    // Use a dedicated STA so the host application's COM mode cannot break the
    // Windows folder picker. The picker runs before the guest starts.
    std::thread picker([&]{
        const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        if(FAILED(initialized))return;
        bool folders=false;
        do {
            IFileDialog* dialog=nullptr;
            if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))break;
            DWORD options=0;dialog->GetOptions(&options);dialog->SetOptions(options|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR|(folders?FOS_PICKFOLDERS:FOS_FILEMUSTEXIST));
            dialog->SetTitle(em1?(folders?L"Select your extracted Epic Mickey Wii folder":L"Select your Epic Mickey Wii disc image"):(folders?L"Select your EM2 game folder or GOD package folder":L"Select your Epic Mickey 2 Xbox 360 image or GOD package (All files)"));
            dialog->SetOkButtonLabel(folders?L"Use game folder":L"Use disc image");
            auto* events=new SourcePickerEvents;DWORD cookie=0;
            if(!folders) {
                const COMDLG_FILTERSPEC filters[]={{em1?L"Wii disc images":L"Xbox 360 disc images",em1?L"*.rvz;*.iso;*.gcm;*.wia;*.wbfs;*.ciso;*.gcz;*.nfs":L"*.iso;*.xiso;*.iso.*;*.xiso.*;*.cso;*.zso;*.cci;*.chd;*.god;*.zar;*.zip;*.7z;*.img;*.bin"},{L"All files",L"*.*"}};dialog->SetFileTypes(2,filters);
                IFileDialogCustomize* customize=nullptr;
                if(SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&customize)))){customize->AddPushButton(100,em1?L"Use an extracted game folder instead":L"Use a game folder instead");customize->Release();}
                dialog->Advise(events,&cookie);
            }
            if(SUCCEEDED(dialog->Show(nullptr))) {
                IShellItem* item=nullptr;if(SUCCEEDED(dialog->GetResult(&item))){PWSTR path=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))){selected=path;CoTaskMemFree(path);}item->Release();}
            }
            if(cookie)dialog->Unadvise(cookie);
            const bool choose_folder=!folders && events->folder_requested;
            events->Release();dialog->Release();
            if(!choose_folder)break;folders=true;
        }while(true);
        CoUninitialize();
    });picker.join();return selected;
}
bool Install(const fs::path& source,const fs::path& content,std::string& error,IProgressDialog* progress=nullptr) {
    if(Nested(content,source) || Nested(source,content)){error="Keep the Beetleworx folder and original game folder separate, then start beetleworx.exe again.";return false;}
    fs::create_directories(content/"packfiles");
    // Persist ownership before creating even one link. A canceled import can
    // resume only against the identical source, preventing a mixed game view.
    constexpr const char* intent_name="content-setup-source.txt";
    const auto pending=Marker(content,intent_name);
    if(fs::exists(content/intent_name)) {
        std::error_code equivalent_error;
        if(pending.empty() || !fs::equivalent(source,pending,equivalent_error)) {
            error="An unfinished setup used a different game folder. Resume with that folder, or extract a fresh recomp release folder.";return false;
        }
    } else if(!StoreMarker(content,intent_name,source)) {
        error="Could not save content setup progress. Move the recomp to a writable folder and try again.";return false;
    }
    uint64_t completed=0,total=0;
    if(progress){for(const auto& entry:fs::directory_iterator(source))++total;for(const auto& entry:fs::directory_iterator(source/"packfiles"))++total;}
    auto link=[&](const fs::directory_entry& entry,const fs::path& target) {
        if(progress){if(progress->HasUserCancelled())return false;progress->SetLine(2,entry.path().filename().c_str(),FALSE,nullptr);progress->SetProgress64(++completed,total);}
        if(fs::exists(target))return true;
        if(entry.is_directory())return Junction(target,entry.path());
        if(!entry.is_regular_file())return true;
        if(CreateHardLinkW(target.c_str(),entry.path().c_str(),nullptr))return true;
        // Different volumes do not support hard links. CopyFile never replaces
        // an existing patched archive or modifies the selected original.
        return CopyFileExW(entry.path().c_str(),target.c_str(),
            [](LARGE_INTEGER,LARGE_INTEGER,LARGE_INTEGER,LARGE_INTEGER,DWORD,DWORD,HANDLE,HANDLE,LPVOID context)->DWORD {
                auto* dialog=static_cast<IProgressDialog*>(context);
                return dialog && dialog->HasUserCancelled()?PROGRESS_CANCEL:PROGRESS_CONTINUE;
            },progress,nullptr,COPY_FILE_FAIL_IF_EXISTS)!=0;
    };
    for(const auto& entry:fs::directory_iterator(source)) {
        const auto name=entry.path().filename();
        if(name==L"packfiles" || name==L"ConfigFiles.ini" || name==L"$SystemUpdate" || name.native().starts_with(L".em2-") || name==L"source-game-directory.txt" || name==L"content-setup-source.txt")continue;
        if(!link(entry,content/name)){error="Could not prepare game content. Keep the recomp in a writable folder, preferably on the same drive as the original game, and try again.";return false;}
    }
    for(const auto& entry:fs::directory_iterator(source/"packfiles")) {
        const auto name=entry.path().filename().string();
        if(std::find(patches.begin(),patches.end(),name)!=patches.end())continue;
        if(!link(entry,content/"packfiles"/entry.path().filename())){error="Could not link or copy a game archive. Check free space and folder permissions, then try again.";return false;}
    }
    if(!StoreMarker(content,"source-game-directory.txt",source)){error="Could not save the selected game location. Move the recomp to a writable folder and try again.";return false;}
    // The completed source marker is the only path metadata allowed into
    // content identity. Delete only our own transient setup intent file.
    if(!fs::remove(content/intent_name)){error="Could not finish content setup metadata. Check folder permissions and retry.";return false;}
    return true;
}
}
bool PrepareGameContent(const fs::path& executable_directory,fs::path& game,const fs::path& content,bool explicit_game,bool interactive,std::string& error) {
    try {
        error.clear();
        const auto saved=Marker(content);
        const auto pending=Marker(content,"content-setup-source.txt");
        if(fs::exists(content/"content-setup-source.txt") && pending.empty()){error="Content setup progress is incomplete. Extract a fresh recomp release folder and try again.";return false;}
        const auto remembered=saved.empty()?pending:saved;
        if(!explicit_game && !remembered.empty() && OriginalFolder(remembered))game=remembered;
        auto extract_image=[&] {
            if(fs::is_directory(game) && !OriginalFolder(game)){
                const auto package=SvodImage::FindPackage(game,error);if(!error.empty())return false;if(!package.empty())game=package;
            }
            auto extension=game.extension().wstring();for(auto& ch:extension)if(ch>=L'A' && ch<=L'Z')ch+=32;
            if(!fs::is_regular_file(game))return true;
            if(extension==L".xex" && OriginalFolder(game.parent_path())){game=game.parent_path();return true;}
            if(!interactive){error="Disc-image import is available during normal setup. Diagnostics require an already prepared content folder.";return false;}
            const auto image=game,destination=executable_directory/"game-data";bool extracted=false;
            std::thread importer([&]{
                const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);IProgressDialog* progress=nullptr;
                if(SUCCEEDED(initialized) && SUCCEEDED(CoCreateInstance(CLSID_ProgressDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&progress)))){progress->SetTitle(L"Importing Epic Mickey 2 disc");progress->StartProgressDialog(nullptr,nullptr,PROGDLG_AUTOTIME|PROGDLG_NOMINIMIZE,nullptr);}
                extracted=ImportXbox360Disc(image,destination,[&](std::wstring_view stage,uint64_t completed,uint64_t total){if(!progress)return true;const std::wstring text(stage);progress->SetLine(1,text.c_str(),FALSE,nullptr);progress->SetProgress64(completed,total);return !progress->HasUserCancelled();},error);
                if(progress){progress->StopProgressDialog();progress->Release();}if(SUCCEEDED(initialized))CoUninitialize();
            });importer.join();if(extracted)game=destination;return extracted;
        };
        if(!extract_image())return false;
        if(!OriginalFolder(game)) {
            if(explicit_game || !interactive){error="Game folder must contain default.xex, ConfigFiles.ini and packfiles. Select the extracted Xbox 360 game folder.";return false;}
            game=ChooseSource();if(game.empty())return false;
            if(!extract_image())return false;
            if(!OriginalFolder(game)){error="That folder does not contain default.xex, ConfigFiles.ini and packfiles. Extract your Xbox 360 game files first, then choose that folder.";return false;}
        }
        if(!pending.empty()) {
            std::error_code equivalent_error;
            if(!fs::equivalent(game,pending,equivalent_error)){error="An unfinished setup belongs to a different game folder. Resume with that folder, or extract a fresh recomp release folder.";return false;}
        }
        std::error_code equivalent_error;
        const bool same=!saved.empty() && fs::equivalent(game,saved,equivalent_error);
        if(!interactive) {
            // Existing private renderer fixtures intentionally carry only their
            // required assets. Keep their previous staged-path contract, rather
            // than enforcing the ordinary player's complete release payload.
            if(same && pending.empty() && fs::is_regular_file(content/"default.xex"))return true;
            error="The staged content view is missing or does not match the selected game folder. Prepare content before running a diagnostic or explicit content-directory launch.";return false;
        }
        if(Nested(content,game) || Nested(game,content) || Nested(content/"packfiles",game)){
            error="Keep the recomp content folder and original game folder separate. The content packfiles folder must not link to your original packfiles folder.";return false;
        }
        for(const auto* name:patches){
            const auto delta=content/"patches"/(std::string(name)+".em2patch");
            if(fs::is_regular_file(delta)){
                if(!ApplyContentPatch(game/"packfiles"/name,delta,content/"packfiles"/name,error))return false;
            }else if(!fs::is_regular_file(content/"packfiles"/name)){
                error="The recomp package is incomplete: a game content patch is missing. Extract the complete release folder and try again.";return false;
            }
        }
        if(!fs::is_regular_file(content/"pc-shader-catalog.em2")){error="The recomp package is incomplete: pc-assets/pc-shader-catalog.em2 is missing. Extract the complete release folder.";return false;}
        if(same && pending.empty() && fs::is_regular_file(content/"default.xex"))return true;
        if(!saved.empty() && !same){error="This content view belongs to another game folder. Extract a fresh recomp release folder to select a different game location.";return false;}
        bool installed=false;
        std::thread importer([&]{
            const auto initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            IProgressDialog* progress=nullptr;
            if(SUCCEEDED(initialized) && SUCCEEDED(CoCreateInstance(CLSID_ProgressDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&progress)))) {
                progress->SetTitle(L"Preparing Epic Mickey 2");progress->SetLine(1,L"Reusing your original game files. Different drives may require copying some archives.",FALSE,nullptr);
                progress->StartProgressDialog(nullptr,nullptr,PROGDLG_NOMINIMIZE,nullptr);
            }
            try{installed=Install(game,content,error,progress);}catch(const std::exception& exception){error=exception.what();}
            if(progress){if(progress->HasUserCancelled())error.clear();progress->StopProgressDialog();progress->Release();}
            if(SUCCEEDED(initialized))CoUninitialize();
        });importer.join();return installed;
    }catch(const std::exception& exception){error="Game content setup failed: "+std::string(exception.what());return false;}
}
void ShowContentSetupError(const std::string& error) {if(!error.empty())MessageBoxW(nullptr,Wide(error).c_str(),L"Beetleworx setup",MB_OK|MB_ICONERROR);}
void InitializeContentLibrary(const fs::path& directory) {
    std::lock_guard lock(library_mutex);
    library_directory=directory;
    const auto source=Marker(directory,"em1-source.txt");
    em1_library_status=source.empty()?0:((fs::is_regular_file(source) || fs::is_directory(source))?0:2);
}
fs::path ConfiguredEm1Source() {
    std::lock_guard lock(library_mutex);
    return library_directory.empty()?fs::path{}:Marker(library_directory,"em1-source.txt");
}
bool SelectEm1SourceForSetup(const fs::path& directory,std::string& error) {
    error.clear();
    try {
        const auto selected=ChooseEm1Source();if(selected.empty())return false;
        auto source=em1::RvzSource::Open(selected,error);
        if(!source || !source->DiscId().starts_with("SEM")){if(error.empty())error="This is not an Epic Mickey Wii disc image or extracted folder.";return false;}
        fs::create_directories(directory);
        if(!StoreMarker(directory,"em1-source.txt",selected)){error="Could not remember the Wii source. Extract the demo to a writable folder.";return false;}
        InitializeContentLibrary(directory);return true;
    }catch(const std::exception& e){error="Could not add the Wii source: "+std::string(e.what());return false;}
}
bool NativeContentLibraryOption(int id,int choice,int& result) {
    struct RestoreHost {DWORD error=GetLastError();unsigned csr=_mm_getcsr();~RestoreHost(){SetLastError(error);_mm_setcsr(csr);}} restore;
    if(id==7203 && choice==-1){result=PublicDemoBuild()?1:0;return true;}
    if((id!=7200 && id!=7201) || (choice!=-1 && choice!=1))return false;
    if(id==7201){result=1;return true;}
    auto* kernel=rex::kernel::kernel_state();
    auto* filesystem=kernel?kernel->file_system():nullptr;
    if(filesystem && em1::CurrentLevelCatalog()) {
        result=em1_library_status==6?6:1;
        return true;
    }
    if(choice==-1){result=em1_library_status;return true;}
    if(library_selecting.exchange(true)){result=em1_library_status;return true;}
    struct Selecting {~Selecting(){library_selecting=false;}} selecting;
    try {
        const auto selected=ChooseEm1Source();
        if(selected.empty()){result=em1_library_status;return true;}
        std::string error;
        std::shared_ptr<em1::RvzSource> source=em1::RvzSource::Open(selected,error);
        if(!source || !filesystem || !em1::MountRvz(*filesystem,source,error)) {
            em1_library_status=3;
            ShowContentSetupError(error.empty()?"The game content library is not available yet.":error);
        } else {
            std::lock_guard lock(library_mutex);
            if(!library_directory.empty())fs::create_directories(library_directory);
            const bool saved=!library_directory.empty() && StoreMarker(library_directory,"em1-source.txt",selected);
            em1_library_status=saved?1:6;
            if(!saved)ShowContentSetupError("The EM1 disc is mounted for this session, but its location could not be saved. Check that your user_data folder is writable.");
            REXLOG_INFO("Game Library: original EM1 source mounted read-only, remembered {}",saved);
        }
    }catch(const std::exception& error){
        em1_library_status=em1::CurrentLevelCatalog()?6:3;
        ShowContentSetupError("Could not add the EM1 disc: "+std::string(error.what()));
    }
    result=em1_library_status;return true;
}
bool TestContentLibraryPersistence(const fs::path& root) {
    try {
        if(fs::exists(root))return false;
        fs::create_directories(root);
        const auto source=root/fs::path(L"Epic Mickey \u00e9.rvz");
        std::ofstream(source,std::ios::binary)<<"unchanged-disc-fixture";
        if(!StoreMarker(root,"em1-source.txt",source) || !fs::equivalent(Marker(root,"em1-source.txt"),source))return false;
        const auto canonical=fs::canonical(source).u8string();
        {std::ofstream out(root/"em1-source.txt",std::ios::binary);out<<"\xEF\xBB\xBF";out.write(reinterpret_cast<const char*>(canonical.data()),canonical.size());out<<"\r\n";}
        if(!fs::equivalent(Marker(root,"em1-source.txt"),source))return false;
        {std::ofstream out(root/"em1-source.txt",std::ios::binary);out<<std::string(40000,'x');}
        if(!Marker(root,"em1-source.txt").empty())return false;
        {std::ofstream out(root/"em1-source.txt",std::ios::binary);out.write("fake\0disc",9);}
        if(!Marker(root,"em1-source.txt").empty())return false;
        if(!StoreMarker(root,"em1-source.txt",source))return false;
        fs::remove(source);
        if(Marker(root,"em1-source.txt")!=fs::path(canonical))return false;
        REXLOG_INFO("Content library persistence passed: Unicode paths, BOM/CRLF, bounded malformed paths, atomic replacement and missing-source recovery");
        return true;
    }catch(const std::exception& error){REXLOG_ERROR("Content library fixture: {}",error.what());return false;}
}
bool TestContentSetup(const fs::path& root) {
    try {
        if(fs::exists(root))return false;
        const auto source=root/"original",content=root/"recomp"/"pc-assets";
        fs::create_directories(source/"packfiles");fs::create_directories(source/"Audio");fs::create_directories(content/"packfiles");
        auto write=[](const fs::path& path,const char* text){std::ofstream(path,std::ios::binary)<<text;};
        write(source/"default.xex","original-xex");write(source/"ConfigFiles.ini","original-config");write(source/"Audio"/"sample","original-audio");write(source/"packfiles"/"level.pak","original-level");
        write(content/"pc-shader-catalog.em2","catalog");for(auto name:patches){write(source/"packfiles"/name,"original-menu");write(content/"packfiles"/name,"modified-menu");}
        std::string error;auto game=source;
        if(PrepareGameContent(root/"recomp",game,content,true,false,error) || error.empty())return false;
        if(!Install(game,content,error))return false;
        auto read=[](const fs::path& path){std::ifstream in(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});};
        if(read(source/"ConfigFiles.ini")!="original-config" || fs::exists(content/"ConfigFiles.ini") ||
           read(source/"packfiles"/"UIOptions.pak")!="original-menu" || read(content/"packfiles"/"UIOptions.pak")!="modified-menu" ||
           read(content/"Audio"/"sample")!="original-audio" || read(content/"packfiles"/"level.pak")!="original-level")return false;
        game=root/"nonexistent-compiled-default";
        if(!PrepareGameContent(root/"recomp",game,content,false,false,error) || !fs::equivalent(game,source))return false;
        game=root/"missing-explicit";
        if(PrepareGameContent(root/"recomp",game,content,true,false,error) || error.empty())return false;
        const auto partial=root/"interrupted"/"pc-assets",different=root/"different-original";
        fs::create_directories(partial/"packfiles");fs::create_directories(different/"packfiles");
        write(partial/"pc-shader-catalog.em2","catalog");for(auto name:patches)write(partial/"packfiles"/name,"modified-menu");
        write(different/"default.xex","different-xex");write(different/"ConfigFiles.ini","different-config");
        if(!StoreMarker(partial,"content-setup-source.txt",source) || !CreateHardLinkW((partial/"default.xex").c_str(),(source/"default.xex").c_str(),nullptr))return false;
        if(Install(different,partial,error) || error.empty() || read(partial/"default.xex")!="original-xex" || fs::exists(partial/"source-game-directory.txt"))return false;
        if(!Install(source,partial,error) || fs::exists(partial/"content-setup-source.txt") || !fs::equivalent(Marker(partial),source))return false;
        game=source;if(!PrepareGameContent(root/"interrupted",game,partial,true,false,error))return false;
        REXLOG_INFO("Content setup passed: portable saved path, strict noninteractive/explicit paths, junction/hard-link view, patched archive/original preservation and interrupted-import source ownership/recovery (no dialogs launched)");return true;
    }catch(const std::exception& error){REXLOG_ERROR("Content setup fixture: {}",error.what());return false;}
}
}

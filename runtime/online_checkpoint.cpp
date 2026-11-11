#include "online_checkpoint.h"
#include "online_identity.h"
#include "online_invite.h"
#include <windows.h>
#include <rex/sha256.h>
#include <rex/logging.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <stdexcept>

namespace em2::net {
namespace {
namespace fs=std::filesystem;
using Hash=std::array<uint8_t,32>;
using Bytes=std::vector<uint8_t>;
constexpr uint64_t kMaxData=32*1024*1024,kMaxFile=8*1024*1024;
constexpr unsigned kMaxFiles=128;
void Require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
void Cancel(std::stop_token stop){Require(!stop.stop_requested(),"Checkpoint operation cancelled");}
Hash Digest(std::span<const uint8_t> bytes) {
    const auto text=rex::util::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
    Require(text.size()==64,"Checkpoint SHA-256 unavailable");Hash result{};
    for(unsigned i=0;i<32;++i){unsigned value=0;const auto r=std::from_chars(text.data()+i*2,text.data()+i*2+2,value,16);
        Require(r.ec==std::errc{} && r.ptr==text.data()+i*2+2,"Invalid checkpoint SHA-256");result[i]=uint8_t(value);}
    return result;
}
void Number(Bytes& out,uint64_t value,unsigned count){for(unsigned i=count;i;--i)out.push_back(uint8_t(value>>((i-1)*8)));}
void Text(Bytes& out,std::string_view text){out.insert(out.end(),text.begin(),text.end());}
bool SafeKey(std::string_view key) {
    if(key.empty() || key.size()>512)return false;
    for(char c:key)if(c<32 || c>=127 || (c>='A'&&c<='Z') || std::string_view("\\:<>\"|?*").find(c)!=std::string_view::npos)return false;
    unsigned depth=0;
    while(true) {
        const auto slash=key.find('/');const auto part=key.substr(0,slash);
        if(++depth>16 || part.empty() || part=="." || part==".." || part.back()=='.' || part.back()==' ')return false;
        const auto name=part.substr(0,part.find('.'));
        if(!name.empty() && (name.back()==' ' || name.back()=='.'))return false;
        if(name=="con" || name=="prn" || name=="aux" || name=="nul" || name=="conin$" || name=="conout$")return false;
        if(name.size()==4 && (name.starts_with("com") || name.starts_with("lpt")) && name[3]>='0'&&name[3]<='9')return false;
        if(slash==std::string_view::npos)return true;
        key.remove_prefix(slash+1);
    }
}
void NoReparse(const fs::path& path) {
    const auto flags=GetFileAttributesW(path.c_str());
    Require(flags!=INVALID_FILE_ATTRIBUTES && !(flags&FILE_ATTRIBUTE_REPARSE_POINT),"Checkpoint reparse/link entries are unsupported");
}
void WaitCommitRetry(std::chrono::milliseconds delay,std::stop_token stop) {
    std::mutex mutex;std::condition_variable_any condition;std::unique_lock lock(mutex);
    condition.wait_for(lock,stop,delay,[]{return false;});Cancel(stop);
}
template<class Attempt,class Wait> unsigned CommitWithRetry(Attempt attempt,Wait wait,std::stop_token stop) {
    constexpr unsigned delays[]={20,40,80,160,200,200,200};
    for(unsigned i=0;;++i) {
        Cancel(stop);const DWORD error=attempt();if(error==ERROR_SUCCESS)return i+1;
        const bool transient=error==ERROR_ACCESS_DENIED || error==ERROR_SHARING_VIOLATION || error==ERROR_LOCK_VIOLATION;
        if(!transient || i==std::size(delays))throw std::runtime_error("Checkpoint atomic commit failed (Win32 "+std::to_string(error)+", attempts "+std::to_string(i+1)+")");
        wait(std::chrono::milliseconds(delays[i]),stop);
    }
}
DWORD TryCommitDirectory(const fs::path& staging,const fs::path& target,const fs::path& parent) {
    // Keep every retry inside the same resolved parent, with new-directory
    // semantics. Never replace a directory that appeared while waiting.
    Require(parent.is_absolute() && staging.parent_path()==parent && target.parent_path()==parent && fs::canonical(parent)==parent,
        "Checkpoint commit parent changed");
    NoReparse(parent);NoReparse(staging);
    Require(!fs::exists(fs::symlink_status(target)),"Checkpoint destination already exists");
    if(MoveFileExW(staging.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))return ERROR_SUCCESS;
    return GetLastError();
}
struct Source {fs::path path;std::string key;uint64_t size;fs::file_time_type time;};
struct ReadLease {
    HANDLE handle=INVALID_HANDLE_VALUE;
    explicit ReadLease(const fs::path& path) {
        handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        Require(handle!=INVALID_HANDLE_VALUE,"Checkpoint file is in use or unreadable. Close any game using this save and try again.");
    }
    ReadLease(const ReadLease&)=delete;ReadLease& operator=(const ReadLease&)=delete;
    ReadLease(ReadLease&& other) noexcept:handle(other.handle){other.handle=INVALID_HANDLE_VALUE;}
    ~ReadLease(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
};
std::vector<Source> Inventory(const fs::path& root,std::stop_token stop) {
    NoReparse(root);Require(fs::is_directory(root),"Checkpoint source must be a directory");
    std::vector<Source> result;uint64_t total=0;
    auto visit=[&](auto&& self,const fs::path& folder,const fs::path& relative,unsigned depth)->void {
        Cancel(stop);Require(depth<16,"Checkpoint directory depth limit");
        for(const auto& entry:fs::directory_iterator(folder)) {
            Cancel(stop);NoReparse(entry.path());const auto local=relative/entry.path().filename();
            if(entry.is_directory())self(self,entry.path(),local,depth+1);
            else {
                Require(entry.is_regular_file(),"Unsupported checkpoint entry");
                auto key=local.generic_string();for(auto& c:key)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
                Require(SafeKey(key),"Unsafe checkpoint path");const auto size=entry.file_size();
                Require(size<=kMaxFile && size<=kMaxData-total && result.size()<kMaxFiles,"Checkpoint size/file-count limit");total+=size;
                result.push_back({entry.path(),key,size,entry.last_write_time()});
            }
        }
    };
    visit(visit,root,{},0);Require(!result.empty(),"Empty checkpoint");
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.key<b.key;});
    for(size_t i=1;i<result.size();++i)Require(result[i-1].key!=result[i].key,"Ambiguous checkpoint path");
    return result;
}
Hash Tree(const std::vector<std::pair<std::string,Hash>>& files) {
    Bytes tree;Text(tree,"EM2-checkpoint-files-v1");Number(tree,files.size(),8);
    for(const auto& [key,digest]:files){Number(tree,key.size(),4);Text(tree,key);tree.insert(tree.end(),digest.begin(),digest.end());}
    return Digest(tree);
}
struct Reader {
    std::span<const uint8_t> bytes;size_t at=0;
    std::span<const uint8_t> Take(size_t count){Require(count<=bytes.size()-at,"Truncated checkpoint bundle");auto value=bytes.subspan(at,count);at+=count;return value;}
    uint64_t Number(unsigned count){uint64_t value=0;for(auto b:Take(count))value=(value<<8)|b;return value;}
};
struct Entry {std::string key;std::span<const uint8_t> data;};
std::vector<Entry> Decode(std::span<const uint8_t> bytes,const Hash& expected,std::stop_token stop) {
    Cancel(stop);Require(bytes.size()<=kMaxCheckpointBundle,"Checkpoint bundle size limit");Reader reader{bytes};
    const auto magic=reader.Take(4);Require(std::equal(magic.begin(),magic.end(),"EM2C") && reader.Number(2)==1 && reader.Number(2)==0,"Unsupported checkpoint bundle");
    const auto count=reader.Number(4),total=reader.Number(8);Require(count && count<=kMaxFiles && total<=kMaxData,"Checkpoint size/file-count limit");
    const auto identity=reader.Take(32);Require(std::equal(identity.begin(),identity.end(),expected.begin()),"Checkpoint identity mismatch");
    std::vector<Entry> entries;std::vector<std::pair<std::string,Hash>> hashes;uint64_t received=0;
    for(uint64_t i=0;i<count;++i) {
        Cancel(stop);const auto length=reader.Number(2);Require(reader.Number(2)==0,"Checkpoint reserved flags");
        const auto size=reader.Number(8);Require(length && length<=512 && size<=kMaxFile && size<=total-received,"Checkpoint entry bounds");
        Hash hash{};const auto encoded_hash=reader.Take(32);std::copy(encoded_hash.begin(),encoded_hash.end(),hash.begin());
        const auto name=reader.Take(size_t(length));std::string key(name.begin(),name.end());Require(SafeKey(key),"Unsafe checkpoint path");
        Require(entries.empty() || entries.back().key<key,"Duplicate/unsorted checkpoint paths");
        for(size_t slash=key.find('/');slash!=std::string::npos;slash=key.find('/',slash+1))
            Require(std::none_of(entries.begin(),entries.end(),[&](const auto& e){return e.key==key.substr(0,slash);}),"Checkpoint file/directory collision");
        const auto data=reader.Take(size_t(size));Require(Digest(data)==hash,"Checkpoint file hash mismatch");
        entries.push_back({key,data});hashes.emplace_back(std::move(key),hash);received+=size;
    }
    Require(reader.at==bytes.size() && received==total,"Checkpoint bundle length mismatch");
    Require(Tree(hashes)==expected,"Checkpoint tree hash mismatch");Cancel(stop);return entries;
}
}
CampaignSaveSourceResult ResolveCampaignSaveSource(const fs::path& selection) {
    try {
        Require(!selection.empty(),"Choose your EM2 save folder. The current save folder is selected by default.");
        auto path=fs::absolute(selection).lexically_normal();
        // A trailing separator must not turn SAVEGAME/ into an unnamed leaf.
        if(path.filename().empty())path=path.parent_path();
        auto name_is=[](const fs::path& value,const wchar_t* name){return _wcsicmp(value.filename().c_str(),name)==0;};
        if(fs::is_regular_file(path)) {
            Require(name_is(path,L"save.dat") && name_is(path.parent_path(),L"SAVEGAME") && name_is(path.parent_path().parent_path(),L"00000001"),
                "Choose save.dat from EM2's 00000001/SAVEGAME folder, or select its save folder.");
            path=path.parent_path().parent_path().parent_path();
        }else if(name_is(path,L"SAVEGAME") && name_is(path.parent_path(),L"00000001"))path=path.parent_path().parent_path();
        else if(name_is(path,L"00000001"))path=path.parent_path();
        if(!fs::exists(path/"00000001"/"SAVEGAME"/"save.dat") && fs::exists(path/"42560816"/"00000001"/"SAVEGAME"/"save.dat"))path/="42560816";
        const auto campaign=path/"00000001"/"SAVEGAME"/"save.dat";
        Require(fs::is_regular_file(campaign),"No saved campaign found here. Start an offline game, reach a save point, close it, then try Host again.");
        NoReparse(path);NoReparse(path/"00000001");NoReparse(campaign.parent_path());NoReparse(campaign);
        const auto size=fs::file_size(campaign);
        Require(size>0,"The campaign save is empty. Create or restore a working offline save before hosting.");
        Require(size<=kMaxFile,"The campaign save exceeds the supported checkpoint file size.");
        // Do not hold a write-blocking handle in the UI. The worker acquires
        // leases for the entire source inventory before copying any bytes.
        std::ifstream readable(campaign,std::ios::binary);Require(bool(readable),"The campaign save cannot be read. Close any game using it and check the folder.");
        return {std::move(path),{}};
    }catch(const std::exception& error){return {{},error.what()};}
}
CheckpointBundleResult PackCheckpoint(const fs::path& root,std::stop_token stop) {
    try {
        const auto inventory=Inventory(root,stop);CheckpointBundle bundle;Bytes records;
        // Freeze existing files as a group, not one at a time. A writer that
        // is already open causes a clean failure; later writers/deletes cannot
        // change an earlier file while a later file is being copied.
        std::vector<ReadLease> leases;leases.reserve(inventory.size());
        for(const auto& file:inventory){Cancel(stop);leases.emplace_back(file.path);
            BY_HANDLE_FILE_INFORMATION info{};Require(GetFileInformationByHandle(leases.back().handle,&info) && !(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)),"Checkpoint entry changed or is not a regular file");}
        std::vector<std::pair<std::string,Hash>> hashes;
        for(size_t index=0;index<inventory.size();++index) {const auto& file=inventory[index];
            Cancel(stop);Bytes data(size_t(file.size));
            for(size_t at=0;at<data.size();) {
                Cancel(stop);const auto count=DWORD(std::min<size_t>(65536,data.size()-at));DWORD read=0;
                Require(ReadFile(leases[index].handle,data.data()+at,count,&read,nullptr) && read==count,"Checkpoint read failed");at+=count;
            }
            Require(fs::file_size(file.path)==file.size && fs::last_write_time(file.path)==file.time,"Checkpoint changed while reading");
            const auto hash=Digest(data);hashes.emplace_back(file.key,hash);
            Number(records,file.key.size(),2);Number(records,0,2);Number(records,file.size,8);records.insert(records.end(),hash.begin(),hash.end());
            Text(records,file.key);records.insert(records.end(),data.begin(),data.end());bundle.payload_bytes+=file.size;
        }
        const auto after=Inventory(root,stop);Require(after.size()==inventory.size(),"Checkpoint inventory changed");
        for(size_t i=0;i<after.size();++i)Require(after[i].key==inventory[i].key && after[i].size==inventory[i].size && after[i].time==inventory[i].time,"Checkpoint inventory changed");
        bundle.identity=Tree(hashes);bundle.files=inventory.size();Text(bundle.bytes,"EM2C");Number(bundle.bytes,1,2);Number(bundle.bytes,0,2);
        Number(bundle.bytes,bundle.files,4);Number(bundle.bytes,bundle.payload_bytes,8);bundle.bytes.insert(bundle.bytes.end(),bundle.identity.begin(),bundle.identity.end());
        bundle.bytes.insert(bundle.bytes.end(),records.begin(),records.end());Require(bundle.bytes.size()<=kMaxCheckpointBundle,"Checkpoint bundle size limit");
        Cancel(stop);return {std::move(bundle),{}};
    }catch(const std::exception& error){return {{},error.what()};}
}
CheckpointInstallResult InstallCheckpoint(std::span<const uint8_t> bytes,const Hash& expected,const fs::path& destination,std::stop_token stop) {
    fs::path staging,parent;bool owned=false;
    try {
        const auto entries=Decode(bytes,expected,stop);
        Require(destination.is_absolute() && !destination.filename().empty() && destination.filename()!="." && destination.filename()!="..","Checkpoint destination must be a new absolute directory");
        auto leaf=destination.filename().string();for(auto& c:leaf)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
        Require(SafeKey(leaf),"Unsafe checkpoint destination name");
        parent=fs::canonical(destination.parent_path());NoReparse(parent);
        const auto target=parent/destination.filename();Require(!fs::exists(fs::symlink_status(target)),"Checkpoint destination already exists");
        std::array<uint8_t,16> nonce{};Require(NewSessionNonce(nonce),"Checkpoint staging nonce unavailable");
        std::string suffix;constexpr char hex[]="0123456789abcdef";for(auto byte:nonce){suffix+=hex[byte>>4];suffix+=hex[byte&15];}
        staging=parent/(destination.filename().string()+".partial-"+suffix);
        Require(fs::create_directory(staging),"Cannot create checkpoint staging directory");owned=true;
        for(const auto& entry:entries) {
            Cancel(stop);const auto path=staging/fs::path(entry.key);fs::create_directories(path.parent_path());
            std::ofstream output(path,std::ios::binary|std::ios::trunc);Require(bool(output),"Cannot write checkpoint file");
            for(size_t at=0;at<entry.data.size();) {
                Cancel(stop);const auto count=std::min<size_t>(65536,entry.data.size()-at);output.write(reinterpret_cast<const char*>(entry.data.data()+at),count);
                Require(bool(output),"Checkpoint write failed");at+=count;
            }
            output.close();Require(bool(output),"Checkpoint close failed");
        }
        // Re-read the actual staged files before the atomic directory commit.
        const auto verified=PackCheckpoint(staging,stop);Require(verified.value && verified.value->identity==expected,"Installed checkpoint verification failed");
        Cancel(stop);
        CommitWithRetry([&]{return TryCommitDirectory(staging,target,parent);},WaitCommitRetry,stop);
        owned=false;return {true,{}};
    }catch(const std::exception& error) {
        if(owned) {
            std::error_code ignored;
            // Only the newly created, verified child may be recursively removed.
            if(staging.parent_path()==parent && fs::weakly_canonical(staging,ignored)==staging && !ignored)
                fs::remove_all(staging,ignored);
        }
        return {false,error.what()};
    }
}
bool WriteNewCheckpointBundle(std::span<const uint8_t> bytes,const fs::path& path) {
    if(!path.is_absolute() || bytes.size()>kMaxCheckpointBundle)return false;
    const auto file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    bool success=true;
    for(size_t at=0;at<bytes.size();) {
        const auto count=DWORD(std::min<size_t>(65536,bytes.size()-at));DWORD written=0;
        if(!WriteFile(file,bytes.data()+at,count,&written,nullptr) || written!=count){success=false;break;}at+=count;
    }
    if(!CloseHandle(file))success=false;
    if(!success)DeleteFileW(path.c_str());return success;
}
bool TestCheckpointBundles(const fs::path& root) {
    auto check=[](bool ok,const char* text){if(!ok)REXLOG_ERROR("Checkpoint bundle test: {}",text);return ok;};
    auto write=[](const fs::path& path,std::string_view text){fs::create_directories(path.parent_path());std::ofstream out(path,std::ios::binary);out.write(text.data(),text.size());};
    try {
        fs::create_directories(root);const auto source=root/"frozen";
        write(root/"game.exe","build");write(root/"content"/"configfiles.ini","Gravity=9.81\n");
        write(source/"00000001"/"SAVEGAME"/"save.dat",std::string_view("save\0state",10));
        write(source/"profile"/"User"/"63E83FFD","profile");write(source/"empty",{});
        {
            const auto expected=fs::absolute(source).lexically_normal();
            for(const auto& selected:std::vector<fs::path>{source,source/"00000001",source/"00000001"/"SAVEGAME",source/"00000001"/"SAVEGAME"/"save.dat",source/"00000001"/"SAVEGAME"/"",source/"."}) {
                const auto resolved=ResolveCampaignSaveSource(selected);
                if(!check(resolved.root==expected && resolved.error.empty(),"title/content-type/SAVEGAME/file/trailing-separator selections normalize to the title root"))return false;
            }
            const auto user=root/"user_data",title=user/"42560816";
            write(title/"00000001"/"SAVEGAME"/"save.dat","campaign");
            if(!check(ResolveCampaignSaveSource(user).root==fs::absolute(title).lexically_normal(),"a user_data selection resolves only the exact EM2 title child"))return false;
            const auto missing=root/"profile-only";
            write(missing/"profile"/"User"/"63E83FFD","profile");
            const auto no_campaign=ResolveCampaignSaveSource(missing);
            if(!check(!ResolveCampaignSaveSource({}).root && !ResolveCampaignSaveSource(root/"absent").root &&
                !no_campaign.root && no_campaign.error.find("save point")!=std::string::npos &&
                !ResolveCampaignSaveSource(source/"profile"/"User"/"63E83FFD").root,"missing/profile-only/unrelated file sources fail with campaign guidance"))return false;
            const auto invalid=root/"invalid-save",invalid_file=invalid/"00000001"/"SAVEGAME"/"save.dat";
            write(invalid_file,{});
            if(!check(!ResolveCampaignSaveSource(invalid).root,"an empty campaign save cannot start Host"))return false;
            fs::resize_file(invalid_file,kMaxFile+1);
            if(!check(!ResolveCampaignSaveSource(invalid).root,"oversized saves are rejected before hashing the installation"))return false;
            fs::resize_file(invalid_file,0);
            // Existing write access, even with permissive sharing, must stop
            // packing. Use a late-sorted file to exercise partial-lease unwind.
            const auto profile=source/"profile"/"User"/"63E83FFD";
            HANDLE writer=CreateFileW(profile.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
            struct Close {HANDLE& h;~Close(){if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);}} close{writer};
            if(!check(writer!=INVALID_HANDLE_VALUE,"open a real save-writer sharing fixture"))return false;
            const auto busy=PackCheckpoint(source);
            if(!check(!busy.value && busy.error.find("in use or unreadable")!=std::string::npos,"an open writer prevents a mixed snapshot"))return false;
            CloseHandle(writer);writer=INVALID_HANDLE_VALUE;
            const auto campaign=source/"00000001"/"SAVEGAME"/"save.dat";
            HANDLE probe=CreateFileW(campaign.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
            if(probe!=INVALID_HANDLE_VALUE)CloseHandle(probe);
            if(!check(probe!=INVALID_HANDLE_VALUE,"failed packing releases every earlier read lease"))return false;
            {
                ReadLease first(campaign),second(profile);
                HANDLE attempt=CreateFileW(campaign.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
                const auto denied=GetLastError();if(attempt!=INVALID_HANDLE_VALUE)CloseHandle(attempt);
                if(!check(attempt==INVALID_HANDLE_VALUE && denied==ERROR_SHARING_VIOLATION,"held source leases prevent a subsequent ordinary file writer"))return false;
            }
            if(!check(fs::file_size(campaign)==10 && fs::file_size(profile)==7,"source preflight and sharing failures preserve save/profile bytes"))return false;
            REXLOG_INFO("Campaign source preflight and Win32 copy leases passed: normalized source selections, profile-only/empty/oversize rejection, real writer exclusion and partial-acquisition release");
        }
        const auto packed=PackCheckpoint(source);const auto identity=FingerprintInstallation(root/"game.exe",root/"content",source);
        if(!check(packed.value && identity.value && packed.value->identity==identity.value->checkpoint && packed.value->files==3,"bundle identity agrees with installation checkpoint fingerprint"))return false;
        const auto& bundle=*packed.value;const auto target=fs::absolute(root/"received");
        {
            unsigned attempts=0,waits=0;
            const auto recovered=CommitWithRetry([&]{return ++attempts<3?ERROR_ACCESS_DENIED:ERROR_SUCCESS;},
                [&](auto,std::stop_token){++waits;},{});
            if(!check(recovered==3 && waits==2,"transient commit denials retry before publishing"))return false;
            attempts=waits=0;bool exhausted=false;
            try{CommitWithRetry([&]{++attempts;return ERROR_SHARING_VIOLATION;},[&](auto,std::stop_token){++waits;},{});}
            catch(const std::exception&){exhausted=true;}
            if(!check(exhausted && attempts==8 && waits==7,"persistent sharing denial has a bounded retry budget"))return false;
            attempts=waits=0;bool permanent=false;
            try{CommitWithRetry([&]{++attempts;return ERROR_PATH_NOT_FOUND;},[&](auto,std::stop_token){++waits;},{});}
            catch(const std::exception&){permanent=true;}
            if(!check(permanent && attempts==1 && waits==0,"permanent commit errors do not retry"))return false;
            std::stop_source cancellation;attempts=0;bool stopped=false;
            try{CommitWithRetry([&]{++attempts;return ERROR_LOCK_VIOLATION;},
                [&](auto delay,std::stop_token token){cancellation.request_stop();WaitCommitRetry(delay,token);},cancellation.get_token());}
            catch(const std::exception& error){stopped=std::string_view(error.what())=="Checkpoint operation cancelled";}
            if(!check(stopped && attempts==1,"cancellation interrupts commit backoff before another attempt"))return false;
        }
        {
            // A real Windows directory handle that denies delete sharing
            // reproduces rename denial, without guessing its external cause.
            const auto parent=fs::canonical(root),staged=parent/"locked-stage",published=parent/"locked-published";
            fs::create_directory(staged);write(staged/"save","locked-save");
            HANDLE blocker=CreateFileW(staged.c_str(),FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
            struct Close {HANDLE& handle;~Close(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}} close{blocker};
            if(!check(blocker!=INVALID_HANDLE_VALUE,"create real sharing-denial fixture"))return false;
            DWORD denial=0;
            const auto attempts=CommitWithRetry([&]{const auto error=TryCommitDirectory(staged,published,parent);if(error)denial=error;return error;},
                [&](auto delay,std::stop_token stop){CloseHandle(blocker);blocker=INVALID_HANDLE_VALUE;WaitCommitRetry(delay,stop);},{});
            if(!check(attempts>=2 && !fs::exists(staged) && fs::file_size(published/"save")==11,"real sharing denial recovers and publishes the complete directory"))return false;
            REXLOG_INFO("Checkpoint real sharing-denial test recovered Win32 {} after {} attempts",denial,attempts);
        }
        {
            const auto parent=fs::canonical(root),staged=parent/"race-stage",published=parent/"race-existing";
            fs::create_directory(staged);write(staged/"save","original");
            unsigned attempts=0;bool refused=false;
            try {
                CommitWithRetry([&]()->DWORD {if(!attempts++)return ERROR_ACCESS_DENIED;return TryCommitDirectory(staged,published,parent);},
                    [&](auto,std::stop_token){fs::create_directory(published);write(published/"sentinel","keep");},{});
            }catch(const std::exception& error){refused=std::string_view(error.what())=="Checkpoint destination already exists";}
            if(!check(refused && attempts==2 && fs::file_size(staged/"save")==8 && fs::file_size(published/"sentinel")==4,
                "a destination appearing during retry is preserved, with staging unpublished"))return false;
            REXLOG_INFO("Checkpoint commit retry tests passed: transient recovery, retry bound, permanent error, cancellation, real sharing denial and racing destination preservation");
        }
        const auto output=fs::absolute(root/"checkpoint.em2cp");
        if(!check(WriteNewCheckpointBundle(bundle.bytes,output) && !WriteNewCheckpointBundle({},output) && fs::file_size(output)==bundle.bytes.size(),"bundle persistence creates a new file and never overwrites an existing output"))return false;
        const auto installed=InstallCheckpoint(bundle.bytes,bundle.identity,target);
        const auto again=installed.installed?PackCheckpoint(target):CheckpointBundleResult{};
        if(!installed.installed)REXLOG_ERROR("Checkpoint test install error: {}",installed.error);
        else if(!again.value)REXLOG_ERROR("Checkpoint test repack error: {}",again.error);
        if(!check(installed.installed && again.value && again.value->bytes==bundle.bytes,"binary saves, canonical paths and empty files survive installation byte-for-byte"))return false;
        write(target/"sentinel","keep");
        if(!check(!InstallCheckpoint(bundle.bytes,bundle.identity,target).installed && fs::file_size(target/"sentinel")==4,"existing player directories cannot be overwritten"))return false;
        const auto empty_target=fs::absolute(root/"existing-empty");fs::create_directory(empty_target);
        if(!check(!InstallCheckpoint(bundle.bytes,bundle.identity,empty_target).installed && fs::is_empty(empty_target),"existing empty directories are also preserved"))return false;
        auto corrupt=bundle.bytes;corrupt.back()^=1;
        const auto rejected=InstallCheckpoint(corrupt,bundle.identity,fs::absolute(root/"corrupt"));
        if(!check(!rejected.installed && !fs::exists(root/"corrupt"),"corrupt data never creates an installed checkpoint"))return false;
        auto wrong=bundle.identity;wrong[0]^=1;
        if(!check(!InstallCheckpoint(bundle.bytes,wrong,fs::absolute(root/"wrong")).installed,"unexpected checkpoint identity rejected"))return false;
        for(size_t length:{size_t(0),size_t(3),size_t(51),bundle.bytes.size()-1})
            if(!check(!InstallCheckpoint(std::span(bundle.bytes).first(length),bundle.identity,fs::absolute(root/"truncated")).installed,"truncated bundles rejected"))return false;
        auto trailing=bundle.bytes;trailing.push_back(0);
        if(!check(!InstallCheckpoint(trailing,bundle.identity,fs::absolute(root/"trailing")).installed,"trailing data rejected"))return false;
        for(auto name:{"../save","/save","a\\b","a:b","con.txt","con .txt","com1 .txt","a./b","a /b","lpt1/save","a//b","A/b"})
            if(!check(!SafeKey(name),"unsafe/noncanonical Windows paths rejected"))return false;
        auto path_attack=bundle.bytes;path_attack[52+44]='/';
        const auto path_result=InstallCheckpoint(path_attack,bundle.identity,fs::absolute(root/"escape"));
        if(!check(!path_result.installed && path_result.error=="Unsafe checkpoint path" && !fs::exists(root/"escape"),"received traversal path rejected before any output"))return false;
        std::stop_source cancelled;cancelled.request_stop();
        if(!check(!PackCheckpoint(source,cancelled.get_token()).value && !InstallCheckpoint(bundle.bytes,bundle.identity,fs::absolute(root/"cancelled"),cancelled.get_token()).installed,
            "cancelled work does not publish a checkpoint"))return false;
        const auto sparse=source/"large";write(sparse,{});fs::resize_file(sparse,kMaxFile+1);
        if(!check(!PackCheckpoint(source).value,"oversized save rejected before payload allocation"))return false;
        REXLOG_INFO("Checkpoint bundle tests passed: native identity, binary roundtrip, atomic new-directory install, corruption/truncation/path bounds, cancellation and size limits");return true;
    }catch(const std::exception& error){REXLOG_ERROR("Checkpoint bundle test exception: {}",error.what());return false;}
}
}

#include "disc_image.h"
#include <windows.h>
#include <winioctl.h>
#include <bcrypt.h>
#include <cstring>
#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <vector>
#include <rex/logging.h>

namespace em2 {
namespace {
namespace fs=std::filesystem;
constexpr uint64_t sector_size=2048;
constexpr std::array<uint64_t,4> partitions={0,0x02080000,0x0FD90000,0x18300000};
constexpr char magic[]="MICROSOFT*XBOX*MEDIA";
struct Failure:std::runtime_error{using std::runtime_error::runtime_error;};
struct Canceled{};
struct Handle {HANDLE value=INVALID_HANDLE_VALUE;~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
uint16_t U16(const uint8_t* p){return uint16_t(p[0])|uint16_t(p[1])<<8;}
uint32_t U32(const uint8_t* p){return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;}
uint32_t BE32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|uint32_t(p[3]);}
void Require(bool value,const char* error){if(!value)throw Failure(error);}
void Progress(const DiscImportProgress& callback,std::wstring_view stage,uint64_t done,uint64_t total){if(callback && !callback(stage,done,total))throw Canceled{};}
struct Image {
    Handle file;uint64_t size=0;
    explicit Image(const fs::path& path) {
        // Deny writers while validating, hashing and extracting this image.
        file.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        LARGE_INTEGER length{};Require(file.value!=INVALID_HANDLE_VALUE && GetFileSizeEx(file.value,&length),"Could not open the disc image. Close any app writing it and try again.");
        size=uint64_t(length.QuadPart);Require(size>=0x10800 && size<=16ull*1024*1024*1024,"The file is not a supported Xbox 360 disc image (expected an uncompressed .iso).");
    }
    bool Contains(uint64_t offset,uint64_t length)const{return offset<=size && length<=size-offset;}
    void Read(uint64_t offset,std::span<uint8_t> bytes) {
        Require(Contains(offset,bytes.size()),"The disc image is truncated or contains an invalid file extent.");
        LARGE_INTEGER position{};position.QuadPart=LONGLONG(offset);Require(SetFilePointerEx(file.value,position,nullptr,FILE_BEGIN),"Could not seek in the disc image.");
        DWORD read=0;Require(bytes.size()<=MAXDWORD && ReadFile(file.value,bytes.data(),DWORD(bytes.size()),&read,nullptr) && read==bytes.size(),"Could not read the disc image.");
    }
};
std::string Lower(std::string text){for(auto& c:text)if(c>='A' && c<='Z')c+=32;return text;}
bool Component(const std::string& name) {
    if(name.empty() || name=="." || name==".." || name.back()=='.' || name.back()==' ' || Lower(name).starts_with(".em2-"))return false;
    for(unsigned char c:name)if(c<32 || c>=127 || std::string_view("<>:\"/\\|?*").find(char(c))!=std::string_view::npos)return false;
    auto base=Lower(name.substr(0,name.find('.')));
    if(base=="con" || base=="prn" || base=="aux" || base=="nul")return false;
    if(base.size()==4 && (base.starts_with("com") || base.starts_with("lpt")) && base[3]>='1' && base[3]<='9')return false;
    return true;
}
struct DiscFile {std::string path;uint64_t offset=0,size=0;bool directory=false;};
struct DiscPlan {uint64_t partition=0,total=0;std::vector<DiscFile> files;};
DiscPlan Plan(Image& image) {
    DiscPlan plan;std::array<uint8_t,2048> descriptor{};bool found=false;
    for(const auto base:partitions)if(image.Contains(base+0x10000,descriptor.size())) {
        image.Read(base+0x10000,descriptor);
        if(std::equal(magic,magic+20,descriptor.begin()) && std::equal(magic,magic+20,descriptor.begin()+0x7EC)){plan.partition=base;found=true;break;}
    }
    Require(found,"No supported XDVDFS game partition was found. Use an extracted Xbox 360 folder or an uncompressed XISO/XGD2/XGD3 .iso image.");
    struct Directory {uint32_t sector,size;std::string path;unsigned depth;};
    std::vector<Directory> directories{{U32(descriptor.data()+20),U32(descriptor.data()+24),{},0}};
    std::set<std::pair<uint32_t,uint32_t>> tables;std::set<std::string> paths;
    uint64_t directory_bytes=0;
    while(!directories.empty()) {
        auto directory=std::move(directories.back());directories.pop_back();
        Require(directory.depth<=64,"The image has too many nested directories.");
        if(!directory.size)continue;
        Require(directory.size<=16*1024*1024 && (directory_bytes+=directory.size)<=64*1024*1024,"The image's directory tables exceed safe limits.");
        Require(tables.emplace(directory.sector,directory.size).second,"The image contains a repeated/cyclic directory table.");
        std::vector<uint8_t> table(directory.size);image.Read(plan.partition+uint64_t(directory.sector)*sector_size,table);
        std::vector<uint32_t> pending{0};std::map<uint32_t,uint32_t> occupied;
        while(!pending.empty()) {
            const auto offset=pending.back();pending.pop_back();
            Require(offset<=table.size() && table.size()-offset>=14,"An image directory entry points outside its table.");
            const auto* entry=table.data()+offset;const uint32_t length=14+entry[13];
            Require(entry[13] && length<=table.size()-offset,"An image directory entry has an invalid name length.");
            auto next=occupied.lower_bound(offset);
            Require((next==occupied.end() || offset+length<=next->first) && (next==occupied.begin() || std::prev(next)->second<=offset),"The image contains overlapping/cyclic directory entries.");
            occupied.emplace(offset,offset+length);
            const std::string name(reinterpret_cast<const char*>(entry+14),entry[13]);Require(Component(name),"The image contains an unsafe or unsupported filename.");
            const auto path=directory.path.empty()?name:directory.path+"/"+name;
            Require(path.size()<=2048 && paths.insert(Lower(path)).second && paths.size()<=100000,"The image contains duplicate paths or too many files.");
            const bool is_directory=(entry[12]&0x10)!=0;
            const auto size=U32(entry+8);const uint64_t data=plan.partition+uint64_t(U32(entry+4))*sector_size;
            Require(image.Contains(data,size),"The disc image contains a file outside the image.");
            plan.files.push_back({path,data,size,is_directory});
            if(is_directory)directories.push_back({U32(entry+4),size,path,directory.depth+1});
            else {plan.total+=size;Require(plan.total<=image.size*2,"The image has an excessive expanded data size.");}
            for(unsigned side=0;side<2;++side){const auto child=uint32_t(U16(entry+side*2))*4;if(child)pending.push_back(child);}
        }
    }
    auto find=[&](std::string_view path)->const DiscFile* {const auto it=std::find_if(plan.files.begin(),plan.files.end(),[&](const auto& file){return Lower(file.path)==path;});return it==plan.files.end()?nullptr:&*it;};
    const auto* executable=find("default.xex");const auto* config=find("configfiles.ini");const auto* packs=find("packfiles");
    Require(executable && !executable->directory && config && !config->directory && packs && packs->directory,"This image does not contain the required Epic Mickey 2 game files.");
    Require(executable->size>=32,"The image's default.xex is invalid.");
    std::array<uint8_t,24> header{};image.Read(executable->offset,header);
    Require(std::equal(header.begin(),header.begin()+4,"XEX2"),"This image does not contain an Xbox 360 XEX2 executable.");
    const auto count=BE32(header.data()+20);Require(count<=1024 && uint64_t(count)*8<=executable->size-24,"The XEX optional header table is invalid.");
    std::vector<uint8_t> optional(size_t(count)*8);image.Read(executable->offset+24,optional);bool title=false;
    for(uint32_t i=0;i<count;++i)if(BE32(optional.data()+8*i)==0x00040006) {
        const auto position=BE32(optional.data()+8*i+4);Require(position<=executable->size && 24<=executable->size-position,"The XEX execution information is invalid.");
        std::array<uint8_t,24> info{};image.Read(executable->offset+position,info);title=BE32(info.data()+12)==0x42560816;break;
    }
    Require(title,"The selected disc is not Epic Mickey 2 for Xbox 360 (title 42560816).");return plan;
}
struct Digest {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;std::vector<uint8_t> object;
    Digest(){Require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"SHA-256 initialization failed.");DWORD length=0,read=0;Require(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),sizeof(length),&read,0)>=0,"SHA-256 initialization failed.");object.resize(length);Require(BCryptCreateHash(algorithm,&hash,object.data(),length,nullptr,0,0)>=0,"SHA-256 initialization failed.");}
    ~Digest(){if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
    void Add(std::span<uint8_t> bytes){Require(BCryptHashData(hash,bytes.data(),ULONG(bytes.size()),0)>=0,"Disc image hashing failed.");}
    std::string Finish(){std::array<uint8_t,32> value{};Require(BCryptFinishHash(hash,value.data(),ULONG(value.size()),0)>=0,"Disc image hashing failed.");std::string result;for(auto byte:value){result+="0123456789abcdef"[byte>>4];result+="0123456789abcdef"[byte&15];}return result;}
};
void SafeDirectory(const fs::path& root,const fs::path& relative) {
    fs::path current=root;
    for(const auto& component:relative) {
        current/=component;const DWORD attributes=GetFileAttributesW(current.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES){Require(CreateDirectoryW(current.c_str(),nullptr),"Could not create an extraction directory. Check folder permissions.");}
        else Require((attributes&FILE_ATTRIBUTE_DIRECTORY) && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Extraction cannot use redirected/junction directories inside its destination.");
    }
}
bool ExistingMatches(Image& image,const DiscFile& file,const fs::path& destination,std::vector<uint8_t>& buffer) {
    const auto attributes=GetFileAttributesW(destination.c_str());if(attributes==INVALID_FILE_ATTRIBUTES)return false;
    Require(!(attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)),"An extraction file conflicts with an existing directory or link.");
    Handle input;input.value=CreateFileW(destination.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    LARGE_INTEGER size{};Require(input.value!=INVALID_HANDLE_VALUE && GetFileSizeEx(input.value,&size) && uint64_t(size.QuadPart)==file.size,"An existing extracted file differs from the selected image. Use a fresh recomp folder.");
    std::vector<uint8_t> existing(buffer.size());
    for(uint64_t offset=0;offset<file.size;) {
        const auto count=size_t(std::min<uint64_t>(buffer.size(),file.size-offset));image.Read(file.offset+offset,std::span(buffer).first(count));DWORD read=0;
        Require(ReadFile(input.value,existing.data(),DWORD(count),&read,nullptr) && read==count && std::equal(buffer.begin(),buffer.begin()+count,existing.begin()),"An existing extracted file differs from the selected image. Use a fresh recomp folder.");offset+=count;
    }
    return true;
}
}
bool ImportXbox360Disc(const fs::path& path,const fs::path& destination,const DiscImportProgress& progress,std::string& error) {
    try {
        error.clear();Image image(path);auto plan=Plan(image);std::vector<uint8_t> buffer(1024*1024);Digest digest;
        for(uint64_t offset=0;offset<image.size;) {Progress(progress,L"Verifying the disc image",offset,image.size);const auto count=size_t(std::min<uint64_t>(buffer.size(),image.size-offset));auto bytes=std::span(buffer).first(count);image.Read(offset,bytes);digest.Add(bytes);offset+=count;}
        const auto identity=digest.Finish();
        if(!fs::exists(destination))fs::create_directories(destination);
        const auto attributes=GetFileAttributesW(destination.c_str());Require(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_DIRECTORY) && !(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"Choose a normal writable folder for extracted game data.");
        const auto metadata=destination/".em2-iso-import";
        if(!fs::exists(metadata/"identity.txt"))for(const auto& entry:fs::directory_iterator(destination))Require(entry.path().filename()==L".em2-iso-import","The extraction destination already contains unrelated files. Use a fresh recomp release folder.");
        SafeDirectory(destination,L".em2-iso-import");
        const auto receipt=metadata/"identity.txt";
        if(fs::exists(receipt)) {
            Require(!(GetFileAttributesW(receipt.c_str())&FILE_ATTRIBUTE_REPARSE_POINT),"Extraction identity cannot be a redirected file.");
            std::ifstream input(receipt,std::ios::binary);std::array<char,65> previous{};input.read(previous.data(),previous.size());
            Require(input.gcount()==previous.size() && std::string_view(previous.data(),64)==identity && previous[64]=='\n' && input.peek()==EOF,"An unfinished extraction belongs to a different disc image. Resume with that image, or use a fresh recomp folder.");
        } else {
            Handle output;output.value=CreateFileW(receipt.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);DWORD written=0;
            const auto text=identity+'\n';Require(output.value!=INVALID_HANDLE_VALUE && WriteFile(output.value,text.data(),DWORD(text.size()),&written,nullptr) && written==text.size() && FlushFileBuffers(output.value),"Could not save extraction progress.");
        }
        ULARGE_INTEGER available{};Require(GetDiskFreeSpaceExW(destination.c_str(),&available,nullptr,nullptr),"Could not check extraction disk space.");
        uint64_t needed=0;for(const auto& file:plan.files)if(!file.directory && !fs::exists(destination/fs::path(file.path)))needed+=file.size;
        Require(needed<=available.QuadPart,"Not enough free space to extract the game. Move the recomp folder to a drive with more space.");
        uint64_t completed=0,index=0;
        for(const auto& file:plan.files) {
            ++index;const fs::path relative=file.path;
            if(file.directory){SafeDirectory(destination,relative);continue;}
            SafeDirectory(destination,relative.parent_path());
            const auto target=destination/relative;Progress(progress,L"Extracting game files",completed,plan.total);
            if(ExistingMatches(image,file,target,buffer)){completed+=file.size;continue;}
            const auto temporary=metadata/(std::to_wstring(index)+L".part");
            // Remove only our owned partial pathname, then create exclusively.
            // Never truncate an existing file/hard link supplied by another app.
            if(GetFileAttributesW(temporary.c_str())!=INVALID_FILE_ATTRIBUTES)Require(DeleteFileW(temporary.c_str()),"Could not replace an owned partial extraction file.");
            Handle output;output.value=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);Require(output.value!=INVALID_HANDLE_VALUE,"Could not create an extracted game file.");
            for(uint64_t offset=0;offset<file.size;) {
                Progress(progress,L"Extracting game files",completed+offset,plan.total);const auto count=size_t(std::min<uint64_t>(buffer.size(),file.size-offset));image.Read(file.offset+offset,std::span(buffer).first(count));DWORD written=0;
                Require(WriteFile(output.value,buffer.data(),DWORD(count),&written,nullptr) && written==count,"Could not write game data. Check free space and permissions.");offset+=count;
            }
            Require(FlushFileBuffers(output.value),"Could not finish writing game data.");CloseHandle(output.value);output.value=INVALID_HANDLE_VALUE;
            Require(MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH),"Could not publish an extracted game file. No existing file was replaced.");completed+=file.size;
        }
        Progress(progress,L"Game files ready",completed,plan.total);return true;
    }catch(const Canceled&){error.clear();return false;}catch(const std::exception& exception){error=exception.what();return false;}
}

bool TestDiscImageImport(const fs::path& root) {
    try {
        Require(!fs::exists(root),"Disc image fixture directory already exists.");fs::create_directories(root);
        auto fixture=[&](const fs::path& path,uint64_t base,int invalid) {
            std::vector<uint8_t> image(47*2048,0);
            auto word=[&](size_t at,uint32_t value){for(unsigned i=0;i<4;++i)image[at+i]=uint8_t(value>>(8*i));};
            auto big=[&](size_t at,uint32_t value){for(unsigned i=0;i<4;++i)image[at+i]=uint8_t(value>>(24-8*i));};
            std::copy(magic,magic+20,image.begin()+0x10000);std::copy(magic,magic+20,image.begin()+0x107EC);word(0x10014,40);word(0x10018,2048);
            auto entry=[&](size_t table,size_t at,const char* name,uint32_t sector,uint32_t size,bool directory,uint16_t right) {
                const size_t p=table*2048+at;image[p+2]=uint8_t(right);image[p+3]=uint8_t(right>>8);word(p+4,sector);word(p+8,size);image[p+12]=directory?0x10:0x80;image[p+13]=uint8_t(strlen(name));std::copy(name,name+strlen(name),image.begin()+p+14);
            };
            entry(40,0,invalid==1?"../escape":"ConfigFiles.ini",43,6,false,8);entry(40,32,"default.xex",44,80,false,16);entry(40,64,"packfiles",42,2048,true,invalid==2?16:0);entry(42,0,"level.pak",46,10,false,0);
            std::copy_n("config",6,image.begin()+43*2048);std::copy_n("XEX2",4,image.begin()+44*2048);big(44*2048+20,1);big(44*2048+24,0x00040006);big(44*2048+28,48);big(44*2048+60,invalid==3?0x12345678:0x42560816);
            std::copy_n("level-data",10,image.begin()+46*2048);
            Handle output;output.value=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);Require(output.value!=INVALID_HANDLE_VALUE,"Could not create ISO fixture.");DWORD ignored=0;DeviceIoControl(output.value,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&ignored,nullptr);
            LARGE_INTEGER start{};start.QuadPart=LONGLONG(base);Require(SetFilePointerEx(output.value,start,nullptr,FILE_BEGIN),"Could not seek ISO fixture.");DWORD written=0;Require(WriteFile(output.value,image.data(),DWORD(image.size()),&written,nullptr) && written==image.size(),"Could not write ISO fixture.");
        };
        for(size_t i=0;i<partitions.size();++i){const auto path=root/("layout"+std::to_string(i)+".iso");fixture(path,partitions[i],0);Image image(path);Require(Plan(image).partition==partitions[i],"XDVDFS partition offset fixture failed.");}
        for(int bad=1;bad<=3;++bad){const auto path=root/("invalid"+std::to_string(bad)+".iso");fixture(path,0,bad);bool rejected=false;try{Image image(path);Plan(image);}catch(const Failure&){rejected=true;}Require(rejected,"Malformed/cyclic/wrong-title ISO was accepted.");}
        const auto source=root/"layout0.iso",target=root/"extracted";std::string error;
        Require(!ImportXbox360Disc(source,target,[](std::wstring_view stage,uint64_t done,uint64_t){return stage!=L"Extracting game files" || done==0;},error) && error.empty(),"ISO cancellation failed.");
        Require(!ImportXbox360Disc(root/"layout1.iso",target,{},error) && !error.empty(),"A different image resumed an owned partial extraction.");
        Require(ImportXbox360Disc(source,target,{},error) && ImportXbox360Disc(source,target,{},error),"ISO extraction/resume failed.");
        std::ifstream stream(target/"packfiles"/"level.pak",std::ios::binary);Require(std::string(std::istreambuf_iterator<char>(stream),{})=="level-data","Extracted ISO contents differ.");
        Require(!fs::exists(root/"escape"),"Unsafe ISO path escaped extraction.");
        REXLOG_INFO("Native ISO importer passed: four XDVDFS partition bases, traversal/cycle/title rejection, streaming extraction/cancel, SHA-256-owned resume and byte-exact files (synthetic images; retail ISO still needs validation)");return true;
    }catch(const std::exception& exception){REXLOG_ERROR("Disc image fixture: {}",exception.what());return false;}
}
}

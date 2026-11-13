#include "disc_image.h"
#include "em1_inflate.h"
#include "em1_deflate.h"
#include "svod_image.h"
#include "zar_image.h"
#include <windows.h>
#include <winioctl.h>
#include <bcrypt.h>
#include <cstring>
#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <vector>
#include <rex/logging.h>
extern "C" {
void* bw_chd_open(const uint8_t*,size_t,uint64_t*,uint8_t*,size_t);
void bw_chd_close(void*);
int bw_chd_read(void*,uint64_t,uint8_t*,size_t,uint8_t*,size_t);
}

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
uint64_t U64(const uint8_t* p){return uint64_t(U32(p))|uint64_t(U32(p+4))<<32;}
uint32_t BE32(const uint8_t* p){return uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|uint32_t(p[3]);}
void Require(bool value,const char* error){if(!value)throw Failure(error);}
void Progress(const DiscImportProgress& callback,std::wstring_view stage,uint64_t done,uint64_t total){if(callback && !callback(stage,done,total))throw Canceled{};}
struct Image {
    Handle file;uint64_t size=0,physical_size=0;unsigned version=0,shift=0;bool compressed=false,lz4=false,cci=false;
    std::vector<std::unique_ptr<Image>> parts;std::vector<uint64_t> part_starts;
    std::unique_ptr<void,void(*)(void*)> chd{nullptr,bw_chd_close};
    std::unique_ptr<SvodImage> svod;
    std::unique_ptr<ZarImage> zar;uint64_t zar_base=0;bool zar_tree=false;
    uint32_t block_size=0,cached=UINT32_MAX;std::vector<uint32_t> index;std::vector<uint8_t> cache;
    void Physical(uint64_t offset,std::span<uint8_t> bytes){
        Require(offset<=physical_size && bytes.size()<=physical_size-offset,"Compressed image block extends outside its container");
        LARGE_INTEGER position{};position.QuadPart=LONGLONG(offset);Require(SetFilePointerEx(file.value,position,nullptr,FILE_BEGIN),"Could not seek in the disc image");
        DWORD read=0;Require(bytes.size()<=MAXDWORD && ReadFile(file.value,bytes.data(),DWORD(bytes.size()),&read,nullptr) && read==bytes.size(),"Could not read disc image bytes");
    }
    uint64_t Position(size_t i)const{return uint64_t(index[i]&0x7FFFFFFF)<<shift;}
    explicit Image(const fs::path& path,bool discover_parts=true,const DiscImportProgress& progress={}) {
        // Deny writers while validating, hashing and extracting this image.
        file.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        LARGE_INTEGER length{};Require(file.value!=INVALID_HANDLE_VALUE && GetFileSizeEx(file.value,&length),"Could not open the disc image. Close any app writing it and try again.");
        size=physical_size=uint64_t(length.QuadPart);Require(size>=24 && size<=16ull*1024*1024*1024,"Invalid Xbox disc image size");
        std::array<uint8_t,24> header{};Physical(0,header);
        std::array<uint8_t,8> trailer{};Physical(physical_size-8,trailer);
        if((BE32(trailer.data())==0x61bf3a01 && BE32(trailer.data()+4)==0x169f52d6) || CompareStringOrdinal(path.extension().c_str(),-1,L".zar",-1,TRUE)==CSTR_EQUAL){
            std::string error;zar=ZarImage::Open(path,error,[&](uint64_t done,uint64_t total){return !progress || progress(L"Verifying archive integrity",done,total);});Require(bool(zar),error.c_str());
            size=zar->Size();zar_tree=true;const ZarFile* single=nullptr;size_t count=0;for(const auto& entry:zar->Files())if(!entry.directory){single=&entry;++count;}
            if(count==1){auto extension=fs::path(single->path).extension().wstring();if(CompareStringOrdinal(extension.c_str(),-1,L".iso",-1,TRUE)==CSTR_EQUAL || CompareStringOrdinal(extension.c_str(),-1,L".xiso",-1,TRUE)==CSTR_EQUAL || CompareStringOrdinal(extension.c_str(),-1,L".img",-1,TRUE)==CSTR_EQUAL || CompareStringOrdinal(extension.c_str(),-1,L".bin",-1,TRUE)==CSTR_EQUAL){zar_tree=false;zar_base=single->offset;size=single->size;}}
            return;
        }
        if(std::equal(header.begin(),header.begin()+4,"LIVE") || std::equal(header.begin(),header.begin()+4,"PIRS") || std::equal(header.begin(),header.begin()+4,"CON ")){
            std::string error;svod=SvodImage::Open(path,error);Require(bool(svod),error.c_str());size=svod->Size();return;
        }
        if(std::equal(header.begin(),header.begin()+8,"MComprHD")){
            const auto encoded=path.u8string();std::array<uint8_t,1024> error{};
            chd.reset(bw_chd_open(reinterpret_cast<const uint8_t*>(encoded.data()),encoded.size(),&size,error.data(),error.size()));
            Require(bool(chd),reinterpret_cast<const char*>(error.data()));compressed=true;return;
        }
        cci=std::equal(header.begin(),header.begin()+4,"CCIM");
        if(cci){
            std::array<uint8_t,32> full{};Physical(0,full);compressed=lz4=true;version=1;shift=2;block_size=2048;size=U64(full.data()+8);
            Require(U32(full.data()+4)==32 && U32(full.data()+24)==2048 && full[28]==1 && full[29]==2 && !U16(full.data()+30) &&
                size>0 && size<=16ull*1024*1024*1024 && !(size%2048),"Unsupported CCI header or logical size");
            const auto table=U64(full.data()+16),count=size/2048+1;
            Require(table>=32 && table<=physical_size && count*4<=64*1024*1024 && count*4==physical_size-table,"Invalid CCI index bounds");
            std::vector<uint8_t> bytes(size_t(count*4));Physical(table,bytes);index.resize(size_t(count));
            for(size_t i=0;i<index.size();++i)index[i]=U32(bytes.data()+i*4);
            Require(Position(0)==32 && Position(index.size()-1)==table,"CCI blocks do not match the header/index boundaries");
            for(size_t i=1;i<index.size();++i){const auto span=Position(i)-Position(i-1);
                Require(Position(i)>Position(i-1) && Position(i)<=table && ((index[i-1]&0x80000000u)?(span>=4 && span<=2048):(span==2048)),"Invalid CCI sector extent");}
            if(discover_parts){
                const auto stem=path.stem().wstring();const auto dot=stem.rfind(L'.');
                if(dot!=std::wstring::npos && dot+1<stem.size() && std::all_of(stem.begin()+dot+1,stem.end(),[](wchar_t c){return c>=L'0' && c<=L'9';})){
                    const auto number=stem.substr(dot+1);Require(number.size()<=2 && number[0]!=L'0',"Invalid CCI part number");const auto chosen=std::stoul(number);
                    Require(chosen<=32,"CCI has too many parts");const auto base=stem.substr(0,dot);bool gap=false;uint64_t total=0;
                    Require(!fs::exists(path.parent_path()/(base+L".33"+path.extension().wstring())),"CCI split image exceeds the 32-part limit");
                    for(unsigned n=1;n<=32;++n){const auto sibling=path.parent_path()/(base+L'.'+std::to_wstring(n)+path.extension().wstring());
                        if(!fs::exists(sibling)){gap=true;continue;}Require(!gap,"CCI split image has a missing part; keep all numbered parts together");
                        auto part=std::make_unique<Image>(sibling,false);Require(part->cci,"CCI split image contains a different container format");
                        Require(part->size<=16ull*1024*1024*1024-total,"CCI split image is too large");part_starts.push_back(total);total+=part->size;parts.push_back(std::move(part));
                    }
                    Require(parts.size()>=chosen,"CCI split image is missing its selected part");size=total;
                    CloseHandle(file.value);file.value=INVALID_HANDLE_VALUE;index.clear();
                }
            }
            return;
        }
        compressed=std::equal(header.begin(),header.begin()+4,"CISO") || std::equal(header.begin(),header.begin()+4,"ZISO");
        if(!compressed){Require(size>=0x10800,"The disc image is too short");return;}
        lz4=header[0]=='Z';version=header[20];shift=header[21];block_size=U32(header.data()+16);size=U64(header.data()+8);
        const auto start=U32(header.data()+4)?U32(header.data()+4):24;
        Require(version<=2 && (!lz4 || version<=1) && shift<=16 && block_size>=2048 && block_size<=1024*1024 && !(block_size&(block_size-1)) &&
            size>=0x10800 && size<=16ull*1024*1024*1024 && !(size%2048) && start>=24 && start<=4096,"Unsupported CSO/ZSO header, version or block size");
        const auto count=(size+block_size-1)/block_size+1;Require(count*4<=64*1024*1024 && start+count*4<=physical_size,"Invalid CSO block index bounds");
        std::vector<uint8_t> bytes(size_t(count*4));Physical(start,bytes);index.resize(size_t(count));
        for(size_t i=0;i<index.size();++i)index[i]=U32(bytes.data()+i*4);
        Require(Position(0)>=start+count*4,"CSO payload overlaps its index");
        for(size_t i=1;i<index.size();++i)Require(Position(i)>Position(i-1) && Position(i)<=physical_size &&
            Position(i)-Position(i-1)<=uint64_t(block_size)*2+(uint64_t(1)<<shift),"Invalid CSO block extent or nonmonotonic index");
    }
    bool Contains(uint64_t offset,uint64_t length)const{return offset<=size && length<=size-offset;}
    void Read(uint64_t offset,std::span<uint8_t> bytes) {
        Require(Contains(offset,bytes.size()),"The disc image is truncated or contains an invalid file extent.");
        if(zar){std::string error;const auto result=zar->Read(zar_base+offset,bytes,error);Require(result,error.c_str());return;}
        if(svod){std::string error;const auto result=svod->Read(offset,bytes,error);Require(result,error.c_str());return;}
        if(chd){std::array<uint8_t,1024> error{};const auto result=bw_chd_read(chd.get(),offset,bytes.data(),bytes.size(),error.data(),error.size());Require(result==0,reinterpret_cast<const char*>(error.data()));return;}
        if(!parts.empty()){size_t copied=0;while(copied<bytes.size()){
            const auto i=size_t(std::upper_bound(part_starts.begin(),part_starts.end(),offset)-part_starts.begin()-1);
            const auto local=offset-part_starts[i],count=size_t(std::min<uint64_t>(bytes.size()-copied,parts[i]->size-local));
            parts[i]->Read(local,bytes.subspan(copied,count));offset+=count;copied+=count;
        }return;}
        if(compressed){size_t copied=0;while(copied<bytes.size()){
            const auto block=uint32_t(offset/block_size);const auto within=size_t(offset%block_size);
            if(cached!=block){const auto length=size_t(Position(block+1)-Position(block));const auto minimum=size_t(std::min<uint64_t>(block_size,size-uint64_t(block)*block_size));
                const bool plain=cci?!(index[block]&0x80000000u):version==2?length>=block_size:bool(index[block]&0x80000000u);
                cached=UINT32_MAX;cache.assign(block_size,0);
                if(plain){Require(length>=minimum,"Short uncompressed CSO block");Physical(Position(block),std::span(cache).first(minimum));cache.resize(minimum);}
                else{std::vector<uint8_t> encoded(length);Physical(Position(block),encoded);size_t produced=0;std::string error;
                    const bool use_lz4=lz4 || (version==2 && (index[block]&0x80000000u));
                    if(cci){const auto pad=encoded[0];Require(pad<=3 && encoded.size()>size_t(pad)+1,"Invalid CCI block padding");
                        Require(std::all_of(encoded.end()-pad,encoded.end(),[](uint8_t b){return b==0;}),"Nonzero CCI padding");
                        produced=DecodeLz4(std::span(encoded).subspan(1,encoded.size()-1-pad),cache,minimum,0);Require(produced==2048,"CCI sector did not decode to 2048 bytes");}
                    else if(use_lz4)produced=DecodeLz4(encoded,cache,minimum,(size_t(1)<<shift)-1);
                    else{const auto decoded=em1::InflateDiscBlock(encoded,cache,minimum,(size_t(1)<<shift)-1,produced,error);Require(decoded,error.c_str());}
                    cache.resize(produced);
                }cached=block;
            }
            Require(within<cache.size(),"Missing decompressed CSO bytes");const auto count=std::min(bytes.size()-copied,cache.size()-within);
            std::copy_n(cache.begin()+within,count,bytes.begin()+copied);copied+=count;offset+=count;
        }return;}
        LARGE_INTEGER position{};position.QuadPart=LONGLONG(offset);Require(SetFilePointerEx(file.value,position,nullptr,FILE_BEGIN),"Could not seek in the disc image.");
        DWORD read=0;Require(bytes.size()<=MAXDWORD && ReadFile(file.value,bytes.data(),DWORD(bytes.size()),&read,nullptr) && read==bytes.size(),"Could not read the disc image.");
    }
    static size_t DecodeLz4(std::span<const uint8_t> input,std::span<uint8_t> output,size_t minimum,size_t padding){
        size_t at=0,written=0;
        auto length=[&](size_t n){if(n==15){uint8_t extra;do{Require(at<input.size(),"Truncated LZ4 length");extra=input[at++];Require(n<=output.size() && extra<=output.size()-n,"Oversized LZ4 length");n+=extra;}while(extra==255);}return n;};
        while(at<input.size()){
            if(written>=minimum && input.size()-at<=padding)return written;
            const auto token=input[at++];const auto literals=length(token>>4);
            Require(literals<=input.size()-at && literals<=output.size()-written,"LZ4 literal exceeds disc block");std::copy_n(input.begin()+at,literals,output.begin()+written);at+=literals;written+=literals;
            if(at==input.size() || (written>=minimum && input.size()-at<=padding)){Require(written>=minimum,"Short LZ4 disc block");return written;}
            Require(input.size()-at>=2,"Truncated LZ4 match");const auto distance=U16(input.data()+at);at+=2;const auto match=length(token&15)+4;
            Require(distance && distance<=written && match<=output.size()-written,"Invalid LZ4 match distance/length");
            for(size_t i=0;i<match;++i)output[written+i]=output[written+i-distance];written+=match;
        }Require(written>=minimum,"Incomplete LZ4 disc block");return written;
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
    if(image.zar && image.zar_tree){
        const auto& entries=image.zar->Files();std::set<std::string> names;for(const auto& entry:entries){size_t at=0;while(at<entry.path.size()){auto end=entry.path.find('/',at);if(end==std::string::npos)end=entry.path.size();Require(Component(entry.path.substr(at,end-at)),"Unsafe filename in archived Xbox game");at=end+1;}names.insert(Lower(entry.path));}
        std::vector<std::string> roots;
        for(const auto& entry:entries)if(!entry.directory){const auto path=Lower(entry.path);const auto slash=path.rfind('/');const auto base=slash==std::string::npos?std::string{}:path.substr(0,slash+1);if(path.substr(base.size())=="default.xex" && names.contains(base+"configfiles.ini") && names.contains(base+"packfiles"))roots.push_back(base);}
        Require(roots.size()==1,"ZArchive must contain one EM2 game folder, or one raw ISO file");
        for(const auto& entry:entries)if(Lower(entry.path).starts_with(roots[0]) && entry.path.size()>roots[0].size()){
            plan.files.push_back({entry.path.substr(roots[0].size()),entry.offset,entry.size,entry.directory});if(!entry.directory){plan.total+=entry.size;Require(plan.total<=image.size*2,"Archived game has an excessive expanded data size");}}
    }else{
    if(image.svod){descriptor=image.svod->Descriptor();found=true;}
    else for(const auto base:partitions)if(image.Contains(base+0x10000,descriptor.size())) {
        image.Read(base+0x10000,descriptor);
        if(std::equal(magic,magic+20,descriptor.begin()) && std::equal(magic,magic+20,descriptor.begin()+0x7EC)){plan.partition=base;found=true;break;}
    }
    Require(found,"No supported Xbox game filesystem was found. Select an EM2 Xbox 360 image, GOD package header, or extracted game folder.");
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
    void Add(std::span<const uint8_t> bytes){Require(BCryptHashData(hash,const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),0)>=0,"Disc image hashing failed.");}
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
        error.clear();Image image(path,true,progress);auto plan=Plan(image);std::vector<uint8_t> buffer(1024*1024);Digest digest;
        if(image.svod)digest.Add(image.svod->IdentityPrefix());
        if(image.zar)digest.Add(image.zar->Identity());
        else for(uint64_t offset=0;offset<image.size;) {Progress(progress,L"Verifying the disc image",offset,image.size);const auto count=size_t(std::min<uint64_t>(buffer.size(),image.size-offset));auto bytes=std::span(buffer).first(count);image.Read(offset,bytes);digest.Add(bytes);offset+=count;}
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

bool VerifyXbox360Disc(const fs::path& path,const fs::path& reference,std::string& error) {
    try {
        error.clear();Image image(path);const auto plan=Plan(image);std::vector<uint8_t> buffer(1024*1024);
        Require(fs::is_directory(reference),"Choose an existing extracted reference folder");
        REXLOG_INFO("Read-only Xbox disc comparison: {} entries, {} file bytes",plan.files.size(),plan.total);
        uint64_t bytes=0,files=0;
        for(const auto& file:plan.files){
            if(file.directory)continue;
            try{Require(ExistingMatches(image,file,reference/fs::path(file.path),buffer),"Reference file is missing");}
            catch(const std::exception& problem){throw Failure(file.path+": "+problem.what());}
            bytes+=file.size;++files;
        }
        REXLOG_INFO("Read-only Xbox disc comparison passed: {} files, {} bytes; partition {:#x}; compressed {}",files,bytes,plan.partition,image.compressed);
        return true;
    }catch(const std::exception& problem){error=problem.what();REXLOG_ERROR("Xbox disc comparison: {}",error);return false;}
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
        Require(VerifyXbox360Disc(source,target,error),"Read-only reference verification failed");
        const auto absent=root/"absent-reference";
        Require(!VerifyXbox360Disc(source,absent,error) && !fs::exists(absent),"Reference verification created a missing folder");
        std::ifstream raw_file(source,std::ios::binary);const std::vector<uint8_t> raw(std::istreambuf_iterator<char>(raw_file),{});
        auto compressed_fixture=[&](const fs::path& path,unsigned version,unsigned shift,bool zso,bool lz4){
            constexpr size_t block=4096;const auto count=(raw.size()+block-1)/block;
            std::vector<uint8_t> cso(24+(count+1)*4,0);
            auto word=[&](size_t at,uint32_t n){for(unsigned i=0;i<4;++i)cso[at+i]=uint8_t(n>>(8*i));};
            std::copy_n(zso?"ZISO":"CISO",4,cso.begin());word(4,version==1 && !zso?0:24);word(8,uint32_t(raw.size()));word(16,block);cso[20]=uint8_t(version);cso[21]=uint8_t(shift);
            const auto alignment=size_t(1)<<shift;cso.resize((cso.size()+alignment-1)&~(alignment-1));
            for(size_t b=0;b<count;++b){std::vector<uint8_t> bytes(block,0);std::copy_n(raw.begin()+b*block,std::min(block,raw.size()-b*block),bytes.begin());std::vector<uint8_t> packed;
                if(lz4 || zso){
                    if(std::all_of(bytes.begin(),bytes.end(),[&](auto v){return v==bytes[0];})){
                        packed={0x1f,bytes[0],1,0};size_t extra=block-25;while(extra>=255){packed.push_back(255);extra-=255;}packed.push_back(uint8_t(extra));packed.push_back(0x50);packed.insert(packed.end(),5,bytes[0]);
                    }
                }else{const auto zlib=em1::DeflateNativePakResource(bytes);packed.assign(zlib.begin()+2,zlib.end()-4);}
                // Include raw blocks too; v2 identifies them by stored extent.
                const bool plain=packed.empty() || packed.size()+alignment>=block || b%3==0;
                const auto flags=version==2?((lz4 && !plain)?0x80000000u:0u):(plain?0x80000000u:0u);
                word(24+b*4,uint32_t(cso.size()>>shift)|flags);const auto& data=plain?bytes:packed;cso.insert(cso.end(),data.begin(),data.end());cso.resize((cso.size()+alignment-1)&~(alignment-1));
            }
            word(24+count*4,uint32_t(cso.size()>>shift));std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(cso.data()),cso.size());
        };
        for(unsigned mode=0;mode<4;++mode){const auto path=root/("compressed"+std::to_string(mode)+(mode==0?".img":mode==3?".zso":".cso"));
            compressed_fixture(path,mode==0 || mode==3?1:2,mode==2?9:mode==1?2:0,mode==3,mode>=2);
            Image compressed(path);std::vector<uint8_t> decoded(raw.size());compressed.Read(0,decoded);
            Require(decoded==raw && Plan(compressed).partition==0,"CSO/ZSO block decoding changed logical ISO bytes or file tables");
            std::array<uint8_t,4125> crossing{};compressed.Read(2040,crossing);Require(std::equal(crossing.begin(),crossing.end(),raw.begin()+2040),"CSO cross-block reads differ");
            Require(ImportXbox360Disc(path,target,{},error),"Equivalent CSO/ZSO failed verified ISO extraction/resume");
            Require(VerifyXbox360Disc(path,target,error),"Compressed disc reference verification failed");
        }
        auto cci_fixture=[&](const fs::path& path,std::span<const uint8_t> logical){
            Require(logical.size()%2048==0,"CCI fixture is not sector-aligned");std::vector<uint8_t> container(32,0);std::vector<uint32_t> positions;
            auto word=[&](size_t at,uint32_t n){for(unsigned i=0;i<4;++i)container[at+i]=uint8_t(n>>(8*i));};
            std::copy_n("CCIM",4,container.begin());word(4,32);word(8,uint32_t(logical.size()));word(12,uint32_t(uint64_t(logical.size())>>32));word(24,2048);container[28]=1;container[29]=2;
            for(size_t b=0;b<logical.size()/2048;++b){const auto sector=logical.subspan(b*2048,2048);std::vector<uint8_t> packed;
                if(b%3 && std::all_of(sector.begin(),sector.end(),[&](uint8_t v){return v==sector[0];})){
                    const auto literals=1+b%4;packed.push_back(uint8_t(literals<<4|15));packed.insert(packed.end(),literals,sector[0]);packed.push_back(1);packed.push_back(0);
                    size_t extra=2048-literals-24;while(extra>=255){packed.push_back(255);extra-=255;}packed.push_back(uint8_t(extra));packed.push_back(0x50);packed.insert(packed.end(),5,sector[0]);
                }
                positions.push_back(uint32_t(container.size()/4)|(packed.empty()?0u:0x80000000u));
                if(packed.empty())container.insert(container.end(),sector.begin(),sector.end());
                else{const auto pad=(4-(packed.size()+1)%4)%4;container.push_back(uint8_t(pad));container.insert(container.end(),packed.begin(),packed.end());container.insert(container.end(),pad,0);}
            }
            const auto table=container.size();positions.push_back(uint32_t(table/4));word(16,uint32_t(table));word(20,uint32_t(uint64_t(table)>>32));
            container.resize(table+positions.size()*4);for(size_t i=0;i<positions.size();++i)word(table+i*4,positions[i]);
            std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(container.data()),container.size());
        };
        const auto cci=root/"single.cci";cci_fixture(cci,raw);
        {Image image(cci);std::vector<uint8_t> decoded(raw.size());image.Read(0,decoded);Require(decoded==raw,"CCI changed logical disc bytes");}
        Require(ImportXbox360Disc(cci,target,{},error) && VerifyXbox360Disc(cci,target,error),"CCI extraction or verified resume failed");
        constexpr size_t split=17*2048;cci_fixture(root/"split.1.cci",std::span(raw).first(split));cci_fixture(root/"split.2.cci",std::span(raw).subspan(split));
        {Image image(root/"split.2.cci");std::vector<uint8_t> decoded(raw.size());image.Read(0,decoded);Require(decoded==raw,"CCI numbered parts did not join correctly");
            std::array<uint8_t,4125> crossing{};image.Read(split-13,crossing);Require(std::equal(crossing.begin(),crossing.end(),raw.begin()+split-13),"CCI part boundary read differs");}
        Require(ImportXbox360Disc(root/"split.2.cci",target,{},error),"CCI split image failed equivalent-container resume");
        fs::copy_file(root/"split.2.cci",root/"split.33.cci");
        {bool rejected=false;try{Image malformed(root/"split.1.cci");}catch(const Failure&){rejected=true;}Require(rejected,"CCI excess part was silently omitted");}
        fs::remove(root/"split.33.cci");
        fs::rename(root/"split.2.cci",root/"split.3.cci");
        {bool rejected=false;try{Image malformed(root/"split.1.cci");}catch(const Failure&){rejected=true;}Require(rejected,"CCI missing middle part accepted");}
        const auto bad_cci=root/"bad-pad.cci";cci_fixture(bad_cci,raw);
        {std::fstream change(bad_cci,std::ios::binary|std::ios::in|std::ios::out);change.seekp(32+2048);change.put(4);}
        {Image malformed(bad_cci);std::array<uint8_t,2048> bytes{};malformed.Read(0x10000,bytes);
            bool rejected=false;try{malformed.Read(2048,bytes);}catch(const Failure&){rejected=true;}Require(rejected,"CCI invalid block padding accepted");
            malformed.Read(0x10000,bytes);Require(std::equal(bytes.begin(),bytes.end(),raw.begin()+0x10000),"Failed compressed decode poisoned a previously cached sector");}
        auto chd_fixture=[&](const fs::path& path,bool legacy,int invalid){
            constexpr size_t hunk=4096;const auto count=(raw.size()+hunk-1)/hunk;
            std::vector<uint8_t> container(legacy?120+count*16+16:hunk+count*hunk,0);
            auto be=[&](size_t at,uint64_t n,unsigned length){for(unsigned i=0;i<length;++i)container[at+i]=uint8_t(n>>(8*(length-1-i)));};
            std::copy_n("MComprHD",8,container.begin());be(8,legacy?120:124,4);be(12,legacy?3:5,4);
            if(legacy){be(16,invalid==2?1:0,4);be(20,1,4);be(24,count,4);be(28,raw.size(),8);be(76,hunk,4);std::copy_n("EndOfListCookie\0",16,container.begin()+120+count*16);}
            else{be(32,raw.size(),8);be(40,124,8);be(56,hunk,4);be(60,2048,4);if(invalid==2)container[104]=1;}
            for(size_t b=0;b<count;++b){std::vector<uint8_t> bytes(hunk,0);std::copy_n(raw.begin()+b*hunk,std::min(hunk,raw.size()-b*hunk),bytes.begin());
                if(!legacy){const bool zero=b%3 && std::all_of(bytes.begin(),bytes.end(),[](uint8_t v){return v==0;});be(124+b*4,zero?0:b+1,4);std::copy(bytes.begin(),bytes.end(),container.begin()+hunk+b*hunk);continue;}
                const auto encoded=em1::DeflateNativePakResource(bytes);const auto payload=std::span(encoded).subspan(2,encoded.size()-6);const bool packed=payload.size()<hunk;
                uint32_t crc=0xffffffff;for(auto byte:bytes){crc^=byte;for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320:0);}crc=~crc;
                const auto row=120+b*16;be(row,container.size(),8);be(row+8,crc^(invalid==1 && b==0?1u:0u),4);const auto length=packed?payload.size():bytes.size();be(row+12,length&65535,2);container[row+14]=uint8_t(length>>16);container[row+15]=packed?1:2;
                if((invalid==3 && b==0) || (invalid==4 && b==1)){be(row,0,8);container[row+15]=4;}
                const auto data=packed?payload:std::span<const uint8_t>(bytes);container.insert(container.end(),data.begin(),data.end());
            }
            if(invalid==5){const auto at=container.size();container.resize(at+16,0);be(legacy?36:48,at,8);be(at+8,at,8);}
            if(invalid==6){be(16,0x7a6c6962,4);be(124,0x40000000,4);}
            std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(container.data()),container.size());
        };
        for(bool legacy:{false,true}){const auto path=root/(legacy?"deflate-v3.chd":"raw-v5.chd");chd_fixture(path,legacy,0);
            Image image(path);std::vector<uint8_t> decoded(raw.size());image.Read(0,decoded);Require(decoded==raw,"CHD hunk decoding changed logical bytes");
            std::array<uint8_t,4125> crossing{};image.Read(4083,crossing);Require(std::equal(crossing.begin(),crossing.end(),raw.begin()+4083),"CHD cross-hunk read differs");
            Require(ImportXbox360Disc(path,target,{},error) && VerifyXbox360Disc(path,target,error),"CHD extraction or verified resume failed");
        }
        for(int invalid:{1,2,3,5,6}){const auto path=root/("bad"+std::to_string(invalid)+".chd");chd_fixture(path,invalid!=6,invalid);bool rejected=false;
            try{Image malformed(path);std::array<uint8_t,4096> decoded{};malformed.Read(0,decoded);}catch(const Failure&){rejected=true;}Require(rejected,"CHD accepted corrupt CRC, required parent or cyclic self reference");}
        {const auto path=root/"self-ref.chd";chd_fixture(path,true,4);Image image(path);std::vector<uint8_t> decoded(raw.size());image.Read(0,decoded);Require(decoded==raw,"Valid CHD self reference decoded incorrectly");}
        auto god_fixture=[&](const fs::path& path,unsigned mode){
            fs::create_directories(path.parent_path());
            const bool enhanced=mode==0 || mode==3,internal=mode==2,split=mode==3;constexpr uint64_t per_file=0x14388,max_file=0xa290000;
            const uint32_t blocks=split?41413:enhanced?25:24,count=split?2:1;std::vector<uint8_t> header(0xb000,0);
            const auto be=[&](size_t at,uint32_t n){for(unsigned i=0;i<4;++i)header[at+i]=uint8_t(n>>(24-8*i));};
            std::copy_n("LIVE",4,header.begin());be(0x340,0xad0e);be(0x344,0x7000);be(0x348,2);be(0x360,0x42560816);header[0x379]=0x24;header[0x391]=enhanced?0x40:0;
            for(unsigned i=0;i<3;++i)header[0x392+i]=uint8_t(blocks>>(i*8));be(0x39d,count);be(0x3a9,1);
            if(split){header[0x392]=0;header[0x393]=0;header[0x394]=1;} // Trimmed/converter allocation counts must not truncate actual parts.
            {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(header.data()),header.size());}
            const auto folder=fs::path(path.wstring()+L".data");if(!internal)fs::create_directory(folder);
            std::vector<std::unique_ptr<Handle>> files;
            const auto address=[&](uint64_t logical){const auto true_block=logical+(enhanced?2:0),block=true_block%per_file,l0=block/0x198+1;
                return std::pair{size_t(true_block/per_file),block*2048+l0*4096+4096+(internal?0xb000:0)};};
            for(uint32_t i=0;i<count;++i){const auto part=internal?path:folder/(i?L"Data0001":L"Data0000");auto file=std::make_unique<Handle>();file->value=CreateFileW(part.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,internal?OPEN_EXISTING:CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);Require(file->value!=INVALID_HANDLE_VALUE,"Cannot create private GOD fixture");DWORD ignored=0;DeviceIoControl(file->value,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&ignored,nullptr);
                const auto end=address(uint64_t(blocks)*2-1-(enhanced?2:0));LARGE_INTEGER length{};length.QuadPart=LONGLONG(i+1<count?max_file:end.second+2048-(mode==1?2048:0));Require(SetFilePointerEx(file->value,length,nullptr,FILE_BEGIN) && SetEndOfFile(file->value),"Cannot size private GOD fixture");files.push_back(std::move(file));}
            const auto write=[&](size_t part,uint64_t at,std::span<const uint8_t> data){LARGE_INTEGER offset{};offset.QuadPart=LONGLONG(at);DWORD written=0;Require(SetFilePointerEx(files.at(part)->value,offset,nullptr,FILE_BEGIN) && WriteFile(files.at(part)->value,data.data(),DWORD(data.size()),&written,nullptr) && written==data.size(),"Cannot write GOD fixture sector");};
            auto content=raw;if(split){const auto put=[&](size_t at,uint32_t n){for(unsigned i=0;i<4;++i)content[at+i]=uint8_t(n>>(i*8));};put(42*2048+4,uint32_t(per_file-3));put(42*2048+8,4096);}
            for(size_t b=0;b<content.size()/2048;++b){const auto [part,offset]=address(b);write(part,offset,std::span(content).subspan(b*2048,2048));}
            write(0,internal?0xd000:enhanced?0x2000:0x12000,std::span(raw).subspan(0x10000,2048));
            if(split){std::array<uint8_t,4096> payload{};for(size_t i=0;i<payload.size();++i)payload[i]=uint8_t(i*17+3);
                for(unsigned b=0;b<2;++b){const auto [part,offset]=address(per_file-3+b);write(part,offset,std::span(payload).subspan(b*2048,2048));}}
            BCRYPT_ALG_HANDLE sha_provider=nullptr;Require(BCryptOpenAlgorithmProvider(&sha_provider,BCRYPT_SHA1_ALGORITHM,nullptr,0)>=0,"Cannot initialize fixture SHA-1");
            struct CloseSha{BCRYPT_ALG_HANDLE value;~CloseSha(){BCryptCloseAlgorithmProvider(value,0);}} close_sha{sha_provider};
            const auto sha=[&](std::span<const uint8_t> data){std::array<uint8_t,20> hash{};Require(BCryptHash(sha_provider,nullptr,0,const_cast<PUCHAR>(data.data()),ULONG(data.size()),hash.data(),ULONG(hash.size()))>=0,"Cannot hash GOD fixture data");return hash;};
            const auto read=[&](size_t part,uint64_t at,std::span<uint8_t> data){LARGE_INTEGER position{};position.QuadPart=LONGLONG(at);DWORD got=0;Require(SetFilePointerEx(files.at(part)->value,position,nullptr,FILE_BEGIN) && ReadFile(files.at(part)->value,data.data(),DWORD(data.size()),&got,nullptr) && got==data.size(),"Cannot read GOD fixture data");};
            std::array<uint8_t,20> next_master{};
            for(size_t part=count;part-->0;){std::array<uint8_t,4096> master{};if(part+1<count)std::copy(next_master.begin(),next_master.end(),master.begin()+203*20);
                LARGE_INTEGER physical{};Require(GetFileSizeEx(files.at(part)->value,&physical),"Cannot size fixture hash tree");
                for(size_t group=0;group<203;++group){const auto table_at=(internal?0xb000:0)+4096+uint64_t(group)*(4096+204*4096),data_at=table_at+4096;
                    if(data_at>=uint64_t(physical.QuadPart))break;
                    std::vector<uint8_t> data(size_t(std::min<uint64_t>(204*4096,uint64_t(physical.QuadPart)-data_at)));read(part,data_at,data);std::array<uint8_t,4096> table{};
                    for(size_t at=0;at<data.size();at+=4096){const auto hash=sha(std::span(data).subspan(at,std::min<size_t>(4096,data.size()-at)));std::copy(hash.begin(),hash.end(),table.begin()+at/4096*20);}
                    write(part,table_at,table);const auto hash=sha(table);std::copy(hash.begin(),hash.end(),master.begin()+group*20);
                }
                write(part,internal?0xb000:0,master);next_master=sha(master);
            }
            if(internal)write(0,0x37d,next_master);
            else{std::fstream package(path,std::ios::binary|std::ios::in|std::ios::out);package.seekp(0x37d);package.write(reinterpret_cast<const char*>(next_master.data()),next_master.size());Require(bool(package),"Cannot seal fixture master hash");}
        };
        for(unsigned mode=0;mode<4;++mode){const auto package=root/("god"+std::to_string(mode))/"Package",output=root/("god-output"+std::to_string(mode));god_fixture(package,mode);
            Require(SvodImage::FindPackage(package.parent_path(),error)==package && error.empty(),"GOD folder did not resolve its extensionless package header");
            if(mode!=2)Require(SvodImage::FindPackage(fs::path(package.wstring()+L".data"),error)==package,"GOD data folder did not resolve its package header");
            Require(ImportXbox360Disc(package,output,{},error) && VerifyXbox360Disc(package,output,error),"GOD extraction/verification failed");
            if(mode<3)Require(VerifyXbox360Disc(package,target,error),"GOD decoded bytes differ from the source ISO reference");
            else{Image image(package);std::array<uint8_t,4096> payload{};image.Read(uint64_t(0x14388-3)*2048,payload);for(size_t i=0;i<payload.size();++i)Require(payload[i]==uint8_t(i*17+3),"GOD fragment-boundary bytes differ");}
        }
        {const auto package=root/"god0/Package",folder=fs::path(package.wstring()+L".data");fs::rename(folder/L"Data0000",folder/L"missing");bool rejected=false;
            try{Image malformed(package);}catch(const Failure&){rejected=true;}Require(rejected,"GOD missing fragment accepted");fs::rename(folder/L"missing",folder/L"Data0000");}
        {const auto package=root/"god0/Package",duplicate=root/"god0/Duplicate";fs::copy_file(package,duplicate);
            Require(SvodImage::FindPackage(package.parent_path(),error).empty() && !error.empty(),"GOD folder silently chose between multiple packages");fs::remove(duplicate);}
        const auto flip_byte=[&](const fs::path& path,uint64_t at){std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);file.seekg(std::streamoff(at));char byte=0;file.read(&byte,1);Require(bool(file),"Cannot read fixture corruption byte");byte^=1;file.seekp(std::streamoff(at));file.write(&byte,1);Require(bool(file),"Cannot write fixture corruption byte");};
        for(const auto at:{uint64_t(0),uint64_t(0x1000),uint64_t(0x3000)}){
            const auto package=root/"god0/Package",part=fs::path(package.wstring()+L".data")/L"Data0000";flip_byte(part,at);
            Require(!ImportXbox360Disc(package,root/"corrupt-god-output",{},error) && !error.empty() && !fs::exists(root/"corrupt-god-output"),"Corrupt GOD hash/data reached extraction");flip_byte(part,at);
        }
        {const auto package=root/"god3/Package",part=fs::path(package.wstring()+L".data")/L"Data0001";flip_byte(part,0);bool rejected=false;
            try{Image malformed(package);}catch(const Failure&){rejected=true;}Require(rejected,"Corrupt GOD second-fragment master chain accepted");flip_byte(part,0);}
        {const auto package=root/"god3/Package",part=fs::path(package.wstring()+L".data")/L"Data0000";flip_byte(part,0xa290000-1);
            {Image image(package);std::array<uint8_t,80> executable{};image.Read(44*2048,executable);bool rejected=false;
                try{std::array<uint8_t,4096> corrupt{};image.Read(uint64_t(0x14388-3)*2048,corrupt);}catch(const Failure&){rejected=true;}Require(rejected,"Corrupt GOD final data group accepted");
                image.Read(44*2048,executable);Require(std::equal(executable.begin(),executable.end(),raw.begin()+44*2048),"GOD checksum failure poisoned the verified group cache");}
            Require(!ImportXbox360Disc(package,root/"late-corrupt-god-output",{},error) && !error.empty() && !fs::exists(root/"late-corrupt-god-output"),"Late GOD corruption created an extraction destination");
            flip_byte(part,0xa290000-1);
        }
        const auto zar_fixture=[&](const fs::path& path,unsigned mode,int invalid){
            const bool iso=mode==2,wrapped=mode==1;std::vector<uint8_t> logical(2*65536,0);
            if(iso)std::copy(raw.begin(),raw.end(),logical.begin()+37);
            else{std::copy_n(raw.begin()+44*2048,80,logical.begin());std::copy_n("config",6,logical.begin()+80);}
            std::vector<uint8_t> archive(logical.begin(),logical.begin()+65536);
            constexpr std::array<uint8_t,20> zero_frame={40,181,47,253,96,0,255,85,0,0,16,0,0,1,0,251,127,29,96,1};
            if(iso)archive.insert(archive.end(),logical.begin()+65536,logical.end());else{archive.insert(archive.end(),zero_frame.begin(),zero_frame.end());if(invalid==7)archive[65536]^=1;}
            const auto data_size=archive.size();archive.resize((archive.size()+7)&~size_t(7),0);const auto record_at=archive.size();archive.resize(archive.size()+40,0);
            const auto be=[&](size_t at,uint64_t n,unsigned bytes){for(unsigned i=0;i<bytes;++i)archive[at+i]=uint8_t(n>>(8*(bytes-1-i)));};be(record_at,0,8);be(record_at+8,65535,2);be(record_at+10,iso?65535:19,2);
            const auto names_at=archive.size();const auto name=[&](std::string_view value){const auto at=archive.size()-names_at;if(value.size()>=128){archive.push_back(uint8_t(value.size()&127)|128);archive.push_back(uint8_t(value.size()>>7));}else archive.push_back(uint8_t(value.size()));archive.insert(archive.end(),value.begin(),value.end());return uint32_t(at);};
            const auto empty=name("");std::vector<std::array<uint32_t,4>> nodes;
            if(iso){const auto disc=name("Disc.iso");nodes={{{empty,1,1,0}},{{0x80000000u|disc,37,uint32_t(raw.size()),0}}};}
            else{const auto folder=wrapped?name("EM2"):0,exe=name("default.xex"),config=name("ConfigFiles.ini"),packs=name("packfiles"),aux=name(invalid==1?"../escape":invalid==3?"CONFIGFILES.INI":std::string(130,'a')),pak=name("level.pak");
                nodes.push_back({empty,wrapped?1u:1u,wrapped?1u:4u,0});if(wrapped)nodes.push_back({folder,2,4,0});
                nodes.push_back({0x80000000u|exe,0,80,0});nodes.push_back({0x80000000u|config,80,6,0});nodes.push_back({packs,wrapped?6u:5u,1,0});nodes.push_back({0x80000000u|aux,0,0,0});nodes.push_back({0x80000000u|pak,86,65546,0});
                if(invalid==2)nodes[0][1]=0;if(invalid==5)nodes.back()[3]=65535;
            }
            const auto names_size=archive.size()-names_at,tree_at=archive.size();archive.resize(archive.size()+nodes.size()*16,0);for(size_t n=0;n<nodes.size();++n)for(unsigned w=0;w<4;++w)be(tree_at+n*16+w*4,nodes[n][w],4);
            const auto footer=archive.size();archive.resize(archive.size()+144,0);
            const auto section=[&](unsigned n,uint64_t at,uint64_t bytes){be(footer+n*16,at,8);be(footer+n*16+8,bytes,8);};section(0,0,data_size);section(1,record_at,40);section(2,invalid==4?record_at:names_at,names_size);section(3,tree_at,nodes.size()*16);section(4,footer,0);section(5,footer,0);
            be(footer+128,archive.size(),8);be(footer+136,0x61bf3a01,4);be(footer+140,0x169f52d6,4);Digest digest;digest.Add(archive);const auto hash=digest.Finish();
            const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};for(size_t i=0;i<32;++i)archive[footer+96+i]=uint8_t(digit(hash[i*2])*16+digit(hash[i*2+1]));
            std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(archive.data()),archive.size());
        };
        for(unsigned mode=0;mode<3;++mode){const auto path=root/("archive"+std::to_string(mode)+".zar"),output=root/("zar-output"+std::to_string(mode));zar_fixture(path,mode,0);
            Require(ImportXbox360Disc(path,output,{},error) && ImportXbox360Disc(path,output,{},error) && VerifyXbox360Disc(path,output,error),"ZArchive import, resume or verification failed");
            if(mode==2)Require(VerifyXbox360Disc(path,target,error),"ZArchive-wrapped ISO differs from its extracted reference");
            else{Image image(path);std::array<uint8_t,80> exe{};image.Read(0,exe);Require(std::equal(exe.begin(),exe.end(),raw.begin()+44*2048),"ZArchive raw-block bytes differ");std::vector<uint8_t> zeroes(65546);image.Read(86,zeroes);Require(std::all_of(zeroes.begin(),zeroes.end(),[](uint8_t b){return b==0;}),"ZArchive compressed/cross-block bytes differ");}
        }
        for(int invalid:{1,2,3,4,5}){const auto path=root/("invalid-archive"+std::to_string(invalid)+".zar");zar_fixture(path,0,invalid);
            Require(!ImportXbox360Disc(path,root/"invalid-zar-output",{},error) && !error.empty() && !fs::exists(root/"invalid-zar-output"),"Unsafe/malformed ZArchive reached extraction");}
        {const auto path=root/"checksum.zar";zar_fixture(path,0,0);flip_byte(path,0);Require(!ImportXbox360Disc(path,root/"checksum-zar-output",{},error) && !fs::exists(root/"checksum-zar-output"),"Corrupt ZArchive integrity hash reached extraction");}
        {const auto path=root/"bad-frame.zar";zar_fixture(path,0,7);Image image(path);std::array<uint8_t,80> exe{};image.Read(0,exe);bool rejected=false;
            try{image.Read(65536,exe);}catch(const Failure&){rejected=true;}Require(rejected,"Bad ZArchive Zstandard frame accepted");image.Read(0,exe);Require(std::equal(exe.begin(),exe.end(),raw.begin()+44*2048),"Bad ZArchive decode poisoned a cached block");}
        {const auto path=root/"archive0.zar";Require(!ImportXbox360Disc(path,root/"canceled-zar-output",[](std::wstring_view,uint64_t,uint64_t){return false;},error) && error.empty() && !fs::exists(root/"canceled-zar-output"),"ZArchive cancellation created output or reported a failure");}
        {std::ifstream input(root/"archive0.zar",std::ios::binary);const std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(input),{});
            for(const auto cut:{size_t(1),size_t(143),size_t(144),bytes.size()-1}){const auto path=root/"truncated.zar";
                {std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()-cut);}
                Require(!ImportXbox360Disc(path,root/"truncated-zar-output",{},error) && !error.empty() && !fs::exists(root/"truncated-zar-output"),"Truncated ZArchive reached extraction");}
        }
        REXLOG_INFO("ZArchive regressions passed: root/wrapped game trees, single ISO, raw/Zstandard cross-block reads, extended/empty names, SHA-256 integrity, resume, cancellation, unsafe/cyclic/overlapping/oversized/truncated metadata and failed-decode cache recovery");
        {const auto bad=root/"bad-index.cso";compressed_fixture(bad,1,0,false,false);std::fstream change(bad,std::ios::binary|std::ios::in|std::ios::out);change.seekp(24);const char zero[4]{};change.write(zero,4);change.close();
            bool rejected=false;try{Image malformed(bad);}catch(const Failure&){rejected=true;}Require(rejected,"Overlapping CSO header/index accepted");}
        for(const auto& bad:std::vector<std::vector<uint8_t>>{{0x10,'a',0,0},{0x10,'a',2,0},{0xf0,255,255},{0x10,'a',1}}){
            std::array<uint8_t,32> decoded{};bool rejected=false;try{Image::DecodeLz4(bad,decoded,16,0);}catch(const Failure&){rejected=true;}
            Require(rejected,"Malformed LZ4 distance, length or truncated match accepted");
        }
        std::ifstream stream(target/"packfiles"/"level.pak",std::ios::binary);Require(std::string(std::istreambuf_iterator<char>(stream),{})=="level-data","Extracted ISO contents differ.");
        Require(!fs::exists(root/"escape"),"Unsafe ISO path escaped extraction.");
        REXLOG_INFO("Native disc importer passed: XDVDFS, CSO1/2, ZSO, CCI, raw V5/DEFLATE V3 CHD, GOD layouts and SHA-1 master/table/data/fragment-chain checks, partial blocks, cache recovery, corrupt-source rejection and verified extraction (synthetic images; retail container validation remains required)");return true;
    }catch(const std::exception& exception){REXLOG_ERROR("Disc image fixture: {}",exception.what());return false;}
}
}

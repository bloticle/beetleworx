#include "disc_image.h"
#include "em1_inflate.h"
#include "em1_deflate.h"
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
    uint32_t block_size=0,cached=UINT32_MAX;std::vector<uint32_t> index;std::vector<uint8_t> cache;
    void Physical(uint64_t offset,std::span<uint8_t> bytes){
        Require(offset<=physical_size && bytes.size()<=physical_size-offset,"Compressed image block extends outside its container");
        LARGE_INTEGER position{};position.QuadPart=LONGLONG(offset);Require(SetFilePointerEx(file.value,position,nullptr,FILE_BEGIN),"Could not seek in the disc image");
        DWORD read=0;Require(bytes.size()<=MAXDWORD && ReadFile(file.value,bytes.data(),DWORD(bytes.size()),&read,nullptr) && read==bytes.size(),"Could not read disc image bytes");
    }
    uint64_t Position(size_t i)const{return uint64_t(index[i]&0x7FFFFFFF)<<shift;}
    explicit Image(const fs::path& path,bool discover_parts=true) {
        // Deny writers while validating, hashing and extracting this image.
        file.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        LARGE_INTEGER length{};Require(file.value!=INVALID_HANDLE_VALUE && GetFileSizeEx(file.value,&length),"Could not open the disc image. Close any app writing it and try again.");
        size=physical_size=uint64_t(length.QuadPart);Require(size>=24 && size<=16ull*1024*1024*1024,"Invalid Xbox disc image size");
        std::array<uint8_t,24> header{};Physical(0,header);
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
        {const auto bad=root/"bad-index.cso";compressed_fixture(bad,1,0,false,false);std::fstream change(bad,std::ios::binary|std::ios::in|std::ios::out);change.seekp(24);const char zero[4]{};change.write(zero,4);change.close();
            bool rejected=false;try{Image malformed(bad);}catch(const Failure&){rejected=true;}Require(rejected,"Overlapping CSO header/index accepted");}
        for(const auto& bad:std::vector<std::vector<uint8_t>>{{0x10,'a',0,0},{0x10,'a',2,0},{0xf0,255,255},{0x10,'a',1}}){
            std::array<uint8_t,32> decoded{};bool rejected=false;try{Image::DecodeLz4(bad,decoded,16,0);}catch(const Failure&){rejected=true;}
            Require(rejected,"Malformed LZ4 distance, length or truncated match accepted");
        }
        std::ifstream stream(target/"packfiles"/"level.pak",std::ios::binary);Require(std::string(std::istreambuf_iterator<char>(stream),{})=="level-data","Extracted ISO contents differ.");
        Require(!fs::exists(root/"escape"),"Unsafe ISO path escaped extraction.");
        REXLOG_INFO("Native disc importer passed: four XDVDFS bases, CSO1/2 DEFLATE and LZ4/ZSO, single/split CCI, raw/partial/aligned/cross-block decoding, malformed index/padding/missing parts, title/path rejection and equivalent-container SHA-256 resume (synthetic images; retail compressed images still need validation)");return true;
    }catch(const std::exception& exception){REXLOG_ERROR("Disc image fixture: {}",exception.what());return false;}
}
}

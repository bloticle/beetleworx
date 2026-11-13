#include "content_patch.h"
#include "content_setup.h"
#include "content_resource_patch.h"
#include "em1_inflate.h"
#include "em1_deflate.h"
#include <windows.h>
#include <bcrypt.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <stdexcept>

namespace em2 {
namespace {
namespace fs=std::filesystem;
constexpr size_t maximum_file=64*1024*1024,maximum_commands=96*1024*1024,header_size=96;
constexpr std::array<uint8_t,8> magic={'E','M','2','D','P','0','1',0};
bool Header(std::span<const uint8_t> patch){return patch.size()>header_size &&
    std::equal(magic.begin(),magic.begin()+6,patch.begin()) && (patch[6]=='1' || patch[6]=='2') && !patch[7];}
void Require(bool valid,const char* error){if(!valid)throw std::runtime_error(error);}
uint64_t U64(std::span<const uint8_t> bytes,size_t offset){
    Require(offset<=bytes.size() && bytes.size()-offset>=8,"Truncated content patch");
    uint64_t value=0;for(unsigned i=0;i<8;++i)value|=uint64_t(bytes[offset+i])<<(8*i);return value;
}
std::array<uint8_t,32> Hash(std::span<const uint8_t> bytes){
    std::array<uint8_t,32> hash{};
    Require(bytes.size()<=MAXDWORD && BCryptHash(BCRYPT_SHA256_ALG_HANDLE,nullptr,0,
        const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),hash.data(),ULONG(hash.size()))>=0,"Content hashing failed");
    return hash;
}
std::vector<uint8_t> Read(const fs::path& path,size_t limit){
    const auto size=fs::file_size(path);Require(size<=limit,"Content file exceeds supported patch size");
    std::vector<uint8_t> bytes(size);std::ifstream input(path,std::ios::binary);
    Require(bool(input.read(reinterpret_cast<char*>(bytes.data()),bytes.size())),"Could not read content patch or original archive");
    Require(input.peek()==std::char_traits<char>::eof(),"Content file changed while being read");return bytes;
}
void Put64(std::vector<uint8_t>& bytes,uint64_t value){for(unsigned i=0;i<8;++i)bytes.push_back(uint8_t(value>>(8*i)));}
std::vector<uint8_t> Fixture(std::span<const uint8_t> original,std::span<const uint8_t> target,std::span<const uint8_t> commands){
    std::vector<uint8_t> patch(magic.begin(),magic.end());Put64(patch,original.size());Put64(patch,target.size());Put64(patch,commands.size());
    for(const auto& hash:{Hash(original),Hash(target)})patch.insert(patch.end(),hash.begin(),hash.end());
    const auto packed=em1::DeflateNativePakResource(commands);patch.insert(patch.end(),packed.begin(),packed.end());return patch;
}
}
bool DecodeContentPatch(std::span<const uint8_t> original,std::span<const uint8_t> patch,std::vector<uint8_t>& output,std::string& error){
    try {
        output.clear();
        Require(original.size()<=maximum_file && patch.size()>header_size && patch.size()<=maximum_file,"Invalid content patch size");
        Require(Header(patch),"Unsupported content patch version");
        if(patch[6]=='2'){
            const auto packed=U64(patch,96),resource_size=U64(patch,104);
            Require(patch.size()>112 && packed && packed<=patch.size()-112 && resource_size && resource_size<=maximum_file,"Invalid regional patch envelope");
            const auto source_hash=Hash(original);
            if(original.size()==U64(patch,8) && std::equal(source_hash.begin(),source_hash.end(),patch.begin()+32)){
                std::vector<uint8_t> classic(patch.begin(),patch.begin()+96);classic[6]='1';
                classic.insert(classic.end(),patch.begin()+112,patch.begin()+112+size_t(packed));return DecodeContentPatch(original,classic,output,error);
            }
            std::vector<uint8_t> resources(size_t(resource_size),uint8_t(0));
            if(!em1::InflateZlib(patch.subspan(112+size_t(packed)),resources,error))return false;
            output=ApplyRegionalResourcePatch(original,resources);error.clear();
            REXLOG_INFO("Regional archive patch applied by asset name; original localized resources preserved");return true;
        }
        const auto source_size=U64(patch,8),target_size=U64(patch,16),command_size=U64(patch,24);
        Require(source_size==original.size() && target_size && target_size<=maximum_file && command_size && command_size<=maximum_commands,"Invalid content patch dimensions");
        const auto source_hash=Hash(original);
        Require(std::equal(source_hash.begin(),source_hash.end(),patch.begin()+32),"Your original game archive does not match this patch. Use an unmodified supported game dump");
        std::vector<uint8_t> commands(command_size);
        if(!em1::InflateZlib(patch.subspan(header_size),commands,error))return false;
        std::vector<uint8_t> candidate;candidate.reserve(target_size);
        size_t offset=0;
        while(offset<commands.size()){
            const auto type=commands[offset++];uint64_t source=0;
            Require(type<=1,"Unknown content patch command");
            if(!type){source=U64(commands,offset);offset+=8;}
            const auto size=U64(commands,offset);offset+=8;
            Require(size && size<=target_size-candidate.size(),"Content patch output exceeds declared size");
            if(!type){
                Require(source<=original.size() && size<=original.size()-source,"Content patch copy exceeds original archive");
                candidate.insert(candidate.end(),original.begin()+source,original.begin()+source+size);
            }else{
                Require(size<=commands.size()-offset,"Truncated content patch literal");
                candidate.insert(candidate.end(),commands.begin()+offset,commands.begin()+offset+size);offset+=size;
            }
        }
        Require(candidate.size()==target_size,"Incomplete content patch output");
        const auto target_hash=Hash(candidate);
        Require(std::equal(target_hash.begin(),target_hash.end(),patch.begin()+64),"Content patch output hash mismatch");
        output=std::move(candidate);error.clear();return true;
    }catch(const std::exception& exception){output.clear();error=exception.what();return false;}
}
bool ApplyContentPatch(const fs::path& original,const fs::path& patch,const fs::path& destination,std::string& error){
    try {
        const auto input_path=fs::weakly_canonical(original),output_path=fs::weakly_canonical(destination);
        Require(CompareStringOrdinal(input_path.c_str(),-1,output_path.c_str(),-1,TRUE)!=CSTR_EQUAL,"Patched content must be stored separately from original game files");
        const auto delta=Read(patch,maximum_file);
        Require(Header(delta),"Invalid content patch header");
        // A previous complete reconstruction can be reused after an update or
        // interrupted setup. Always verify its bytes, never just its existence.
        if(fs::is_regular_file(destination)){
            const auto existing=Read(destination,maximum_file);const auto hash=Hash(existing);
            if(existing.size()==U64(delta,16) && std::equal(hash.begin(),hash.end(),delta.begin()+64)){error.clear();return true;}
        }
        std::vector<uint8_t> output;
        if(!DecodeContentPatch(Read(original,maximum_file),delta,output,error))return false;
        fs::create_directories(destination.parent_path());
        static std::atomic<uint64_t> nonce{0};
        const auto temporary=destination.parent_path()/(L".em2-patch-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++nonce)+L".tmp");
        struct Temporary {fs::path path;HANDLE file=INVALID_HANDLE_VALUE;~Temporary(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);DeleteFileW(path.c_str());}} work{temporary};
        work.file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        Require(work.file!=INVALID_HANDLE_VALUE,"Could not create patched content. Check folder permissions and free space");
        DWORD written=0;
        Require(WriteFile(work.file,output.data(),DWORD(output.size()),&written,nullptr) && written==output.size() && FlushFileBuffers(work.file),"Could not finish writing patched content");
        CloseHandle(work.file);work.file=INVALID_HANDLE_VALUE;
        // Replace the directory entry, never write through a pre-existing hard
        // link to the user's original archive.
        Require(MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),"Could not install patched content. Close the game and retry");
        error.clear();return true;
    }catch(const std::exception& exception){error=exception.what();return false;}
}
bool TestContentPatches(const fs::path& root){
    try {
        Require(TestRegionalResourcePatches(),"Regional resource patch regression failed");
        Require(!fs::exists(root),"Patch fixture path already exists");fs::create_directories(root);
        const std::vector<uint8_t> original={'a','b','c','d','e','f'},target={'d','e','f','X','Y','a','b','c'};
        std::vector<uint8_t> commands{0};Put64(commands,3);Put64(commands,3);commands.push_back(1);Put64(commands,2);commands.insert(commands.end(),{'X','Y'});commands.push_back(0);Put64(commands,0);Put64(commands,3);
        const auto patch=Fixture(original,target,commands);std::string error;std::vector<uint8_t> result;
        Require(DecodeContentPatch(original,patch,result,error) && result==target,"Copy/literal reconstruction failed");
        auto reject=[&](const auto& bytes){result={42};Require(!DecodeContentPatch(original,bytes,result,error) && result.empty() && !error.empty(),"Malformed patch accepted or partial output escaped");};
        for(size_t size=0;size<patch.size();++size)reject(std::span(patch).first(size));
        for(size_t offset:{size_t(0),size_t(8),size_t(16),size_t(24),size_t(32),size_t(64),patch.size()-1}){auto corrupt=patch;corrupt[offset]^=0xFF;reject(corrupt);}
        auto invalid=commands;invalid[1]=255;reject(Fixture(original,target,invalid));
        invalid=commands;invalid[0]=2;reject(Fixture(original,target,invalid));
        invalid=commands;invalid.push_back(1);reject(Fixture(original,target,invalid));
        invalid=commands;invalid[9]=0;reject(Fixture(original,target,invalid));
        auto write=[](const fs::path& path,const auto& bytes){std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());};
        write(root/"source.pak",original);write(root/"update.em2patch",patch);
        Require(!ApplyContentPatch(root/"source.pak",root/"update.em2patch",root/"source.pak",error) && Read(root/"source.pak",maximum_file)==original,"In-place modification of original archive was allowed");
        Require(CreateHardLinkW((root/"result.pak").c_str(),(root/"source.pak").c_str(),nullptr),"Patch hard-link fixture failed");
        Require(ApplyContentPatch(root/"source.pak",root/"update.em2patch",root/"result.pak",error),"Atomic patch installation failed");
        Require(Read(root/"source.pak",maximum_file)==original && Read(root/"result.pak",maximum_file)==target,"Patch changed original bytes or installed wrong output");
        Require(ApplyContentPatch(root/"missing-original",root/"update.em2patch",root/"result.pak",error),"Verified completed patch could not be reused");
        write(root/"result.pak",original);write(root/"source.pak",target);
        Require(!ApplyContentPatch(root/"source.pak",root/"update.em2patch",root/"result.pak",error) && Read(root/"result.pak",maximum_file)==original,"Wrong source modified existing destination");
        const auto game=root/"game",content=root/"recomp"/"pc-assets";
        fs::create_directories(game/"packfiles");fs::create_directories(content/"patches");
        write(game/"default.xex",original);write(game/"ConfigFiles.ini",original);
        write(content/"default.xex",original);write(content/"pc-shader-catalog.em2",original);
        {std::ofstream marker(content/"source-game-directory.txt",std::ios::binary);const auto name=fs::canonical(game).u8string();marker.write(reinterpret_cast<const char*>(name.data()),name.size());marker<<'\n';}
        constexpr auto& archives=kPatchedGameArchives;
        for(const auto* name:archives){write(game/"packfiles"/name,original);write(content/"patches"/(std::string(name)+".em2patch"),patch);}
        auto selected_game=game;
        Require(PrepareGameContent(root/"recomp",selected_game,content,true,true,error),"Normal setup did not reconstruct packaged patches");
        for(const auto* name:archives)Require(Read(content/"packfiles"/name,maximum_file)==target && Read(game/"packfiles"/name,maximum_file)==original,"Normal setup changed original archive or lost patch output");
        selected_game=game;
        Require(!PrepareGameContent(root,selected_game,game,true,true,error) && !error.empty(),"Setup accepted the original folder as patched content destination");
        for(const auto* name:archives)Require(Read(game/"packfiles"/name,maximum_file)==original,"Rejected overlapping setup changed original archives");
        REXLOG_INFO("Content patch tests passed: copy/literal roundtrip, all truncations, corrupt headers/hash/stream, invalid copy/tag/length, atomic hard-link replacement, original preservation and verified interrupted-setup reuse");return true;
    }catch(const std::exception& exception){REXLOG_ERROR("Content patch fixture: {}",exception.what());return false;}
}
}

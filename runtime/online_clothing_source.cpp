#include "online_clothing_texture.h"
#include "archive_io.h"
#include "em1_inflate.h"
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <mutex>

namespace em2::net {
namespace {
using Bytes=std::vector<uint8_t>;
using Sources=std::array<Bytes,2>;
using archive_detail::Require;
std::mutex source_mutex;
std::filesystem::path source_archive;
Sources originals;
uint64_t generation=0;
uint32_t Word(std::span<const uint8_t> b,size_t at,bool big){
    Require(at<=b.size() && b.size()-at>=4,"Truncated clothing archive word");
    uint32_t v=0;for(unsigned i=0;i<4;++i)v=(v<<8)|b[at+(big?i:3-i)];return v;
}
Sources ReadSources(const std::filesystem::path& path){
    archive_detail::File file(path);std::array<uint8_t,32> header{};file.Read(0,header);
    const bool big=std::equal(header.begin(),header.begin()+4," KAP");
    Require(big || std::equal(header.begin(),header.begin()+4,"PAK "),"Invalid clothing archive signature");
    Require(Word(header,4,big)==2 && Word(header,8,big)==0,"Unsupported clothing archive version");
    const uint64_t at=Word(header,12,big),size=Word(header,16,big);
    Require(at>=32 && at<=file.size && size>=4 && size<=16*1024*1024 && size<=file.size-at,"Invalid clothing archive directory bounds");
    Bytes directory(size);file.Read(at,directory);const auto count=Word(directory,0,big);
    Require(count<=100000 && uint64_t(count)*24+4<=size,"Invalid clothing archive entry count");
    const auto strings=size_t(count)*24+4;
    auto string_at=[&](uint32_t offset){
        Require(offset<directory.size()-strings,"Invalid clothing archive string offset");
        const auto first=directory.begin()+strings+offset,last=first+std::min<size_t>(2049,directory.end()-first);
        const auto end=std::find(first,last,0);Require(end!=last,"Unterminated clothing archive name");
        std::string value(first,end);for(auto& c:value)if(c>='A' && c<='Z')c+=32;return value;
    };
    Sources result;uint64_t payload=at+size;
    constexpr std::array<const char*,2> names{"mickeymouseng_d_tex.nif","oswaldng_d_tex.nif"};
    for(unsigned i=0;i<count;++i){const auto entry=4+size_t(i)*24;
        const auto raw=Word(directory,entry,big),stored=Word(directory,entry+4,big),aligned=Word(directory,entry+8,big);
        Require(aligned%32==0 && stored<=aligned && payload<=file.size && aligned<=file.size-payload,"Invalid clothing archive payload bounds");
        (void)string_at(Word(directory,entry+12,big));const auto name=string_at(Word(directory,entry+20,big));
        for(unsigned kind=0;kind<2;++kind)if(name==names[kind]){
            Require(result[kind].empty(),"Ambiguous duplicate default clothing texture");
            Require(raw && raw<2*1024*1024 && stored && stored<2*1024*1024,"Oversized default clothing texture");
            Bytes packed(stored);file.Read(payload,packed);
            if(raw==stored)result[kind]=std::move(packed);
            else {Bytes decoded(raw);std::string error;Require(em1::InflateZlib(packed,decoded,error),"Invalid compressed default clothing texture");result[kind]=std::move(decoded);}
        }
        payload+=aligned;
    }
    Require(!result[0].empty() && !result[1].empty(),"Mounted archive lacks the default character diffuse textures");return result;
}
}
void ConfigureOnlineClothingArchive(const std::filesystem::path& path){
    std::lock_guard lock(source_mutex);source_archive=path;originals={};++generation;
}
std::string OnlineClothingSourceKey(){
    std::lock_guard lock(source_mutex);if(source_archive.empty())return {};
    const auto path=source_archive.u8string();return "archive:"+std::string(path.begin(),path.end())+":"+std::to_string(generation);
}
bool ReadMountedOnlineClothingVariant(LobbyCharacter character,uint8_t color,ClothingTexture& output,std::string& error){
    try{
        const auto kind=unsigned(character);Require(kind<2 && color<kLobbyColors.size(),"Invalid mounted clothing selection");
        Bytes source;
        {std::lock_guard lock(source_mutex);Require(!source_archive.empty(),"No mounted clothing archive configured");
            if(originals[0].empty()){
                auto loaded=ReadSources(source_archive);
                for(unsigned i=0;i<2;++i){ClothingTexture checked;if(!RecolorOnlinePants(loaded[i],LobbyCharacter(i),uint8_t(i),checked,error))throw std::runtime_error(error);}
                originals=std::move(loaded);
                REXLOG_INFO("Online clothing source ready: two audited textures read from mounted archive; no extraction or repacking");
            }
            source=originals[kind];
        }
        return RecolorOnlinePants(source,character,color,output,error);
    }catch(const std::exception& e){error=e.what();return false;}
}
bool TestOnlineClothingArchive(const std::filesystem::path& fixture){
    try{
        Require(std::filesystem::create_directory(fixture),"Clothing archive fixture exists");
        unsigned checks=0;auto check=[&](bool v){++checks;Require(v,"Clothing archive regression failed");};
        auto build=[](bool big,bool compressed=false){
            Bytes b(84,0);std::copy_n(big?" KAP":"PAK ",4,b.begin());
            auto word=[&](size_t at,uint32_t v){for(unsigned i=0;i<4;++i)b[at+i]=uint8_t(v>>((big?3-i:i)*8));};
            word(4,2);word(12,32);word(32,2);b.push_back(0);
            for(unsigned i=0;i<2;++i){const auto at=36+i*24;word(at,3);word(at+4,compressed?11:3);word(at+8,32);word(at+20,uint32_t(b.size()-84));
                const std::string name=i?"OswaldNG_D_tex.nif":"MickeyMouseNG_D_tex.nif";b.insert(b.end(),name.begin(),name.end());b.push_back(0);}
            word(16,uint32_t(b.size()-32));const auto payload=b.size();b.resize(payload+64);
            constexpr std::array<std::array<uint8_t,11>,2> zlib{{{120,156,99,100,98,6,0,0,13,0,7},{120,156,99,97,101,3,0,0,31,0,16}}};
            for(unsigned i=0;i<2;++i){if(compressed)std::copy(zlib[i].begin(),zlib[i].end(),b.begin()+payload+i*32);
                else for(unsigned j=0;j<3;++j)b[payload+i*32+j]=uint8_t(1+i*3+j);}return b;
        };
        const auto path=fixture/"source.pak";
        auto write=[&](const Bytes& b){std::ofstream out(path,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(b.data()),b.size());Require(out.good(),"Cannot write clothing fixture");};
        auto rejected=[&](const Bytes& b){write(b);try{(void)ReadSources(path);return false;}catch(const std::exception&){return true;}};
        for(bool big:{false,true}){auto b=build(big);write(b);auto result=ReadSources(path);check(result[0]==Bytes({1,2,3}) && result[1]==Bytes({4,5,6}));
            auto bad=b;bad.resize(31);check(rejected(bad));bad=b;bad[4]=255;check(rejected(bad));
            bad=b;for(unsigned i=0;i<4;++i)bad[16+i]=255;check(rejected(bad));
            bad=b;for(unsigned i=0;i<4;++i)bad[56+i]=255;check(rejected(bad));
            bad=b;std::copy_n(b.begin()+56,4,bad.begin()+80);check(rejected(bad));
            bad=b;bad.pop_back();check(rejected(bad));
            b=build(big,true);write(b);result=ReadSources(path);check(result[0]==Bytes({1,2,3}) && result[1]==Bytes({4,5,6}));
            bad=b;bad[bad.size()-64+10]^=1;check(rejected(bad));
            bad=b;bad[40+(big?3:0)]=12;check(rejected(bad));
        }
        ClothingTexture output;output.bytes={9,8};std::string error;
        check(!ReadMountedOnlineClothingVariant(LobbyCharacter(255),0,output,error) && output.bytes==Bytes({9,8}));
        REXLOG_INFO("Online clothing archive passed {} endian/bounds/duplicates/truncation/zlib/checksum/trailing-data/output-preservation checks; actual mounted source and GPU binding separate",checks);return true;
    }catch(const std::exception& e){REXLOG_ERROR("Online clothing archive regression: {}",e.what());return false;}
}
}

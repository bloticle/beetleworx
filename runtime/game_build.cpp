#include "game_build.h"
#include "em2_game_build.h"
#include <rex/runtime.h>
#include <rex/runtime/xex_module.h>
#include <rex/sha256.h>
#include <rex/logging.h>
#include <algorithm>
#include <fstream>
#include <map>
#include <span>
#include <stdexcept>
#include <vector>

namespace em2 {namespace {
using Bytes=std::vector<uint8_t>;
uint32_t Word(std::span<const uint8_t> bytes,size_t offset){
    if(offset>bytes.size() || bytes.size()-offset<4)throw std::runtime_error("Truncated XEX header");
    return uint32_t(bytes[offset])<<24|uint32_t(bytes[offset+1])<<16|uint32_t(bytes[offset+2])<<8|bytes[offset+3];
}
void Number(Bytes& bytes,uint32_t word){for(int shift=24;shift>=0;shift-=8)bytes.push_back(uint8_t(word>>shift));}
std::string Digest(std::span<const uint8_t> bytes){return rex::util::sha256({reinterpret_cast<const char*>(bytes.data()),bytes.size()});}
std::string Profile(std::span<const uint8_t> bytes){
    if(bytes.size()<24 || Word(bytes,0)!=0x58455832)throw std::runtime_error("Not an XEX2 executable");
    const auto limit=Word(bytes,8),security=Word(bytes,16),count=Word(bytes,20);
    if(limit<24 || limit>bytes.size() || limit>1024*1024 || count>256 || 24+count*8>limit)
        throw std::runtime_error("Invalid XEX header bounds");
    if(security<24+count*8 || uint64_t(security)+0x184>limit)throw std::runtime_error("Invalid XEX security bounds");
    const auto pages=Word(bytes,security+0x180);
    if(!pages || pages>4096 || uint64_t(security)+0x184+pages*24>limit)throw std::runtime_error("Invalid XEX page descriptors");
    const std::string_view domain="Beetleworx-XEX-execution-v2";
    Bytes profile(domain.begin(),domain.end());
    for(auto offset:{4u,security+4,security+0x10C,security+0x110,security+0x160})Number(profile,Word(bytes,offset));
    Number(profile,pages);
    for(uint32_t i=0;i<pages;++i)Number(profile,Word(bytes,security+0x184+i*24));
    std::map<uint32_t,std::span<const uint8_t>> headers;
    for(uint32_t i=0;i<count;++i){
        const auto key=Word(bytes,24+i*8),value=Word(bytes,28+i*8);auto size=key&255;
        std::span<const uint8_t> payload;
        if(size<=1)payload=bytes.subspan(28+i*8,4);
        else{
            size=size==255?Word(bytes,value):size*4;
            if(value<24+count*8 || !size || uint64_t(value)+size>limit)throw std::runtime_error("Invalid XEX optional header bounds");
            payload=bytes.subspan(value,size);
        }
        if(!headers.emplace(key,payload).second)throw std::runtime_error("Duplicate XEX optional header");
    }
    const auto format=headers.find(0x000003FF);
    if(format==headers.end() || format->second.size()<8)throw std::runtime_error("Missing XEX encoding information");
    const auto encoding=Word(format->second,4),encryption=encoding>>16,compression=encoding&65535;
    if(encryption>1 || compression>2 || (compression==2 && format->second.size()<36) ||
       (compression==1 && (format->second.size()-8)%8))throw std::runtime_error("Unsupported XEX encoding");
    const auto payload_size=bytes.size()-limit;
    if(encryption && payload_size%16)throw std::runtime_error("Truncated encrypted XEX payload");
    if(!compression && payload_size!=Word(bytes,security+4))throw std::runtime_error("Invalid uncompressed XEX payload size");
    if(compression==1){
        uint64_t encoded=0,decoded=0;
        for(size_t offset=8;offset<format->second.size();offset+=8){
            const auto data=Word(format->second,offset),zero=Word(format->second,offset+4);
            encoded+=data;decoded+=uint64_t(data)+zero;
        }
        if(encoded>payload_size || decoded>Word(bytes,security+4))throw std::runtime_error("Invalid basic-compression XEX block sizes");
    }
    if(compression==2){
        const auto first=Word(format->second,12);
        if(first<24 || first>payload_size)throw std::runtime_error("Invalid compressed XEX first block size");
    }
    for(const auto& [key,payload]:headers){
        // Retail region/media branding is not executable layout. Keep title,
        // version, imports, TLS, entry point and the exact decoded image checks.
        if(key==0x000003FF || key==0x00040310 || key==0x000405FF || key==0x000183FF)continue;
        const auto executable_payload=key==0x00040006?payload.subspan(4):payload;
        Number(profile,key);Number(profile,uint32_t(executable_payload.size()));profile.insert(profile.end(),executable_payload.begin(),executable_payload.end());
    }
    return Digest(profile);
}
}
bool VerifyCompatibleGameXex(rex::Runtime& runtime,const std::filesystem::path& path,std::string& error){
    error.clear();
    std::string container_hash;
    try{
        const auto size=std::filesystem::file_size(path);
        if(size<24 || size>64*1024*1024)throw std::runtime_error("Invalid XEX file size");
        Bytes bytes(size);std::ifstream stream(path,std::ios::binary);
        if(!stream.read(reinterpret_cast<char*>(bytes.data()),bytes.size()))throw std::runtime_error("Cannot read default.xex");
        container_hash=Digest(bytes);
        if(Profile(bytes)!=kTranslatedGameProfile)throw std::runtime_error("The executable's imports or execution layout differ from this build");
        rex::runtime::XexModule probe(runtime.processor(),runtime.kernel_state());
        struct Unload {rex::runtime::XexModule& module;~Unload(){module.Unload();}} unload{probe};
        if(!probe.Load("beetleworx-build-validation","game:\\default.xex",bytes.data(),bytes.size()))
            throw std::runtime_error("Could not decode default.xex");
        if(probe.base_address()!=kTranslatedGameBase || probe.xex_security_info()->image_size!=kTranslatedGameSize)
            throw std::runtime_error("The decoded executable uses a different address layout");
        const auto* image=runtime.memory()->TranslateVirtual<uint8_t*>(kTranslatedGameBase);
        if(Digest({image,kTranslatedGameSize})!=kTranslatedGameImage)
            throw std::runtime_error("The decoded executable contains different code or data");
        REXLOG_INFO("XEX compatibility verified: exact decoded image, imports, TLS and execution layout; container SHA-256 {}",container_hash);
        return true;
    }catch(const std::exception& exception){
        error=exception.what();REXLOG_WARN("XEX compatibility rejected: container SHA-256 {}, reason {}",container_hash.empty()?"unavailable":container_hash,error);return false;
    }
}
bool TestCompatibleGameXex(rex::Runtime& runtime,const std::filesystem::path& root,std::string& error){
    for(const auto& [name,expected]:std::initializer_list<std::pair<const char*,bool>>{
        {"original.xex",true},{"same-image.xex",true},{"regional-metadata.xex",true},{"wrong-title.xex",false},{"altered-image.xex",false},
        {"altered-entry.xex",false},{"altered-imports.xex",false},{"truncated.xex",false}}){
        std::string reason;const auto accepted=VerifyCompatibleGameXex(runtime,root/name,reason);
        if(accepted!=expected){error=std::string(name)+": unexpected compatibility result: "+reason;return false;}
        REXLOG_INFO("XEX compatibility case {}: {} ({})",name,accepted?"accepted":"rejected",reason);
    }
    if(XFAILED(runtime.LoadXexImage("game:\\default.xex"))){error="Normal XEX loading failed after compatibility probes";return false;}
    REXLOG_INFO("Normal XEX load after compatibility probes passed; no guest execution");
    return true;
}
}

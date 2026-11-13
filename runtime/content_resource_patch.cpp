#include "content_resource_patch.h"
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <stdexcept>
#include <rex/logging.h>
namespace em2 {namespace {
using Bytes=std::vector<uint8_t>;
constexpr size_t limit=64*1024*1024;
void Need(bool b,const char* why){if(!b)throw std::runtime_error(why);}
uint32_t Word(std::span<const uint8_t> b,size_t at,bool big=false){Need(at<=b.size() && b.size()-at>=4,"Truncated resource patch/archive");uint32_t n=0;for(unsigned i=0;i<4;++i)n|=uint32_t(b[at+i])<<(big?(3-i)*8:i*8);return n;}
void Put(Bytes& b,uint32_t n,bool big=false){for(unsigned i=0;i<4;++i)b.push_back(uint8_t(n>>(big?(3-i)*8:i*8)));}
std::string Key(std::string s){for(auto& c:s){if(c=='\\')c='/';if(c>='A' && c<='Z')c+=32;}return s;}
struct Resource {std::string path;uint32_t size=0;std::array<uint8_t,4> kind{};Bytes packed;};
std::vector<Resource> ReadPak(std::span<const uint8_t> b){
    Need(b.size()>=36 && b.size()<=limit,"Invalid regional archive size");
    const bool big=std::equal(b.begin(),b.begin()+4," KAP");Need(big || std::equal(b.begin(),b.begin()+4,"PAK "),"Regional game archive is not an EM2 PAK");
    Need(Word(b,4,big)==2 && Word(b,8,big)==0,"Unsupported regional PAK version");
    const auto header=Word(b,12,big);const uint64_t payload_start=uint64_t(header)+Word(b,16,big);
    const auto count=Word(b,header,big);const uint64_t strings=uint64_t(header)+4+uint64_t(count)*24;
    Need(count<=100000 && header>=32 && strings<=payload_start && payload_start<=b.size(),"Invalid regional PAK directory");
    auto string=[&](uint32_t offset){const auto start=strings+offset;Need(start<payload_start,"Invalid regional PAK name offset");
        const auto end=std::find(b.begin()+size_t(start),b.begin()+size_t(payload_start),uint8_t(0));
        Need(end!=b.begin()+size_t(payload_start) && size_t(end-(b.begin()+size_t(start)))<=2048,"Invalid regional PAK name");
        return std::string(b.begin()+size_t(start),end);};
    std::vector<Resource> resources;std::set<std::string> names;uint64_t payload=payload_start;
    for(unsigned i=0;i<count;++i){const auto at=size_t(header)+4+i*24;Resource r;r.size=Word(b,at,big);
        const auto packed=Word(b,at+4,big),aligned=Word(b,at+8,big);auto folder=string(Word(b,at+12,big)),name=string(Word(b,at+20,big));
        r.path=folder.empty()?name:folder+"/"+name;Need(!name.empty() && names.insert(Key(r.path)).second,"Ambiguous regional PAK asset names");
        Need(r.size<=limit && packed<=aligned && !(aligned%32) && payload<=b.size() && aligned<=b.size()-payload,"Invalid regional PAK payload");
        std::copy_n(b.begin()+at+16,4,r.kind.begin());r.packed.assign(b.begin()+size_t(payload),b.begin()+size_t(payload)+packed);payload+=aligned;resources.push_back(std::move(r));
    }return resources;
}
Bytes WritePak(const std::vector<Resource>& resources){
    Need(resources.size()<=100000,"Too many patched regional assets");Bytes strings,entries;
    uint64_t total_payload=0;
    for(const auto& r:resources){const auto slash=r.path.rfind('/');const auto folder=slash==r.path.npos?std::string{}:r.path.substr(0,slash),name=slash==r.path.npos?r.path:r.path.substr(slash+1);
        Need(strings.size()+r.path.size()+2<=limit,"Regional PAK name budget exceeded");const auto directory=uint32_t(strings.size());strings.insert(strings.end(),folder.begin(),folder.end());strings.push_back(0);
        const auto filename=uint32_t(strings.size());strings.insert(strings.end(),name.begin(),name.end());strings.push_back(0);
        const auto aligned=(r.packed.size()+31)&~size_t(31);total_payload+=aligned;
        Put(entries,r.size,true);Put(entries,uint32_t(r.packed.size()),true);Put(entries,uint32_t(aligned),true);Put(entries,directory,true);entries.insert(entries.end(),r.kind.begin(),r.kind.end());Put(entries,filename,true);
    }
    const uint64_t payload=(36+entries.size()+strings.size()+31)&~uint64_t(31);Need(payload+total_payload<=limit,"Patched regional archive exceeds size limit");
    Bytes out={' ','K','A','P'};Put(out,2,true);Put(out,0,true);Put(out,32,true);Put(out,uint32_t(payload-32),true);out.resize(32);
    Put(out,uint32_t(resources.size()),true);out.insert(out.end(),entries.begin(),entries.end());out.insert(out.end(),strings.begin(),strings.end());out.resize(size_t(payload));
    for(const auto& r:resources){out.insert(out.end(),r.packed.begin(),r.packed.end());out.resize((out.size()+31)&~size_t(31));}return out;
}
}
std::vector<uint8_t> ApplyRegionalResourcePatch(std::span<const uint8_t> original,std::span<const uint8_t> patch){
    Need(patch.size()>=12 && patch.size()<=limit && std::equal(patch.begin(),patch.begin()+8,"EM2RP01\0"),"Invalid regional resource patch");
    auto resources=ReadPak(original);const auto count=Word(patch,8);Need(count<=100000,"Regional patch record limit exceeded");size_t at=12;
    struct Edit {uint32_t mode;Resource resource;std::string before;};std::map<std::string,Edit> edits;std::vector<std::string> additions;
    auto word=[&]{auto n=Word(patch,at);at+=4;return n;};
    auto text=[&]{const auto n=word();Need(n<=2048 && at<=patch.size() && n<=patch.size()-at,"Invalid regional patch name");std::string s(patch.begin()+at,patch.begin()+at+n);at+=n;Need(s.find('\0')==s.npos,"NUL in regional patch name");return s;};
    for(unsigned i=0;i<count;++i){Edit edit;edit.mode=word();edit.resource.path=text();edit.before=Key(text());edit.resource.size=word();const auto packed=word();
        Need(edit.mode<=2 && !edit.resource.path.empty() && edit.resource.size<=limit && at<=patch.size() && patch.size()-at>=4 && packed<=patch.size()-at-4,"Invalid regional patch record");
        std::copy_n(patch.begin()+at,4,edit.resource.kind.begin());at+=4;edit.resource.packed.assign(patch.begin()+at,patch.begin()+at+packed);at+=packed;
        Need((edit.mode==1)==!edit.before.empty(),"Missing regional addition anchor");const auto key=Key(edit.resource.path);
        if(edit.mode==1)additions.push_back(key);Need(edits.emplace(key,std::move(edit)).second,"Duplicate regional resource edit");
    }
    Need(at==patch.size(),"Trailing regional resource patch bytes");std::set<std::string> present;for(const auto& r:resources)present.insert(Key(r.path));
    for(const auto& [name,e]:edits)Need(e.mode==1?(!present.contains(name) && (e.before=="*" || present.contains(e.before))):present.contains(name),"Regional archive is missing an asset required by this update");
    std::vector<Resource> result;
    auto add=[&](std::string_view before){for(const auto& name:additions)if(edits.at(name).before==before)result.push_back(edits.at(name).resource);};
    for(auto& r:resources){const auto key=Key(r.path);add(key);const auto edit=edits.find(key);
        if(edit==edits.end())result.push_back(std::move(r));else if(edit->second.mode==0)result.push_back(edit->second.resource);}
    add("*");return WritePak(result);
}
bool TestRegionalResourcePatches(){
    try{Resource menu{"UI/menu.gfx",3,{'G','F','X',0},{1,2,3}},localized{"text/french.bin",2,{'B','I','N',0},{9,8}},actor{"mickey.hkw",1,{'H','K','W',0},{7}};
        const auto original=WritePak({menu,localized,actor});Bytes patch={'E','M','2','R','P','0','1',0};Put(patch,2);
        auto text=[&](std::string_view s){Put(patch,uint32_t(s.size()));patch.insert(patch.end(),s.begin(),s.end());};
        auto edit=[&](uint32_t mode,std::string_view name,std::string_view before,std::span<const uint8_t> bytes){Put(patch,mode);text(name);text(before);Put(patch,uint32_t(bytes.size()));Put(patch,uint32_t(bytes.size()));patch.insert(patch.end(),{'B','I','N',0});patch.insert(patch.end(),bytes.begin(),bytes.end());};
        const std::array<uint8_t,2> replacement{4,5},clip{6,7};edit(0,"ui/MENU.gfx","",replacement);edit(1,"animations/dash.hkx","mickey.hkw",clip);
        const auto output=ReadPak(ApplyRegionalResourcePatch(original,patch));
        Need(output.size()==4 && output[0].packed==Bytes({4,5}) && output[1].path==localized.path && output[1].packed==localized.packed &&
            output[2].path=="animations/dash.hkx" && output[3].path==actor.path,"Regional merge lost localized bytes or animation dependency order");
        for(size_t n=0;n<patch.size();++n){bool rejected=false;try{ApplyRegionalResourcePatch(original,std::span(patch).first(n));}catch(...){rejected=true;}Need(rejected,"Truncated regional patch accepted");}
        auto bad=original;bad[16]=255;bool rejected=false;try{ApplyRegionalResourcePatch(bad,patch);}catch(...){rejected=true;}Need(rejected,"Malformed regional PAK accepted");
        REXLOG_INFO("Regional resource patches passed localized-byte preservation, replacement case handling, insertion before HKW, every truncation and malformed archive checks");return true;
    }catch(const std::exception& e){REXLOG_ERROR("Regional patch test: {}",e.what());return false;}
}
}

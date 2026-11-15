#include "online_projectile_impact.h"
#include <rex/logging.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <set>
namespace em2::net {namespace {
bool Id(const SpawnId& id){return std::any_of(id.begin(),id.end(),[](auto n){return n!=0;});}
void Put(std::vector<uint8_t>& b,uint32_t n,unsigned count=4){for(unsigned i=count;i;--i)b.push_back(uint8_t(n>>((i-1)*8)));}
struct Reader {std::span<const uint8_t> bytes;size_t at=0;bool good=true;uint32_t Get(unsigned n=4){if(!good || n>bytes.size()-at){good=false;return 0;}uint32_t v=0;while(n--)v=(v<<8)|bytes[at++];return v;}};
}
bool ValidProjectileImpact(const ProjectileImpact& v){
    if(!Id(v.projectile) || v.clones.size()>kMaxImpactClones)return false;
    for(auto p:v.position){const auto f=std::bit_cast<float>(p);if(!std::isfinite(f) || std::abs(f)>1e7f)return false;}
    std::set<SpawnId> ids;for(const auto& c:v.clones)if(!Id(c.prototype) || !Id(c.entity) || c.flags[0]>1 || c.flags[1]>1 || !ids.insert(c.entity).second)return false;
    return true;
}
std::vector<uint8_t> EncodeProjectileImpact(const ProjectileImpact& v){
    if(!ValidProjectileImpact(v))return {};
    std::vector<uint8_t> b{'B','W','P','I',1};for(auto word:v.projectile)Put(b,word);for(auto word:v.position)Put(b,word);
    Put(b,v.random_before);Put(b,v.random_after);Put(b,v.callback,1);Put(b,v.queued,1);Put(b,uint32_t(v.clones.size()),1);
    for(const auto& c:v.clones){for(const auto* id:{&c.prototype,&c.entity})for(auto word:*id)Put(b,word);for(auto flag:c.flags)Put(b,flag,1);Put(b,c.random_before);Put(b,c.random_after);}
    return b;
}
bool DecodeProjectileImpact(std::span<const uint8_t> bytes,ProjectileImpact& out){
    if(bytes.size()>kMaxImpactBytes)return false;Reader r{bytes};ProjectileImpact v;
    if(r.Get()!=0x42575049 || r.Get(1)!=1)return false;
    for(auto& word:v.projectile)word=r.Get();for(auto& word:v.position)word=r.Get();v.random_before=r.Get();v.random_after=r.Get();
    const auto callback=r.Get(1),queued=r.Get(1),count=r.Get(1);
    if(!r.good || callback>1 || queued>1 || count>kMaxImpactClones || bytes.size()-r.at!=size_t(count)*42)return false;v.callback=callback!=0;v.queued=queued!=0;
    for(unsigned i=0;i<count;++i){ImpactClone c;for(auto* id:{&c.prototype,&c.entity})for(auto& word:*id)word=r.Get();for(auto& flag:c.flags)flag=r.Get(1);c.random_before=r.Get();c.random_after=r.Get();v.clones.push_back(c);}
    if(!r.good || !ValidProjectileImpact(v))return false;out=std::move(v);return true;
}
bool TestProjectileImpactCodec(){
    unsigned checks=0;auto check=[&](bool ok,const char* why){++checks;if(!ok)REXLOG_ERROR("Projectile impact codec: {}",why);return ok;};
    ProjectileImpact v;v.projectile={1,2,3,4};v.position={0x80000000,0x3f800001,0};v.random_before=0xFFFFFFFF;v.random_after=0x11223344;v.queued=true;
    v.clones.push_back({{0x1300dc,0,0,0},{5,6,7,8},{1,1},0x7184540C,0x9F646079});
    auto bytes=EncodeProjectileImpact(v);ProjectileImpact out;
    if(!check(!bytes.empty() && DecodeProjectileImpact(bytes,out) && out==v,"projectile, exact position bits, RNG, retirement and FX clone inputs round-trip"))return false;
    const auto sentinel=out;
    for(size_t n=0;n<bytes.size();++n)if(!check(!DecodeProjectileImpact(std::span(bytes).first(n),out) && out==sentinel,"every truncated prefix leaves destination unchanged"))return false;
    for(unsigned mutation=0;mutation<9;++mutation){auto bad=bytes;
        if(mutation==0)bad[0]^=1;if(mutation==1)bad[4]=2;if(mutation==2)bad[41]=2;if(mutation==3)bad[42]=2;if(mutation==4)bad[43]=17;
        if(mutation==5){bad[21]=0x7f;bad[22]=0x80;bad[23]=bad[24]=0;}
        if(mutation==6)bad.push_back(0);if(mutation==7)bad.resize(kMaxImpactBytes+1);if(mutation==8)std::fill(bad.begin()+5,bad.begin()+21,0);
        if(!check(!DecodeProjectileImpact(bad,out) && out==sentinel,"malformed identity/float/format/boolean/count/size rejected transactionally"))return false;
    }
    v.clones.clear();for(unsigned i=0;i<kMaxImpactClones;++i)v.clones.push_back({{7,0,0,0},{i+1,9,10,11},{1,1},i,i+1});v.callback=true;
    if(!check(EncodeProjectileImpact(v).size()<=kMaxImpactBytes && DecodeProjectileImpact(EncodeProjectileImpact(v),out) && out==v,"maximum native clone inventory fits the bounded impact envelope"))return false;
    v.clones[1].entity=v.clones[0].entity;if(!check(EncodeProjectileImpact(v).empty(),"duplicate FX identities rejected"))return false;
    REXLOG_INFO("Projectile impact codec passed {} identity, native input, clone and malformed-payload checks; native replay is separate",checks);return true;
}
}

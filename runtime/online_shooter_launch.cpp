#include "online_shooter_launch.h"
#include <rex/logging.h>
#include <algorithm>
#include <set>
namespace em2::net {namespace {
bool Id(const SpawnId& v){return std::any_of(v.begin(),v.end(),[](auto n){return n!=0;});}
void Put(std::vector<uint8_t>& b,uint32_t n,unsigned count=4){for(unsigned i=count;i;--i)b.push_back(uint8_t(n>>((i-1)*8)));}
struct Reader {std::span<const uint8_t> bytes;size_t at=0;bool good=true;uint32_t Get(unsigned n=4){if(!good || n>bytes.size()-at){good=false;return 0;}uint32_t v=0;while(n--)v=(v<<8)|bytes[at++];return v;}};
}
bool ValidShooterLaunch(const ShooterLaunch& v){
    if(v.owner.peer>16 || (bool(v.owner.peer)==Id(v.owner.id)) || v.projectiles.empty() || v.projectiles.size()>kMaxSpawnChildren || v.clones.size()>kMaxImpactClones)return false;
    std::set<SpawnId> ids;for(const auto& id:v.projectiles)if(!Id(id) || !ids.insert(id).second)return false;
    for(const auto& c:v.clones)if(!Id(c.prototype) || !Id(c.entity) || c.flags[0]>1 || c.flags[1]>1 || !ids.insert(c.entity).second)return false;
    return true;
}
std::vector<uint8_t> EncodeShooterLaunch(const ShooterLaunch& v){
    if(!ValidShooterLaunch(v))return {};
    std::vector<uint8_t> b{'B','W','S','L',1};Put(b,v.owner.peer,1);for(auto word:v.owner.id)Put(b,word);
    Put(b,v.random_before);Put(b,v.random_after);Put(b,v.success,1);Put(b,uint32_t(v.projectiles.size()),1);for(const auto& id:v.projectiles)for(auto word:id)Put(b,word);
    Put(b,uint32_t(v.clones.size()),1);for(const auto& c:v.clones){for(const auto* id:{&c.prototype,&c.entity})for(auto word:*id)Put(b,word);for(auto flag:c.flags)Put(b,flag,1);Put(b,c.random_before);Put(b,c.random_after);}return b;
}
bool DecodeShooterLaunch(std::span<const uint8_t> bytes,ShooterLaunch& out){
    if(bytes.size()>kMaxLaunchBytes)return false;Reader r{bytes};ShooterLaunch v;
    if(r.Get()!=0x4257534c || r.Get(1)!=1)return false;v.owner.peer=uint8_t(r.Get(1));for(auto& word:v.owner.id)word=r.Get();
    v.random_before=r.Get();v.random_after=r.Get();const auto success=r.Get(1),count=r.Get(1);
    if(!r.good || success>1 || !count || count>kMaxSpawnChildren || bytes.size()-r.at<size_t(count)*16+1)return false;v.success=success!=0;
    for(unsigned i=0;i<count;++i){SpawnId id;for(auto& word:id)word=r.Get();v.projectiles.push_back(id);}
    const auto clones=r.Get(1);if(!r.good || clones>kMaxImpactClones || bytes.size()-r.at!=size_t(clones)*42)return false;
    for(unsigned i=0;i<clones;++i){ImpactClone c;for(auto* id:{&c.prototype,&c.entity})for(auto& word:*id)word=r.Get();for(auto& flag:c.flags)flag=r.Get(1);c.random_before=r.Get();c.random_after=r.Get();v.clones.push_back(c);}
    if(!r.good || !ValidShooterLaunch(v))return false;out=std::move(v);return true;
}
bool TestShooterLaunchCodec(){
    unsigned checks=0;auto check=[&](bool ok,const char* why){++checks;if(!ok)REXLOG_ERROR("Shooter launch codec: {}",why);return ok;};
    ShooterLaunch v;v.owner.peer=16;v.random_before=0xFEDCBA98;v.random_after=0x12345678;v.success=true;v.projectiles={{1,2,3,4},{5,6,7,8}};v.clones.push_back({{9,0,0,0},{10,11,12,13},{1,1},14,15});
    const auto bytes=EncodeShooterLaunch(v);ShooterLaunch out;
    if(!check(!bytes.empty() && DecodeShooterLaunch(bytes,out) && out==v,"stable owner, pending-shot order, return result, RNG and FX inputs round-trip"))return false;const auto sentinel=out;
    for(size_t n=0;n<bytes.size();++n)if(!check(!DecodeShooterLaunch(std::span(bytes).first(n),out) && out==sentinel,"every truncated prefix is transactional"))return false;
    for(unsigned mutation=0;mutation<9;++mutation){auto bad=bytes;
        if(mutation==0)bad[0]^=1;if(mutation==1)bad[4]=2;if(mutation==2)bad[5]=17;if(mutation==3)bad[30]=2;if(mutation==4)bad[31]=0;if(mutation==5)bad[31]=65;
        if(mutation==6)bad.push_back(0);if(mutation==7)bad.resize(kMaxLaunchBytes+1);if(mutation==8)bad[6]=1;
        if(!check(!DecodeShooterLaunch(bad,out) && out==sentinel,"malformed owner/boolean/count/format/size rejected without writes"))return false;
    }
    v.owner.peer=0;v.owner.id={1,2,3,4};v.projectiles.clear();for(unsigned i=0;i<kMaxSpawnChildren;++i)v.projectiles.push_back({i+1,2,3,4});
    v.clones.clear();for(unsigned i=0;i<kMaxImpactClones;++i)v.clones.push_back({{9,0,0,0},{i+1,5,6,7},{1,1},i,i+1});v.success=false;
    if(!check(EncodeShooterLaunch(v).size()<=kMaxLaunchBytes && DecodeShooterLaunch(EncodeShooterLaunch(v),out) && out==v,"maximum ordered pending list and FX inventory preserve failed native result"))return false;
    v.projectiles[1]=v.projectiles[0];if(!check(EncodeShooterLaunch(v).empty(),"duplicate pending entities rejected"))return false;
    REXLOG_INFO("Shooter launch codec passed {} ordered identity, native result, boundary and malformed-payload checks; native execution is separate",checks);return true;
}
}

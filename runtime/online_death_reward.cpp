#include "online_death_reward.h"
#include <rex/logging.h>
#include <algorithm>
#include <set>
namespace em2::net {namespace {
bool Id(const SpawnId& id){return std::any_of(id.begin(),id.end(),[](auto n){return n!=0;});}
void Put(std::vector<uint8_t>& b,uint32_t n,unsigned count=4){for(unsigned i=count;i;--i)b.push_back(uint8_t(n>>((i-1)*8)));}
struct Reader {std::span<const uint8_t> bytes;size_t at=0;bool good=true;uint32_t Get(unsigned n=4){if(!good || n>bytes.size()-at){good=false;return 0;}uint32_t v=0;while(n--)v=(v<<8)|bytes[at++];return v;}};
}
bool ValidDeathReward(const DeathReward& v){
    if(!Id(v.owner) || v.clones.size()>kMaxImpactClones || v.parent_before==v.owner || v.parent_after==v.owner)return false;
    std::set<SpawnId> ids;
    for(const auto& c:v.clones)if(!Id(c.prototype) || !Id(c.entity) || c.entity==v.owner || c.flags[0]>1 || c.flags[1]>1 || !ids.insert(c.entity).second)return false;
    if(Id(v.parent_after) && !ids.contains(v.parent_after))return false;
    return true;
}
std::vector<uint8_t> EncodeDeathReward(const DeathReward& v){
    if(!ValidDeathReward(v))return {};
    std::vector<uint8_t> b{'B','W','D','R',2};for(auto n:v.owner)Put(b,n);Put(b,v.reason);
    for(const auto* state:{&v.before,&v.after})for(auto n:*state)Put(b,n);
    for(const auto* id:{&v.parent_before,&v.parent_after})for(auto n:*id)Put(b,n);
    Put(b,v.random_before);Put(b,v.random_after);Put(b,uint32_t(v.clones.size()),1);
    for(const auto& c:v.clones){for(const auto* id:{&c.prototype,&c.entity})for(auto n:*id)Put(b,n);for(auto n:c.flags)Put(b,n,1);Put(b,c.random_before);Put(b,c.random_after);}
    return b;
}
bool DecodeDeathReward(std::span<const uint8_t> bytes,DeathReward& out){
    if(bytes.size()>kMaxDeathRewardBytes)return false;Reader r{bytes};DeathReward v;
    if(r.Get()!=0x42574452 || r.Get(1)!=2)return false;
    for(auto& n:v.owner)n=r.Get();v.reason=r.Get();for(auto* state:{&v.before,&v.after})for(auto& n:*state)n=r.Get();
    for(auto* id:{&v.parent_before,&v.parent_after})for(auto& n:*id)n=r.Get();
    v.random_before=r.Get();v.random_after=r.Get();const auto count=r.Get(1);
    if(!r.good || count>kMaxImpactClones || bytes.size()-r.at!=size_t(count)*42)return false;
    for(unsigned i=0;i<count;++i){ImpactClone c;for(auto* id:{&c.prototype,&c.entity})for(auto& n:*id)n=r.Get();for(auto& n:c.flags)n=r.Get(1);c.random_before=r.Get();c.random_after=r.Get();v.clones.push_back(c);}
    if(!r.good || !ValidDeathReward(v))return false;out=std::move(v);return true;
}
bool TestDeathRewardCodec(){
    unsigned checks=0;auto check=[&](bool ok,const char* why){++checks;if(!ok)REXLOG_ERROR("Death reward codec: {}",why);return ok;};
    DeathReward v;v.owner={0x151a87,0,0,0};v.reason=0xFFFFFFFF;v.before={1,2,3};v.after={5,6,7};v.random_before=0x7184540C;v.random_after=0x9F646079;
    v.clones.push_back({{0x636d0,0,0,0},{0x7720,0x7b2f,0x602f,0x7656},{1,1},0x12345678,0x90abcdef});
    v.parent_before={99,3,2,1};v.parent_after=v.clones.front().entity;
    const auto bytes=EncodeDeathReward(v);DeathReward out;
    if(!check(!bytes.empty() && DecodeDeathReward(bytes,out) && out==v,"native reason/state/RNG/ordered complete clone inputs round-trip"))return false;
    const auto sentinel=out;
    for(size_t n=0;n<bytes.size();++n)if(!check(!DecodeDeathReward(std::span(bytes).first(n),out) && out==sentinel,"truncation is transactional"))return false;
    for(unsigned mutation=0;mutation<9;++mutation){auto bad=bytes;
        if(mutation==0)bad[0]^=1;if(mutation==1)bad[4]=1;if(mutation==2)std::fill(bad.begin()+5,bad.begin()+21,0);
        if(mutation==3)bad[89]=17;if(mutation==4)bad[122]=2;if(mutation==5)bad.push_back(0);
        if(mutation==6)bad.resize(kMaxDeathRewardBytes+1);if(mutation==7)std::fill(bad.begin()+90,bad.begin()+106,0);
        if(mutation==8)bad[65]^=1;
        if(!check(!DecodeDeathReward(bad,out) && out==sentinel,"malformed envelope, ID, count, clone flag and size rejected"))return false;
    }
    v.clones.clear();for(unsigned i=0;i<kMaxImpactClones;++i)v.clones.push_back({{7,0,0,0},{i+1,9,10,11},{1,1},i,i+1});v.parent_after=v.clones.front().entity;
    if(!check(EncodeDeathReward(v).size()<=kMaxDeathRewardBytes && DecodeDeathReward(EncodeDeathReward(v),out) && out==v,"maximum clone list is bounded"))return false;
    v.clones[1].entity=v.clones[0].entity;if(!check(EncodeDeathReward(v).empty(),"duplicate clone rejected"))return false;
    v.clones[1].entity=v.owner;if(!check(EncodeDeathReward(v).empty(),"owner cannot alias a newly born clone"))return false;
    v.clones.clear();v.parent_after={};if(!check(DecodeDeathReward(EncodeDeathReward(v),out) && out==v,"zero-birth reward preserves native one-shot operation"))return false;
    v.parent_after={1,0,0,0};if(!check(EncodeDeathReward(v).empty(),"unbound reward parent rejected"))return false;
    REXLOG_INFO("Death reward codec passed {} checks; native execution and network integration are separate",checks);return true;
}
}

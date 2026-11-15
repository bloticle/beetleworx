#include "online_spawn_batch.h"
#include <rex/logging.h>
#include <algorithm>
#include <bit>
#include <set>
namespace em2::net {
namespace {
bool Nonzero(const SpawnId& id){return std::any_of(id.begin(),id.end(),[](auto n){return n!=0;});}
bool Reference(const SpawnReference& ref,bool required){return ref.peer<=16 && !(ref.peer && Nonzero(ref.id)) && (!required || ref.peer || Nonzero(ref.id));}
void Put(std::vector<uint8_t>& bytes,uint32_t value,unsigned count=4){for(unsigned i=count;i;--i)bytes.push_back(uint8_t(value>>((i-1)*8)));}
struct Reader {
    std::span<const uint8_t> bytes;size_t at=0;bool good=true;
    uint32_t Get(unsigned count=4){if(!good || count>bytes.size()-at){good=false;return 0;}uint32_t n=0;while(count--)n=(n<<8)|bytes[at++];return n;}
};
}
bool ValidSpawnBatch(const SpawnBatch& batch){
    if(!Reference(batch.owner,true) || !Reference(batch.anchor,false) || batch.depth>15 || batch.children.size()>kMaxSpawnChildren ||
       (!batch.random_present && (batch.random_before || batch.random_after)))return false;
    std::set<SpawnId> ids;
    for(const auto& child:batch.children)
        if(!Nonzero(child.prototype) || !Nonzero(child.entity) || child.arguments[2]>1 || !ids.insert(child.entity).second)return false;
    // The original returns its last child, or zero when no child is produced.
    // Capture must fail on unsupported lifetime side effects instead of inventing
    // a different returned entity for the receiving caller.
    return batch.children.empty() ? !Nonzero(batch.result) : batch.result==batch.children.back().entity;
}
std::vector<uint8_t> EncodeSpawnBatch(const SpawnBatch& batch){
    if(!ValidSpawnBatch(batch))return {};
    std::vector<uint8_t> bytes{'B','W','S','B',2};
    Put(bytes,batch.depth,1);
    for(const auto* ref:{&batch.owner,&batch.anchor}){Put(bytes,ref->peer,1);for(auto word:ref->id)Put(bytes,word);}
    Put(bytes,std::bit_cast<uint32_t>(batch.selection));Put(bytes,batch.flag,1);Put(bytes,batch.collection,1);
    for(const auto* state:{&batch.before,&batch.after})for(auto word:*state)Put(bytes,word);
    Put(bytes,batch.random_present,1);Put(bytes,batch.random_before);Put(bytes,batch.random_after);
    for(auto word:batch.result)Put(bytes,word);
    Put(bytes,uint32_t(batch.children.size()),1);
    Put(bytes,batch.shooter,1);
    for(const auto& child:batch.children){for(const auto* id:{&child.prototype,&child.entity})for(auto word:*id)Put(bytes,word);for(auto word:child.arguments)Put(bytes,word);}
    return bytes;
}
bool DecodeSpawnBatch(std::span<const uint8_t> bytes,SpawnBatch& out){
    if(bytes.size()>kMaxSpawnBatchBytes)return false;
    Reader r{bytes};SpawnBatch batch;
    if(r.Get()!=0x42575342 || r.Get(1)!=2)return false;
    batch.depth=uint8_t(r.Get(1));
    for(auto* ref:{&batch.owner,&batch.anchor}){ref->peer=uint8_t(r.Get(1));for(auto& word:ref->id)word=r.Get();}
    batch.selection=std::bit_cast<int32_t>(r.Get());const auto flag=r.Get(1),collection=r.Get(1);
    if(flag>1 || collection>1)return false;batch.flag=flag!=0;batch.collection=collection!=0;
    for(auto* state:{&batch.before,&batch.after})for(auto& word:*state)word=r.Get();
    const auto random=r.Get(1);if(random>1)return false;batch.random_present=random!=0;
    batch.random_before=r.Get();batch.random_after=r.Get();for(auto& word:batch.result)word=r.Get();
    const auto count=r.Get(1),shooter=r.Get(1);if(!r.good || shooter>1 || count>kMaxSpawnChildren || bytes.size()-r.at!=size_t(count)*44)return false;
    batch.shooter=shooter!=0;
    for(unsigned i=0;i<count;++i){SpawnChild child;for(auto* id:{&child.prototype,&child.entity})for(auto& word:*id)word=r.Get();for(auto& word:child.arguments)word=r.Get();batch.children.push_back(child);}
    if(!r.good || r.at!=bytes.size() || !ValidSpawnBatch(batch))return false;
    out=std::move(batch);return true;
}
SpawnEligibility EvaluateSpawnEligibility(std::span<const SpawnBatch> host,std::span<const SpawnBatch> completed,const SpawnReference& owner){
    if(!Reference(owner,true) || completed.size()>host.size() || !std::equal(completed.begin(),completed.end(),host.begin()))return SpawnEligibility::InvalidPrefix;
    for(const auto& batch:host.subspan(completed.size()))if(batch.owner==owner)return SpawnEligibility::Original;
    return SpawnEligibility::Suppress;
}
bool SpawnBeforeCleanupReady(std::span<const SpawnBatch> host,std::span<const SpawnBatch> completed,
    const SpawnReference& owner,const std::array<uint32_t,3>& state){
    if(EvaluateSpawnEligibility(host,completed,owner)!=SpawnEligibility::Original || completed.size()>=host.size())return false;
    const auto& next=host[completed.size()];
    return next.owner==owner && next.anchor==owner && next.shooter && !next.collection &&
        !next.depth && next.flag && next.random_present && next.before==state;
}
bool TestSpawnBatchCodec(){
    unsigned checks=0;auto check=[&](bool ok,const char* why){++checks;if(!ok)REXLOG_ERROR("Spawn batch codec: {}",why);return ok;};
    SpawnBatch batch;batch.owner.id={1,2,3,4};batch.anchor.peer=16;batch.selection=INT32_MIN;batch.depth=15;batch.flag=true;batch.collection=true;
    batch.before={3,1,0};batch.after={4,2,1};batch.random_present=true;batch.random_before=0x7184540C;batch.random_after=0x9F646079;
    batch.children.push_back({{0x69771,0,0,0},{0x4E10,0x5C3,0x7DD3,0x629B},{0,0,1}});batch.result=batch.children.back().entity;
    batch.shooter=true;auto bytes=EncodeSpawnBatch(batch);SpawnBatch out;
    if(!check(!bytes.empty() && DecodeSpawnBatch(bytes,out) && out==batch,"native parent inputs, stable player anchor, counters, CRT state and projectile identity round-trip"))return false;
    const auto sentinel=out;
    for(size_t n=0;n<bytes.size();++n)if(!check(!DecodeSpawnBatch(std::span(bytes).first(n),out) && out==sentinel,"every truncated prefix rejected without changing destination"))return false;
    for(unsigned mutation=0;mutation<12;++mutation){auto bad=bytes;
        if(mutation==0)bad[0]^=1;if(mutation==1)bad[4]=1;if(mutation==2)bad[5]=16;
        if(mutation==3)bad[6]=17;if(mutation==4)bad[6]=1; // Ambiguous owner GUID + peer.
        if(mutation==5)bad[44]=2;if(mutation==6)bad[45]=2;if(mutation==7)bad[70]=2;
        if(mutation==8)bad.push_back(0);if(mutation==9)bad.resize(kMaxSpawnBatchBytes+1);if(mutation==10)bad[95]=65;
        if(mutation==11)bad[96]=2;
        if(!check(!DecodeSpawnBatch(bad,out) && out==sentinel,"invalid version/depth/reference/boolean/count/size rejected transactionally"))return false;
    }
    batch.children.clear();batch.result={};batch.random_present=false;batch.random_before=batch.random_after=0;batch.selection=-1;
    if(!check(DecodeSpawnBatch(EncodeSpawnBatch(batch),out) && out==batch,"zero-birth parent operations preserve their bookkeeping and null result"))return false;
    for(unsigned i=0;i<kMaxSpawnChildren;++i)batch.children.push_back({{7,0,0,0},{i+1,0,0,0},{UINT32_MAX,UINT32_MAX,1}});
    batch.result=batch.children.back().entity;bytes=EncodeSpawnBatch(batch);
    if(!check(bytes.size()<=kMaxSpawnBatchBytes && DecodeSpawnBatch(bytes,out) && out==batch,"maximum distinct native children preserve all scalar bits"))return false;
    for(unsigned mutation=0;mutation<6;++mutation){auto bad=batch;
        if(mutation==0)bad.children.push_back(batch.children.front());
        if(mutation==1)bad.children[1].entity=bad.children[0].entity;
        if(mutation==2)bad.children[0].prototype={};
        if(mutation==3)bad.children[0].arguments[2]=2;
        if(mutation==4)bad.result={999,0,0,0};
        if(mutation==5)bad.random_after=1;
        if(!check(EncodeSpawnBatch(bad).empty(),"oversized/duplicate births, absent templates, unsupported flags/results/RNG rejected"))return false;
    }
    std::vector<SpawnBatch> host{batch},completed;
    if(!check(EvaluateSpawnEligibility(host,completed,batch.owner)==SpawnEligibility::Original,"owner with an unconsumed host operation retains original execution"))return false;
    SpawnReference other;other.id={999,2,3,4};
    if(!check(EvaluateSpawnEligibility(host,completed,other)==SpawnEligibility::Suppress,"guest-only owner is excluded without consuming another owner's operation"))return false;
    completed=host;
    if(!check(EvaluateSpawnEligibility(host,completed,batch.owner)==SpawnEligibility::Suppress,"extra call after the host's last operation is excluded"))return false;
    completed[0].selection^=1;
    if(!check(EvaluateSpawnEligibility(host,completed,batch.owner)==SpawnEligibility::InvalidPrefix,"an earlier mismatch cannot be concealed by suppressing a later call"))return false;
    completed=host;completed.push_back(batch);
    if(!check(EvaluateSpawnEligibility(host,completed,batch.owner)==SpawnEligibility::InvalidPrefix,"excess completed operations do not index past the host sequence"))return false;
    host[0].children.clear();host[0].result={};completed.clear();
    if(!check(EvaluateSpawnEligibility(host,completed,batch.owner)==SpawnEligibility::Original,"host zero-birth calls still execute their native counter and callback effects"))return false;
    host[0].owner={};host[0].owner.peer=16;
    if(!check(EvaluateSpawnEligibility(host,completed,host[0].owner)==SpawnEligibility::Original,"stable player ownership is independent of respawn-generated GUIDs"))return false;
    if(!check(EvaluateSpawnEligibility(host,completed,{})==SpawnEligibility::InvalidPrefix,"absent owner identity is never treated as a safe cancellation"))return false;
    SpawnBatch pending;pending.owner.id={1,2,3,4};pending.anchor=pending.owner;
    pending.shooter=true;pending.flag=true;pending.random_present=true;pending.before={2,2,1};pending.after={3,3,1};
    host={pending};completed.clear();
    if(!check(SpawnBeforeCleanupReady(host,completed,pending.owner,{2,2,1}),"missing Shooter birth admitted before cleanup at exact original pre-state"))return false;
    for(unsigned field=0;field<3;++field){auto changed=pending.before;changed[field]^=1;
        if(!check(!SpawnBeforeCleanupReady(host,completed,pending.owner,changed),"already changed slot/counter state cannot be overwritten by cleanup replay"))return false;}
    for(unsigned mutation=0;mutation<7;++mutation){auto changed=host;
        if(mutation==0)changed[0].owner.id[0]^=1;if(mutation==1)changed[0].anchor.id[0]^=1;
        if(mutation==2)changed[0].shooter=false;if(mutation==3)changed[0].collection=true;
        if(mutation==4)changed[0].depth=1;if(mutation==5)changed[0].flag=false;if(mutation==6)changed[0].random_present=false;
        if(!check(!SpawnBeforeCleanupReady(changed,completed,pending.owner,pending.before),"foreign owner/anchor, unaudited shape and missing RNG cannot enter cleanup replay"))return false;}
    completed=host;
    if(!check(!SpawnBeforeCleanupReady(host,completed,pending.owner,pending.before),"completed host birth cannot run twice before cleanup"))return false;
    host.push_back(pending);completed[0].selection^=1;
    if(!check(!SpawnBeforeCleanupReady(host,completed,pending.owner,pending.before),"earlier divergent parent cannot be concealed by a later cleanup replay"))return false;
    REXLOG_INFO("Spawn batch codec passed {} portable parent/child identity, zero-birth, scalar and malformed-payload checks; no native spawn replay assertion",checks);return true;
}
}

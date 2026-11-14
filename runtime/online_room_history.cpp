#include "online_room_history.h"
#include <rex/logging.h>
#include <algorithm>
#include <limits>
#include <utility>
namespace em2::net {
namespace {
bool Nonzero(const auto& values){return std::any_of(values.begin(),values.end(),[](auto value){return value!=0;});}
void Advance(uint64_t& tick,bool& exhausted){if(tick==UINT64_MAX)exhausted=true;else ++tick;}
}
bool RoomHistory::Reset(const RoomHistoryScope& scope){
    if(!Nonzero(scope.identity.build) || !Nonzero(scope.identity.content) || !Nonzero(scope.identity.checkpoint) || !Nonzero(scope.identity.session) ||
       !Nonzero(scope.world) || !scope.roster_revision || !(scope.players_mask&1) || std::popcount(scope.players_mask)<2 || scope.first_tick<scope.identity.first_tick)return false;
    std::vector<RoomHistoryEntry> fresh(Capacity);slots_.swap(fresh);scope_=scope;head_=count_=0;
    capture_=complete_=verify_=scope.first_tick;capture_exhausted_=complete_exhausted_=verify_exhausted_=false;return true;
}
const RoomHistoryEntry* RoomHistory::Find(uint64_t tick)const{
    if(!scope_ || !count_ || tick<slots_[head_].frame.tick)return nullptr;
    const auto offset=tick-slots_[head_].frame.tick;if(offset>=count_)return nullptr;
    const auto& entry=slots_[(head_+size_t(offset))%Capacity];return entry.frame.tick==tick?&entry:nullptr;
}
RoomHistoryEntry* RoomHistory::Find(uint64_t tick){return const_cast<RoomHistoryEntry*>(std::as_const(*this).Find(tick));}
std::optional<uint64_t> RoomHistory::oldest_tick()const{return count_?std::optional(slots_[head_].frame.tick):std::nullopt;}
bool RoomHistory::Capture(const RoomFrame& frame){
    if(!scope_ || capture_exhausted_ || frame.tick!=capture_ || frame.players_mask!=scope_->players_mask || (count_==Capacity && !slots_[head_].verified))return false;
    for(unsigned i=0;i<kOnlineRoomCapacity;++i){const auto& command=frame.players[i];
        if(frame.players_mask&(1u<<i)){if(!ValidRoomMovement(command) || command.pad.menu_pause)return false;}
        else if(command!=RoomMovement{})return false;
    }
    const auto at=(head_+count_)%Capacity;slots_[at]={frame,{},false};
    if(count_==Capacity)head_=(head_+1)%Capacity;else ++count_;
    Advance(capture_,capture_exhausted_);return true;
}
bool RoomHistory::Complete(uint64_t tick,const RoomHistoryHash& digest){
    auto* entry=Find(tick);if(!entry || complete_exhausted_ || tick!=complete_ || entry->completed || !Nonzero(digest))return false;
    entry->completed=digest;Advance(complete_,complete_exhausted_);return true;
}
bool RoomHistory::Verify(uint64_t tick,const RoomHistoryHash& digest){
    auto* entry=Find(tick);if(!entry || verify_exhausted_ || tick!=verify_ || !entry->completed || *entry->completed!=digest || entry->verified)return false;
    entry->verified=true;Advance(verify_,verify_exhausted_);return true;
}
RoomHistoryResult RoomHistory::Read(const RoomHistoryScope& scope,uint64_t first,uint64_t last,std::vector<RoomHistoryEntry>& out)const{
    if(!scope_)return RoomHistoryResult::NotStarted;if(scope!=*scope_)return RoomHistoryResult::WrongScope;
    if(first>last || last-first>=Capacity)return RoomHistoryResult::InvalidRange;
    if(count_ && first<slots_[head_].frame.tick)return RoomHistoryResult::Expired;
    for(uint64_t tick=first;;++tick){const auto* entry=Find(tick);if(!entry)return RoomHistoryResult::Unavailable;if(!entry->verified)return RoomHistoryResult::Unverified;if(tick==last)break;}
    std::vector<RoomHistoryEntry> result;result.reserve(size_t(last-first)+1);
    for(uint64_t tick=first;;++tick){result.push_back(*Find(tick));if(tick==last)break;}out.swap(result);return RoomHistoryResult::Ready;
}
bool TestOnlineRoomHistory(){
    bool ok=true;unsigned checks=0;auto check=[&](bool v,const char* why){++checks;if(!v){ok=false;REXLOG_ERROR("Room input history: {}",why);}return v;};
    RoomHistoryScope scope;scope.identity.build[0]=1;scope.identity.content[0]=2;scope.identity.checkpoint[0]=3;scope.identity.session[0]=4;scope.world[0]=5;scope.roster_revision=9;scope.first_tick=UINT64_C(1)<<40;scope.players_mask=0xFFFF;
    RoomHistory history;std::vector<RoomHistoryEntry> out(1);const auto original=out;
    check(history.Read(scope,0,0,out)==RoomHistoryResult::NotStarted && out==original,"no implicit history before native activation");
    check(history.Reset(scope),"complete world/session scope activates history");
    auto frame=[&](uint64_t tick){RoomFrame f;f.tick=tick;f.players_mask=scope.players_mask;for(unsigned i=0;i<16;++i){auto& c=f.players[i];c.actor_epoch=i+1;c.source=RoomMovementSource::NativeActionV3;c.jump_button_mask=0x1000;c.button_actions.schema[0]=uint8_t(i+1);c.button_actions.source_tick=tick-2;c.button_actions.fired[i]=0x81;c.pad.left_x=int16_t(-30000+i);c.x=-0.f;}return f;};
    RoomHistoryHash proof{};proof[0]=37;auto first=frame(scope.first_tick);
    check(!history.Complete(first.tick,proof) && !history.Verify(first.tick,proof),"neither proof nor acknowledgement invents consumed input");
    check(history.Capture(first) && !history.Capture(first) && history.Read(scope,first.tick,first.tick,out)==RoomHistoryResult::Unverified && out==original,"consumption is exact once and cannot masquerade as verified input");
    auto wrong=proof;wrong[1]=1;check(history.Complete(first.tick,proof) && !history.Complete(first.tick,proof) && !history.Verify(first.tick,wrong) && history.Verify(first.tick,proof),"matching immutable local completion and ordered acknowledgement both required");
    check(history.Read(scope,first.tick,first.tick,out)==RoomHistoryResult::Ready && out[0].frame==first && std::bit_cast<uint32_t>(out[0].frame.players[0].x)==0x80000000 && out[0].completed==proof,"all sixteen V3 decisions, native basis, source clocks, axes and signed zero retained exactly");
    out[0].frame.players[0].x=1;check(history.Read(scope,first.tick,first.tick,out)==RoomHistoryResult::Ready && out[0].frame==first,"caller cannot mutate retained input");
    const auto saved=out;for(unsigned field=0;field<4;++field){auto other=scope;if(field==0)++other.identity.session[0];if(field==1)++other.world[0];if(field==2)++other.roster_revision;if(field==3)other.players_mask=3;check(history.Read(other,first.tick,first.tick,out)==RoomHistoryResult::WrongScope && out==saved,"wrong session/world/roster cannot borrow replay inputs");}
    auto bad=frame(first.tick+1);bad.players[0].x=std::numeric_limits<float>::quiet_NaN();check(!history.Capture(bad),"nonfinite command rejected before history mutation");
    for(uint64_t i=1;i<RoomHistory::Capacity+20;++i){auto f=frame(first.tick+i);if(!check(history.Capture(f) && history.Complete(f.tick,proof) && history.Verify(f.tick,proof),"bounded ring captures and verifies contiguous native frames"))return false;}
    check(history.size()==RoomHistory::Capacity && history.oldest_tick()==first.tick+20 && history.Read(scope,first.tick,first.tick,out)==RoomHistoryResult::Expired,"only acknowledged oldest input is evicted and reported expired");
    check(history.Read(scope,first.tick+20,first.tick+RoomHistory::Capacity+19,out)==RoomHistoryResult::Ready && out.size()==RoomHistory::Capacity,"entire wrapped history remains contiguous");
    const auto wrapped=out;check(history.Read(scope,5,4,out)==RoomHistoryResult::InvalidRange && history.Read(scope,0,UINT64_MAX,out)==RoomHistoryResult::InvalidRange && out==wrapped,"invalid/overflow range leaves output unchanged");
    auto invalid_scope=scope;invalid_scope.world={};check(!history.Reset(invalid_scope) && history.Read(scope,first.tick+20,first.tick+RoomHistory::Capacity+19,out)==RoomHistoryResult::Ready && out==wrapped,"invalid rearm cannot discard a valid history");
    check(history.Reset(scope),"unverified retention fixture resets explicitly");
    for(uint64_t i=0;i<RoomHistory::Capacity;++i)if(!check(history.Capture(frame(first.tick+i)),"unverified capture remains bounded"))return false;
    check(!history.Capture(frame(first.tick+RoomHistory::Capacity)) && history.size()==RoomHistory::Capacity,"capacity pressure cannot evict an unverified world");
    check(history.Complete(first.tick,proof) && history.Verify(first.tick,proof) && history.Capture(frame(first.tick+RoomHistory::Capacity)),"only acknowledgement frees oldest retention slot");
    auto terminal=scope;terminal.first_tick=UINT64_MAX;check(history.Reset(terminal),"full uint64 final tick supported without narrowing");
    auto last=frame(UINT64_MAX);check(history.Capture(last) && history.Complete(UINT64_MAX,proof) && history.Verify(UINT64_MAX,proof) && !history.Capture(frame(0)) && history.Read(terminal,UINT64_MAX,UINT64_MAX,out)==RoomHistoryResult::Ready,"final tick verifies once and cannot wrap into old input");
    REXLOG_INFO("Room input history {} {} checks: bounded 256-frame full16-player V3 history, exact consumption/completion/verification, world/session/roster fencing, immutable copies, eviction and 64-bit exhaustion; no native restore or rollback assertion",ok?"passed":"FAILED",checks);return ok;
}
}

#include "online_rollback.h"
#include <rex/sha256.h>
#include <algorithm>
#include <bit>
#include <charconv>
#include <deque>
#include <map>
#include <thread>

namespace em2::net {
namespace {
constexpr size_t HeaderBytes=4+1+16+32+8+8+2+8+8+16*12+4;
constexpr uint32_t MaximumState=16*1024*1024;
bool Nonzero(const auto& value){return std::any_of(value.begin(),value.end(),[](auto x){return x!=0;});}
bool Valid(const RollbackScope& s){return Nonzero(s.session) && Nonzero(s.schema) && s.world_epoch && s.roster_revision && s.players_mask;}
void Put(RollbackBytes& b,uint64_t n,unsigned count){for(unsigned i=count;i;--i)b.push_back(uint8_t(n>>((i-1)*8)));}
struct Reader {
    std::span<const uint8_t> bytes;size_t at=0;bool good=true;
    uint64_t Get(unsigned n){if(!good || n>bytes.size()-at){good=false;return 0;}uint64_t v=0;while(n--)v=(v<<8)|bytes[at++];return v;}
};
void WritePad(RollbackBytes& b,const Pad& p){Put(b,p.buttons,2);Put(b,p.left_trigger,1);Put(b,p.right_trigger,1);for(auto v:{p.left_x,p.left_y,p.right_x,p.right_y})Put(b,uint16_t(v),2);}
Pad ReadPad(Reader& r){Pad p;p.buttons=uint16_t(r.Get(2));p.left_trigger=uint8_t(r.Get(1));p.right_trigger=uint8_t(r.Get(1));for(auto* v:{&p.left_x,&p.left_y,&p.right_x,&p.right_y})*v=std::bit_cast<int16_t>(uint16_t(r.Get(2)));return p;}
std::optional<std::array<uint8_t,32>> Digest(std::span<const uint8_t> bytes){
    const auto hex=rex::util::sha256({reinterpret_cast<const char*>(bytes.data()),bytes.size()});
    if(hex.size()!=64)return {};std::array<uint8_t,32> out{};
    for(unsigned i=0;i<32;++i){unsigned v=0;auto result=std::from_chars(hex.data()+2*i,hex.data()+2*i+2,v,16);if(result.ec!=std::errc{})return {};out[i]=uint8_t(v);}return out;
}
bool ValidPrevious(const RollbackSnapshot& s){for(unsigned i=0;i<16;++i)if(s.previous_inputs[i].menu_pause || (!(s.scope.players_mask&(1u<<i)) && s.previous_inputs[i]!=Pad{}))return false;return true;}
}
std::optional<RollbackBytes> EncodeRollbackSnapshot(const RollbackSnapshot& s,uint32_t maximum)try{
    if(!maximum || maximum>MaximumState || !Valid(s.scope) || !s.revision || s.tick==UINT64_MAX || s.state.empty() || s.state.size()>maximum || !ValidPrevious(s))return {};
    RollbackBytes out;out.reserve(HeaderBytes+s.state.size()+32);out.insert(out.end(),{'B','W','R','S',1});
    out.insert(out.end(),s.scope.session.begin(),s.scope.session.end());out.insert(out.end(),s.scope.schema.begin(),s.scope.schema.end());
    Put(out,s.scope.world_epoch,8);Put(out,s.scope.roster_revision,8);Put(out,s.scope.players_mask,2);Put(out,s.tick,8);Put(out,s.revision,8);
    for(const auto& p:s.previous_inputs)WritePad(out,p);Put(out,s.state.size(),4);out.insert(out.end(),s.state.begin(),s.state.end());
    const auto digest=Digest(out);if(!digest)return {};out.insert(out.end(),digest->begin(),digest->end());return out;
}catch(...){return {};}
std::optional<RollbackSnapshot> DecodeRollbackSnapshot(std::span<const uint8_t> bytes,uint32_t maximum)try{
    if(!maximum || maximum>MaximumState || bytes.size()<HeaderBytes+33 || bytes.size()>HeaderBytes+size_t(maximum)+32 ||
       !std::equal(bytes.begin(),bytes.begin()+5,std::array<uint8_t,5>{'B','W','R','S',1}.begin()))return {};
    const auto digest=Digest(bytes.first(bytes.size()-32));if(!digest || !std::equal(digest->begin(),digest->end(),bytes.end()-32))return {};
    Reader r{bytes.first(bytes.size()-32),5};RollbackSnapshot s;
    for(auto& b:s.scope.session)b=uint8_t(r.Get(1));for(auto& b:s.scope.schema)b=uint8_t(r.Get(1));
    s.scope.world_epoch=r.Get(8);s.scope.roster_revision=r.Get(8);s.scope.players_mask=uint16_t(r.Get(2));s.tick=r.Get(8);s.revision=r.Get(8);
    for(auto& p:s.previous_inputs)p=ReadPad(r);const auto length=r.Get(4);
    if(!r.good || !length || length>maximum || length!=r.bytes.size()-r.at || !Valid(s.scope) || !s.revision || s.tick==UINT64_MAX || !ValidPrevious(s))return {};
    s.state.assign(r.bytes.begin()+r.at,r.bytes.end());return s;
}catch(...){return {};}

struct RollbackSession::Impl {
    struct Actual {uint16_t mask=0;std::array<Pad,16> pads{};};
    struct State {uint64_t tick=0;std::shared_ptr<const RollbackBytes> bytes;std::array<Pad,16> previous{};};
    RollbackWorld& world;RollbackRole role;RollbackLimits limits;RollbackScope scope{};
    std::thread::id owner;bool active=false,faulted=false,resync=false;uint64_t head=0,revision=0,accepted_tick=0,accepted_revision=0;
    std::deque<State> history;std::map<uint64_t,Actual> actual;RollbackStats metrics{};uint16_t suspended=0;
    Impl(RollbackWorld& w,RollbackRole r,RollbackLimits l):world(w),role(r),limits(l){}
    bool Owner()const{return owner==std::this_thread::get_id();}
    bool Limits()const{return limits.history_frames>=2 && limits.history_frames<=240 && limits.max_prediction && limits.max_prediction<=limits.history_frames && limits.future_inputs && limits.future_inputs<=240 &&
        limits.snapshot_bytes && limits.snapshot_bytes<=MaximumState && uint64_t(limits.history_bytes)>=uint64_t(limits.snapshot_bytes)*2 && limits.history_bytes<=256*1024*1024;}
    bool Capable()const{return (world.Capabilities()&RequiredRollbackCapabilities)==RequiredRollbackCapabilities;}
    bool Capture(RollbackBytes& out){try{out.clear();return world.Capture(out) && !out.empty() && out.size()<=limits.snapshot_bytes;}catch(...){return false;}}
    bool Restore(std::span<const uint8_t> bytes){try{return world.Restore(bytes);}catch(...){return false;}}
    bool Step(const RollbackInput& frame,RollbackStep mode){try{return world.Step(frame,mode);}catch(...){return false;}}
    RollbackResult Reject(){++metrics.rejected;return RollbackResult::Rejected;}
    size_t Bytes(const std::deque<State>& states)const{size_t n=0;for(const auto& s:states)n+=s.bytes->size();return n;}
    void Trim(std::deque<State>& states){while(states.size()>1 && (states.size()>size_t(limits.history_frames)+1 || Bytes(states)>limits.history_bytes))states.pop_front();}
    void Prune(){while(!actual.empty() && actual.begin()->first<history.front().tick)actual.erase(actual.begin());}
    RollbackInput Frame(uint64_t tick,const std::array<Pad,16>& previous,const std::map<uint64_t,Actual>& inputs)const{
        RollbackInput f;f.tick=tick;f.players_mask=scope.players_mask;f.players=previous;
        const auto it=inputs.find(tick);for(unsigned i=0;i<16;++i){const auto bit=uint16_t(1u<<i);
            if(!(scope.players_mask&bit) || (role==RollbackRole::Host && tick>=head && (suspended&bit))){f.players[i]={};continue;}
            if(it!=inputs.end() && (it->second.mask&bit))f.players[i]=it->second.pads[i];else f.predicted_mask|=bit;
        }return f;
    }
    RollbackResult Rollback(State start,std::map<uint64_t,Actual> inputs,bool authoritative){
        RollbackBytes backup;if(!Capture(backup)){faulted=true;return RollbackResult::Faulted;}
        auto fail=[&]{if(!Restore(backup))faulted=true;else resync=true;return faulted?RollbackResult::Faulted:RollbackResult::NeedFullState;};
        try{
        auto states=history;while(!states.empty() && states.back().tick>=start.tick)states.pop_back();states.push_back(std::move(start));
        uint64_t replayed=0;
        if(!Restore(*states.back().bytes))return fail();
        while(states.back().tick<head){
            const auto frame=Frame(states.back().tick,states.back().previous,inputs);RollbackBytes state;
            if(!Step(frame,RollbackStep::Replay) || !Capture(state))return fail();
            states.push_back({frame.tick+1,std::make_shared<const RollbackBytes>(std::move(state)),frame.players});++replayed;Trim(states);
        }
        history=std::move(states);actual=std::move(inputs);head=history.back().tick;Prune();resync=false;
        ++metrics.restores;metrics.replayed_frames+=replayed;if(authoritative)++metrics.corrections;else ++revision;
        return RollbackResult::Corrected;
        }catch(...){return fail();}
    }
    RollbackResult Initialize(const RollbackScope& s,uint64_t tick,RollbackBytes bytes,std::array<Pad,16> previous={},uint64_t host_revision=0){
        std::deque<State> prepared;prepared.push_back({tick,std::make_shared<const RollbackBytes>(std::move(bytes)),previous});
        scope=s;head=tick;owner=std::this_thread::get_id();active=true;faulted=resync=false;suspended=0;revision=host_revision?host_revision:1;accepted_tick=tick;accepted_revision=host_revision;actual.clear();history=std::move(prepared);metrics={};return RollbackResult::Ready;
    }
};
RollbackSession::RollbackSession(RollbackWorld& w,RollbackRole r,RollbackLimits l):impl_(std::make_unique<Impl>(w,r,l)){}
RollbackSession::~RollbackSession()=default;
RollbackResult RollbackSession::Begin(const RollbackScope& scope,uint64_t first)try{
    auto& s=*impl_;if(s.active && !s.Owner())return RollbackResult::WrongThread;
    if(!s.Limits() || !s.Capable() || !Valid(scope) || first==UINT64_MAX || (s.active && s.scope==scope))return s.Reject();
    RollbackBytes bytes;if(!s.Capture(bytes))return s.Reject();return s.Initialize(scope,first,std::move(bytes));
}catch(...){impl_->faulted=true;return RollbackResult::Faulted;}
RollbackResult RollbackSession::BeginFromAuthoritative(const RollbackScope& expected,std::span<const uint8_t> packet)try{
    auto& s=*impl_;if(s.active && !s.Owner())return RollbackResult::WrongThread;
    if(s.role!=RollbackRole::Client || !s.Limits() || !s.Capable() || !Valid(expected))return s.Reject();
    auto snapshot=DecodeRollbackSnapshot(packet,s.limits.snapshot_bytes);if(!snapshot || snapshot->scope!=expected || (s.active && s.scope==expected && snapshot->revision<s.accepted_revision))return s.Reject();
    RollbackBytes backup;if(!s.Capture(backup))return s.Reject();
    if(!s.Restore(snapshot->state)){if(!s.Restore(backup)){s.faulted=true;return RollbackResult::Faulted;}return s.Reject();}
    try{return s.Initialize(expected,snapshot->tick,std::move(snapshot->state),snapshot->previous_inputs,snapshot->revision);}
    catch(...){s.Restore(backup);throw;}
}catch(...){impl_->faulted=true;return RollbackResult::Faulted;}
RollbackResult RollbackSession::SubmitInput(unsigned peer,uint64_t tick,const Pad& pad){
    const RollbackCommand command{peer,tick,pad};return SubmitInputs({&command,1});
}
RollbackResult RollbackSession::SubmitInputs(std::span<const RollbackCommand> commands)try{
    auto& s=*impl_;if(s.active && !s.Owner())return RollbackResult::WrongThread;
    if(s.faulted)return RollbackResult::Faulted;
    if(!s.active || commands.empty() || commands.size()>256)return s.Reject();
    auto inputs=s.actual;std::optional<uint64_t> earliest;bool added=false;
    for(const auto& command:commands){const auto peer=command.peer;const auto tick=command.tick;const auto& pad=command.pad;
        if(!peer || peer>16 || !(s.scope.players_mask&(1u<<(peer-1))) || (s.suspended&(1u<<(peer-1))) || pad.menu_pause || tick==UINT64_MAX || (tick>s.head && tick-s.head>s.limits.future_inputs))return s.Reject();
        if(tick<s.history.front().tick)return RollbackResult::TooOld;
        const auto bit=uint16_t(1u<<(peer-1));auto& record=inputs[tick];
        if(record.mask&bit){if(record.pads[peer-1]!=pad)return s.Reject();continue;}
        record.mask|=bit;record.pads[peer-1]=pad;added=true;
        if(tick<s.head){
            const auto before=std::find_if(s.history.begin(),s.history.end(),[&](const auto& state){return state.tick==tick;});
            if(before==s.history.end())return RollbackResult::TooOld;
            if(s.Frame(tick,before->previous,s.actual).players[peer-1]!=pad)earliest=earliest?std::min(*earliest,tick):tick;
        }
    }
    if(!added)return RollbackResult::Duplicate;
    if(!earliest){s.actual=std::move(inputs);return RollbackResult::Accepted;}
    if(s.revision==UINT64_MAX)return s.Reject();
    const auto before=std::find_if(s.history.begin(),s.history.end(),[&](const auto& state){return state.tick==*earliest;});
    return s.Rollback(*before,std::move(inputs),false);
}catch(...){impl_->faulted=true;return RollbackResult::Faulted;}
RollbackResult RollbackSession::SetPeerConnected(unsigned peer,bool connected){
    auto& s=*impl_;if(s.active && !s.Owner())return RollbackResult::WrongThread;
    if(s.faulted)return RollbackResult::Faulted;
    if(!s.active || s.role!=RollbackRole::Host || !peer || peer>16 || !(s.scope.players_mask&(1u<<(peer-1))))return s.Reject();
    const auto bit=uint16_t(1u<<(peer-1));
    if(connected){s.suspended&=uint16_t(~bit);return RollbackResult::Accepted;}
    if(s.suspended&bit)return RollbackResult::Duplicate;
    for(auto it=s.actual.lower_bound(s.head);it!=s.actual.end();){
        it->second.mask&=uint16_t(~bit);it->second.pads[peer-1]={};
        if(!it->second.mask)it=s.actual.erase(it);else ++it;
    }
    // Materialize neutral input for every future live frame so a later replay
    // cannot resurrect a predicted held movement/action from before disconnect.
    s.suspended|=bit;return RollbackResult::Accepted;
}
RollbackResult RollbackSession::Advance()try{
    auto& s=*impl_;if(s.active && !s.Owner())return RollbackResult::WrongThread;
    if(s.faulted)return RollbackResult::Faulted;if(!s.active)return s.Reject();if(s.resync)return RollbackResult::NeedFullState;
    if(s.head>=UINT64_MAX-1 || s.revision==UINT64_MAX)return s.Reject();
    if(s.role==RollbackRole::Client && s.head>=s.accepted_tick && s.head-s.accepted_tick>=s.limits.max_prediction)return RollbackResult::Waiting;
    if(s.role==RollbackRole::Host && s.suspended){auto& neutral=s.actual[s.head];for(unsigned i=0;i<16;++i)if(s.suspended&(1u<<i)){neutral.mask|=uint16_t(1u<<i);neutral.pads[i]={};}}
    const auto frame=s.Frame(s.head,s.history.back().previous,s.actual);RollbackBytes bytes;
    if(!s.Step(frame,RollbackStep::Live) || !s.Capture(bytes)){
        s.faulted=true;s.Restore(*s.history.back().bytes);return RollbackResult::Faulted;
    }
    s.history.push_back({++s.head,std::make_shared<const RollbackBytes>(std::move(bytes)),frame.players});s.Trim(s.history);s.Prune();++s.revision;return RollbackResult::Advanced;
}catch(...){auto& s=*impl_;if(s.active && s.Owner() && !s.history.empty()){s.Restore(*s.history.back().bytes);s.head=s.history.back().tick;}s.faulted=true;return RollbackResult::Faulted;}
RollbackResult RollbackSession::AcceptAuthoritative(std::span<const uint8_t> packet)try{
    auto& s=*impl_;if(s.active && !s.Owner())return RollbackResult::WrongThread;
    if(s.faulted)return RollbackResult::Faulted;if(!s.active || s.role!=RollbackRole::Client)return s.Reject();
    auto snapshot=DecodeRollbackSnapshot(packet,s.limits.snapshot_bytes);if(!snapshot || snapshot->scope!=s.scope)return s.Reject();
    if(snapshot->revision<=s.accepted_revision)return RollbackResult::Duplicate;
    const auto tick=snapshot->tick;
    if(tick<s.history.front().tick){s.resync=true;return RollbackResult::NeedFullState;}
    if(tick>=s.head){
        std::deque<Impl::State> prepared;prepared.push_back({tick,std::make_shared<const RollbackBytes>(std::move(snapshot->state)),snapshot->previous_inputs});
        RollbackBytes backup;if(!s.Capture(backup))return s.Reject();
        if(!s.Restore(*prepared.back().bytes)){if(!s.Restore(backup)){s.faulted=true;return RollbackResult::Faulted;}return s.Reject();}
        s.history=std::move(prepared);s.head=tick;s.Prune();s.resync=false;
        ++s.metrics.restores;++s.metrics.corrections;
    }else{
        const auto result=s.Rollback({tick,std::make_shared<const RollbackBytes>(std::move(snapshot->state)),snapshot->previous_inputs},s.actual,true);
        if(result!=RollbackResult::Corrected)return result;
    }
    s.accepted_revision=snapshot->revision;s.accepted_tick=tick;return RollbackResult::Corrected;
}catch(...){impl_->faulted=true;return RollbackResult::Faulted;}
std::optional<RollbackBytes> RollbackSession::ExportAuthoritative()const try{
    const auto& s=*impl_;if(!s.active || !s.Owner() || s.faulted || s.role!=RollbackRole::Host)return {};
    return EncodeRollbackSnapshot({s.scope,s.head,s.revision,s.history.back().previous,*s.history.back().bytes},s.limits.snapshot_bytes);
}catch(...){return {};}
uint64_t RollbackSession::next_tick()const{return impl_->head;}
uint64_t RollbackSession::authority_revision()const{return impl_->accepted_revision;}
size_t RollbackSession::retained_bytes()const{return impl_->Bytes(impl_->history);}
RollbackStats RollbackSession::stats()const{return impl_->metrics;}
RollbackScope RollbackSession::scope()const{return impl_->scope;}
}

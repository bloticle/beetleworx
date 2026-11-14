#pragma once
#include "online.h"
#include <chrono>
#include <optional>

namespace em2::net {
// Owner-thread state only. No native callbacks or pointers. The first vote
// starts one absolute budget, retained through nested loads until the loaded
// tick is verified. Heartbeats, duplicate votes and nested Begin/End do not
// buy more time.
class RoomLoadExchange {
public:
    using Clock=std::chrono::steady_clock;
    static bool Valid(const LoadPoint& p){
        if((p.phase!=LoadPhase::Begin && p.phase!=LoadPhase::End) ||
           (p.phase==LoadPhase::Begin && p.success))return false;
        bool ended=false;size_t length=0;
        for(unsigned char c:p.destination){if(!c){ended=true;continue;}
            if(ended || c<32 || c>=127)return false;++length;}
        return ended && length;
    }
    bool Add(uint64_t peer,uint16_t roster,const LoadPoint& p,Clock::time_point now){
        if(!peer || peer>16 || !(roster&(1u<<(peer-1))) || !Valid(p) ||
           exhausted_ || failed_ || Expired(now) || p.sequence!=next_ || (root_tick_ && p.tick!=*root_tick_) ||
           (last_ && p.tick<last_->tick))return false;
        if(p.phase==LoadPhase::Begin){if(depth_==stack_.size())return false;}
        else if(!depth_ || stack_[depth_-1].tick!=p.tick || stack_[depth_-1].destination!=p.destination)return false;
        const auto bit=uint16_t(1u<<(peer-1));
        if((votes_&bit) || (pending_ && *pending_!=p) || (mask_ && mask_!=roster))return false;
        if(!started_){started_=now;root_tick_=p.tick;}
        mask_=roster;pending_=p;votes_|=bit;return true;
    }
    bool Complete()const{return pending_ && votes_==mask_;}
    uint16_t voters()const{return votes_;}
    bool Commit(const LoadPoint& p,bool authority){
        if(!pending_ || *pending_!=p || (authority && !Complete()))return false;
        if(p.phase==LoadPhase::Begin)stack_[depth_++]=p;else {--depth_;failed_|=!p.success;}
        last_=p;pending_.reset();votes_=0;
        if(next_==UINT64_MAX)exhausted_=true;else ++next_;
        return true;
    }
    bool Agreed(const LoadPoint& p)const{return last_ && *last_==p;}
    bool Loaded(const LoadPoint& p)const{return Agreed(p) && p.phase==LoadPhase::End && p.success && !Blocks(p.tick);}
    bool Blocks(uint64_t tick)const{return root_tick_ && tick>=*root_tick_ && (failed_ || depth_ || pending_);}
    bool Tracking()const{return started_.has_value();}
    bool Expired(Clock::time_point now)const{return started_ && now-*started_>std::chrono::seconds(120);}
    void Verified(uint64_t tick){if(root_tick_ && tick>=*root_tick_ && !failed_ && !depth_ && !pending_){started_.reset();root_tick_.reset();}}
private:
    uint64_t next_=0;bool exhausted_=false,failed_=false;size_t depth_=0;
    uint16_t mask_=0,votes_=0;
    std::array<LoadPoint,8> stack_{};
    std::optional<LoadPoint> pending_,last_;
    std::optional<uint64_t> root_tick_;
    std::optional<Clock::time_point> started_;
};
}

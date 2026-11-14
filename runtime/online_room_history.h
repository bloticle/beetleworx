#pragma once
#include "online_movement_frame.h"
#include <optional>
#include <vector>
namespace em2::net {
using RoomHistoryHash=std::array<uint8_t,32>;
struct RoomHistoryScope {
    Identity identity;RoomHistoryHash world{};uint64_t roster_revision=0,first_tick=0;uint16_t players_mask=0;
    bool operator==(const RoomHistoryScope&)const=default;
};
struct RoomHistoryEntry {
    RoomFrame frame;std::optional<RoomHistoryHash> completed;bool verified=false;
    bool operator==(const RoomHistoryEntry&)const=default;
};
enum class RoomHistoryResult {Ready,NotStarted,WrongScope,InvalidRange,Expired,Unavailable,Unverified};
// Owner-thread bounded input history, not a native world snapshot. No replay,
// native callback or simulation writes occur here. Only verified ranges can be
// read; missing/evicted input never becomes an invented neutral command.
class RoomHistory {
public:
    static constexpr size_t Capacity=256;
    bool Reset(const RoomHistoryScope&);
    bool Capture(const RoomFrame& consumed);
    bool Complete(uint64_t,const RoomHistoryHash&);
    bool Verify(uint64_t,const RoomHistoryHash&);
    RoomHistoryResult Read(const RoomHistoryScope&,uint64_t first,uint64_t last,std::vector<RoomHistoryEntry>&)const;
    const std::optional<RoomHistoryScope>& scope()const{return scope_;}
    std::optional<uint64_t> oldest_tick()const;
    size_t size()const{return count_;}
private:
    std::optional<RoomHistoryScope> scope_;
    std::vector<RoomHistoryEntry> slots_;
    size_t head_=0,count_=0;
    uint64_t capture_=0,complete_=0,verify_=0;
    bool capture_exhausted_=false,complete_exhausted_=false,verify_exhausted_=false;
    RoomHistoryEntry* Find(uint64_t);
    const RoomHistoryEntry* Find(uint64_t)const;
};
bool TestOnlineRoomHistory();
}

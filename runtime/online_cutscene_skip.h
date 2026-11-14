#pragma once
#include "online.h"
#include <memory>
struct PPCContext;
namespace em2::net {
// Original native skip-action math only. Results grant no callback, wire or
// gameplay ownership. Device/action histories are private copies.
struct NativeCutsceneSkipSample {
    uint64_t epoch=0,tick=0;
    std::array<bool,3> dispatch{}; // display prompt, begin held skip, release
    bool operator==(const NativeCutsceneSkipSample&)const=default;
};
class NativeCutsceneSkipSource {
public:
    NativeCutsceneSkipSource();~NativeCutsceneSkipSource();
    NativeCutsceneSkipSource(const NativeCutsceneSkipSource&)=delete;
    NativeCutsceneSkipSource& operator=(const NativeCutsceneSkipSource&)=delete;
    bool Arm(uint32_t registry,unsigned native_user,uint64_t epoch,uint64_t first_tick,PPCContext&,uint8_t*);
    bool Produce(const Pad&,unsigned native_user,uint64_t epoch,uint64_t tick,PPCContext&,uint8_t*,NativeCutsceneSkipSample&);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
struct RoomFrame;
// Receives agreed UI intent while character controls stay quarantined.
class NativeCutsceneSkipReceiver {
public:
    NativeCutsceneSkipReceiver();~NativeCutsceneSkipReceiver();
    NativeCutsceneSkipReceiver(const NativeCutsceneSkipReceiver&)=delete;
    NativeCutsceneSkipReceiver& operator=(const NativeCutsceneSkipReceiver&)=delete;
    bool Consume(const RoomFrame&,uint32_t registry,uint64_t input_epoch,PPCContext&,uint8_t*);
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
bool NativeCutsceneSkipTargetAvailable(uint32_t registry,PPCContext&,uint8_t*);
bool NativeCutsceneSkipTimerOwned(uint32_t timer,uint32_t caller,PPCContext&,uint8_t*);
bool TestNativeCutsceneSkipMath();
}

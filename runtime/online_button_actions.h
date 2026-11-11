#pragma once
#include "online_native_triggers.h"
#include "online_movement_frame.h"
#include <optional>
namespace em2::net {
// Canonical supported definition subset, excluding addresses and mutable history.
std::optional<std::array<uint8_t,32>> HashNativeButtonSchema(const NativeTriggerBinding&);
bool EncodeNativeButtonActions(const NativeTriggerBinding&,const NativeTriggerBatch&,RoomButtonActions&);
// Metadata matching ONLY. Caller must separately revalidate live receiver
// ownership before using the returned local descriptors. No callbacks here.
bool MatchNativeButtonActions(const NativeTriggerBinding&,const RoomButtonActions&,std::vector<NativeActorAction>&);
bool TestNativeButtonActions();
}

#pragma once
#include <cstdint>
struct PPCContext;
namespace em2::net {
bool ArmOwnedActorProbe(uint64_t peer,uint32_t primary_entity,PPCContext&,uint8_t* base);
bool BeforeOwnedActorProbeTick(PPCContext&,uint8_t* base);
void AfterOwnedActorProbeTick(PPCContext&,uint8_t* base);
bool OwnedActorProbeFailed(uint64_t peer);
bool StopOwnedActorProbe(uint64_t peer);
bool RetireOwnedActorProbe(uint64_t peer,PPCContext&,uint8_t* base);
void ObserveOwnedActorProbeJumpTargets(uint64_t peer,uint32_t task,uint32_t motion,uint32_t physics,uint8_t* base);
void ObserveOwnedActorProbeJumpStimulus(PPCContext&,uint8_t* base);
}

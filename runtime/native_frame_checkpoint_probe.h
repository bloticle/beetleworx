#pragma once
#include <cstdint>
#include <string>
struct PPCContext;
namespace em2 {
bool PreparePrivateNativeFrameScriptRoots(PPCContext&,uint8_t*,uint32_t state,uint32_t values,unsigned count,std::string&);
void ObservePrivateNativeReplayBodyCall(PPCContext&,uint8_t*,uint32_t method=0,uint32_t body=0,uint32_t vector_a=0,uint32_t vector_b=0);
void ObservePrivateNativeReplayController(PPCContext&,uint8_t*);
void ObservePrivateNativeReplayPose(PPCContext&,uint8_t*,unsigned phase,uint32_t object=0);
void ObservePrivateNativeReplayTranslation(PPCContext&,uint8_t*,unsigned offset=72);
void ObservePrivateNativeReplayPhysics(uint8_t*,uint32_t object,unsigned phase);
void TickPrivateNativeFrameCheckpoint(PPCContext&,uint8_t*,uint64_t completed_tick);
void CancelPrivateNativeFrameCheckpoint(PPCContext&,uint8_t*);
}

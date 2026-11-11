#pragma once
#include "em1_native_resources.h"
#include "em1_preview_scene.h"
namespace em2::em1 {
enum class NativePrototype { Trigger, Gear, Particle };
struct NativeArchitectureEntity {
  uint32_t scratch=0,scratch_size=0,entity=0,scenegraph=0,core=0,handle=0;
  bool parser_constructed=false,entity_reference=false,admitted=false,destroy_requested=false;
};
// Call only on the normal game thread after the existing level has finished
// loading. Retain model until Destroy reports complete. No camera/player input.
bool CreateNativeArchitectureEntity(PPCContext&,uint8_t*,const ArchitectureScene&,
    const NativeModelResource&,NativeArchitectureEntity&,std::string& error, bool instantiate_only=false,
    NativePrototype prototype=NativePrototype::Trigger);
// Uses the stock deferred entity destruction queue. Poll on later game ticks;
// it does not flush unrelated deletion work or release live component resources.
bool DestroyNativeArchitectureEntity(PPCContext&,uint8_t*,NativeArchitectureEntity&,
    bool& complete,std::string& error);
bool ActivateNativeTrigger(PPCContext&,uint8_t*,NativeArchitectureEntity&,std::string& error);
void TraceNativeParticleUpdate(PPCContext&,uint8_t*,unsigned stage);
void ResetNativeParticleTrace();
bool ObserveNativeParticleGraph(PPCContext&,uint8_t*,const NativeArchitectureEntity&,bool running);
bool SetNativeParticleEmission(PPCContext&,uint8_t*,const NativeArchitectureEntity&,bool,std::string& error);
bool ActivateNativeParticle(PPCContext&,uint8_t*,NativeArchitectureEntity&,std::string& error);
bool ActivateNativeGear(PPCContext&,uint8_t*,NativeArchitectureEntity&,std::string& error);
bool ObserveNativeGear(uint8_t*,const NativeArchitectureEntity&,uint32_t* body_address=nullptr);
bool SetNativeGearRotationSpeed(PPCContext&,uint8_t*,const NativeArchitectureEntity&,float,std::string& error);
bool ReadNativeGearRotations(uint8_t*,const NativeArchitectureEntity&,std::array<float,18>&);
bool ObserveNativeTrigger(uint8_t*,const NativeArchitectureEntity&);
}

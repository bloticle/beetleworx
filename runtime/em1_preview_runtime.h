#pragma once
struct PPCContext;
#include <cstdint>
#include <string_view>
#include <array>
#include <memory>
namespace em2::em1 {
struct SingleGsaEntry;
struct NativeScriptState;
bool IsActiveArchitectureScriptState(const NativeScriptState&);
struct PreviewLightingSample {
  uint32_t entity=0;
  std::array<float,3> position{};
  std::array<std::array<float,3>,9> coefficients{};
};
std::shared_ptr<const PreviewLightingSample> CurrentPreviewLighting();
SingleGsaEntry& ActiveSingleGsaEntry();
bool BeforeArchitectureLevelChange(PPCContext&,uint8_t*,std::string_view);
void AfterArchitectureLevelChange(PPCContext&,uint8_t*,std::string_view);
bool TryEnterArchitectureLevel(PPCContext&,uint8_t*,uint32_t,std::string_view);
void TickArchitecturePreview(PPCContext&,uint8_t*);
void TickArchitectureScripts(PPCContext&,uint8_t*,double simulation_delta);
bool SetOwnedArchitectureParticleEmission(PPCContext&,uint8_t*,uint32_t native_handle,bool enabled);
bool SetOwnedArchitectureGearSpeed(PPCContext&,uint8_t*,uint32_t native_handle,float speed);
// Zero means unavailable/unowned; mappings are revoked before native unload.
uint32_t NativeHandleForAuthoredEntity(uint32_t authored_id);
uint32_t AuthoredEntityForNativeHandle(uint32_t native_handle);
void NotifyArchitecturePreviewLevelLoaded(std::string_view);
}

#pragma once
#include <cstdint>
#include <span>
#include <string>
struct PPCContext;
namespace em2::net {
struct NativeClothingTexture {uint32_t root=0,renderer_data=0,width=0,height=0;};
// Initialized native render owner only. Retains one original NiPointer
// reference after destroying the temporary NiStream; caller must release it.
bool LoadNativeClothingTexture(PPCContext&,uint8_t*,std::span<const uint8_t>,NativeClothingTexture&,std::string&);
bool ReleaseNativeClothingTexture(PPCContext&,uint8_t*,NativeClothingTexture&,std::string&);
bool AssignNativeClothingPointer(PPCContext&,uint8_t*,uint32_t slot,uint32_t texture);
bool RetainNativeClothingReference(PPCContext&,uint8_t*,uint32_t texture,uint32_t& owned);
bool DropNativeClothingReference(PPCContext&,uint8_t*,uint32_t& owned);
// Private detached load/upload/retain/release audit; never binds a player.
void ProbeNativeClothingTextures(PPCContext&,uint8_t*);
}

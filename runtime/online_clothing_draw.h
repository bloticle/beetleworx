#pragma once
#include <cstdint>
#include <array>
struct PPCContext;
namespace em2::net {
struct NativeClothingBinding {
    uint32_t state=0,property=0,shader_array=0;
    std::array<uint32_t,3> maps{},textures{};
};
// Audited NiMesh property state -> NiTexturingProperty -> diffuse/normal/specular
// shader-map slots. Read-only, native render owner only. Failure leaves output.
bool ReadNativeClothingBinding(uint32_t mesh,uint8_t*,NativeClothingBinding&,unsigned* stage=nullptr);
// Render owner only. Retires cached native references before registering the
// new frame when the session ends, binding is disabled, or its source changes.
void BeginOnlineClothingDrawFrame(PPCContext&,uint8_t*,bool session_available);
void RegisterOnlineClothingRoot(uint64_t peer,uint32_t entity,uint32_t root,uint32_t rgba,unsigned character=0);
void RenderOnlineClothing(PPCContext&,uint8_t*,void(*draw)(PPCContext&,uint8_t*));
bool ReleaseOnlineClothingDrawResources(PPCContext&,uint8_t*);
// Bounded read-only native draw diagnostic. No bindings are changed.
void TraceOnlineClothingDraw(PPCContext&,uint32_t mesh,uint8_t* base);
bool TestOnlineClothingDrawOwnership();
}

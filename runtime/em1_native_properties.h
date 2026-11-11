#pragma once
#include "ppc_context.h"
#include <span>
#include <string>
#include <vector>

namespace em2::em1 {
// The first post-retail dispatch slot is already owned by character switching.
// These identifiers extend the dispatch table without replacing retail code.
inline constexpr uint32_t kFloatDefaultBox=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+4);
inline constexpr uint32_t kFloatDefaultBuffer=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+8);
inline constexpr uint32_t kFloatBoxDescriptor=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+36);
inline constexpr uint32_t kAmmoNativeRead=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+40);
inline constexpr uint32_t kAmmoNativeWrite=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+44);
inline constexpr uint32_t kAmmoNativeCreate=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+48);
inline constexpr uint32_t kArrayDefaultBox=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+52);
inline constexpr uint32_t kInventoryNativeCreate=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+56);
inline constexpr uint32_t kStringPropertyWrite=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+60);
inline constexpr uint32_t kNavigationNativeCreate=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+64);
inline constexpr uint32_t kNavigationNativeDefinition=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+68);
inline constexpr uint32_t kNavigationNativeDestroy=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+72);
inline constexpr uint32_t kBranchNativeCreate=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+76);
inline constexpr uint32_t kRelayNativeCreate=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+80);
inline constexpr uint32_t kSeedNativeCreate=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+84);
inline constexpr uint32_t kTrackingNativeCreate=uint32_t(PPC_CODE_BASE+PPC_CODE_SIZE+88);
inline constexpr uint32_t kPropertyCodeSize=uint32_t(PPC_CODE_SIZE+92);
void CreateNativeFloatDefault(PPCContext&,uint8_t*);
void ReadNativeFloatDefault(PPCContext&,uint8_t*);
void NativeFloatBoxDescriptor(PPCContext&,uint8_t*);
void CreateNativeArrayDefault(PPCContext&,uint8_t*);
void WriteNativeStringProperty(PPCContext&,uint8_t*);
struct FloatPropertyDeclaration {std::string name;float default_value=0;};
enum class NativePropertyShape {Float,FloatArray,IntegerArray,StringArray,Integer,String,Point3,Boolean};
struct NativePropertyDeclaration {std::string name;NativePropertyShape shape=NativePropertyShape::Float;float default_value=0;};
struct NativeFloatSchema {
  NativeFloatSchema()=default;
  NativeFloatSchema(const NativeFloatSchema&)=delete;
  NativeFloatSchema& operator=(const NativeFloatSchema&)=delete;
  NativeFloatSchema(NativeFloatSchema&&)=delete;
  NativeFloatSchema& operator=(NativeFloatSchema&&)=delete;
  std::vector<uint32_t> descriptors;
  uint32_t storage=0,storage_size=0;
};
// Standalone native descriptors only, not component registration or activation.
// Explicit release requires a valid initialized engine PPC context.
bool BuildNativeFloatSchema(PPCContext&,uint8_t*,std::span<const FloatPropertyDeclaration>,NativeFloatSchema&,std::string&);
// Native scalar/array descriptors, including real native vector storage and
// element setters. Array defaults are empty; authored blueprint values remain
// owned by the retail property loader. Integer/String/Point3 currently use
// retail zero/empty defaults; only Float supports a supplied nonzero default.
bool BuildNativePropertySchema(PPCContext&,uint8_t*,std::span<const NativePropertyDeclaration>,NativeFloatSchema&,std::string&);
bool ProbeNativeArraySchema(PPCContext&,uint8_t*,std::string&);
// Native scalar types needed by AIPathDatabaseComponent. This does not register
// that component or establish its lifecycle, authored defaults or world owner.
bool ProbeNativeNavigationSchema(PPCContext&,uint8_t*,std::string&);
bool ProbeNativeBooleanSchema(PPCContext&,uint8_t*,std::string&);
std::span<const NativePropertyDeclaration> NativeNavigationDeclarations();
bool BuildNativeAmmoSchema(PPCContext&,uint8_t*,NativeFloatSchema&,std::string&);
std::span<const NativePropertyDeclaration> NativeInventoryDeclarations();
bool BuildNativeInventorySchema(PPCContext&,uint8_t*,NativeFloatSchema&,std::string&);
void ReleaseNativeFloatSchema(PPCContext&,uint8_t*,NativeFloatSchema&);
bool ProbeNativeFloatSchema(PPCContext&,uint8_t*,std::string&);
}

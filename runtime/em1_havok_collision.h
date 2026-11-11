#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace em2::em1 {
struct AuthoredCollisionPart {
 std::vector<std::array<float,3>> vertices;
 std::vector<std::array<uint32_t,3>> triangles;
 std::vector<uint32_t> triangle_filters;
 uint32_t welding_offset=0,user_data=0,source_index_stride=0,source_index_kind=0;
};
struct AuthoredCollisionBody {
 std::string name;
 uint32_t source_offset=0,filter=0,default_filter=0,user_data=0,shape_user_data=0,key_bits=0;
 uint8_t motion_type=0,welding_type=0,quality_type=0;bool disable_welding=false;
 float radius=0,friction=0,restitution=0;
 std::array<float,16> transform{};
 std::array<uint8_t,12> source_material{};
 std::vector<std::array<uint32_t,4>> properties;
 std::vector<uint16_t> welding;
 std::vector<AuthoredCollisionPart> parts;
 std::array<float,4> original_mopp_info{};
 std::vector<uint8_t> original_mopp_code;
 uint32_t original_mopp_user_data=0;
 uint8_t original_mopp_type=0,original_mopp_build_type=0;
 bool accelerated=false;
 // Current native2011 schema and authored geometry; optional original MOPP.
 // Load through native hkSerializeUtil and retain its hkResource until every
 // body using its array storage has been removed and released.
 std::vector<uint8_t> native_shape_pack;
};
struct AuthoredStaticCollision {
 std::vector<AuthoredCollisionBody> bodies;
 uint64_t vertices=0,triangles=0;
 bool accelerated=false,native_load_proven=false,world_attached=false;
};
// hkRotation columns are padded SIMD vectors, not an affine matrix bottom
// row. Padding may be nonzero in original packs; xyz must remain rigid.
bool IsRigidCollisionPose(const std::array<float,16>&,bool havok_padding=false);
// Original EM1 Havok7 BE32 input only. Unsupported dynamic/compound/transformed
// subparts fail explicitly. Entity placement is not baked: compose the authored
// body transform with stock component placement exactly once during admission.
bool ReconstructAuthoredStaticCollision(std::span<const uint8_t> source,
 AuthoredStaticCollision& result,std::string& error);
bool ScaleAuthoredStaticCollision(AuthoredStaticCollision&,float scale,std::string& error);
// Retains the original MOPP bytecode and quantization over identical triangle
// ordering/keyspace. Native VM parity must be checked before enabling use.
bool AccelerateAuthoredStaticCollision(AuthoredStaticCollision&,std::string& error);
// Rebuild only this body's identical authored triangle collection when its
// acceleration proof fails; other bodies retain their independently proven trees.
bool BuildUnacceleratedCollisionPack(const AuthoredCollisionBody&,
 std::vector<uint8_t>& output,std::string& error);
// CPU schema/fixup checks only; never claims native load/raycast/playability.
bool TestEm1HavokCollision(std::string& error);
// Proven entrypoints from the actual EM2 image; not called by this pure module.
inline constexpr uint32_t kNativeRigidBodyCinfoDefault=0x8249B068;
inline constexpr uint32_t kNativeRigidBodyConstructor=0x82474700;
inline constexpr uint32_t kNativeStorageMeshFinish=0x82441D58;
inline constexpr uint32_t kNativeStorageMeshClass=0x833C85D0;
}

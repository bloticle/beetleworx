#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>
#include <stdexcept>

namespace em2::em1 {
// Exact native Simple arithmetic and sampler0. Unlike the donor, fetch and
// export W rather than constant1. The paired FXLO preserves authored vertex W.
// MinimumColourLevel c0 only affects RGB; no clamp or extra texture is added.
inline std::vector<uint32_t> BuildSimpleAlphaPixelProgram() {
  return {0x00011002,0x00001200,0xC4000000,
          0x00002003,0x00002200,0x00000000,
          0x10080001,0x1F1FF688,0x00004000,
          0xC8070001,0x00C0C000,0xA2010000,
          0xC80FC000,0x00000000,0xE1010000,
          0x4E4A0000,0x1A863251,0xDE60F04D};
}
// ONE source map through sampler1 (the donor FXLO's source map1 annotation).
// Same Simple RGB/minimum equation, with source texture*vertex alpha. Kill is
// independent of EM2's disabled fixed-function alpha testing. c252=127/255;
// greater=true rejects alpha<=cutoff; otherwise rejects alpha<cutoff.
inline std::vector<uint32_t> BuildSimplePropertyCutoutPixelProgram(bool greater) {
  return {0x00011002,0x00001200,0xC4000000,
          0x00004003,0x00002200,0x00000000,
          0x10182001,0x1F1FF688,0x00004000,
          0xC8070001,0x00C0C000,0xA2010200,
          0xC80F0002,0x00000000,0xE1020100,
          0xC80F0003,0x006C1B00,greater?0x7AFC0200u:0x79FC0200u,
          0xC80F8000,0x00000000,0xE2020200};
}
// Wii GEN_DiffuseAlpha_VertexColor_AlphaTest: modulate2x RGB (the vertex
// shader already doubles color), preserve texture alpha. Cutout rejection is
// in the shader, like EM2's material pipeline (which disables fixed-function
// alpha testing). The private FXLO binds c252.x to the authored 128/255 cutoff.
inline std::vector<uint32_t> BuildWiiCutoutPixelProgram() {
  return {
      0x00011002,0x00001200,0xC4000000, // One texture fetch, allocation.
      0x00003003,0x00002200,0x00000000, // Three ALUs, end.
      0x10182001,0x1F1FF688,0x00004000, // r2 = sampler1(UV0), RGBA.
      0xC80F0003,0x006C1B00,0x79FC0200, // Discard when c252.x > r2.a.
      0xC9070002,0x00000000,0xE1020100, // r2.rgb = saturate(r2.rgb*r1.rgb).
      0xC80F8000,0x00000000,0xE2020200, // Export r2 including texture alpha.
  };
}
enum class WiiBlendVertexInterface { NativeBlend, NativeSimple, PackedDualUv, PackedDualSimple };
// Original Wii TEV blend equations, with the native StaticEnv_Blend vertex
// interface: r0.xy is UV0; r2.rgb is twice vertex RGB, r2.a is alpha.
// PackedDualUv uses r0.xy/zw and r3 for color; PackedDualSimple uses the same
// UV packing with color in r1 and avoids the retail tangent inputs. Native vertex binding
// still requires separate validation before any live material can use it. These programs are not enabled until GPU validation and
// native effect registration have both succeeded.
// These are newly assembled instructions, not a copied game shader. The FXLO
// adapter must bind shader maps 0/1/2 to samplers 1/3/2 respectively.
inline std::vector<uint32_t> BuildWiiBlendPixelProgram(bool masked,
    WiiBlendVertexInterface vertex_interface=WiiBlendVertexInterface::NativeBlend,
    bool property_cutout_gt127=false) {
  if(property_cutout_gt127 && !masked)throw std::invalid_argument("Property mask cutout requires masked source alpha");
  const bool dual=vertex_interface==WiiBlendVertexInterface::PackedDualUv ||
                  vertex_interface==WiiBlendVertexInterface::PackedDualSimple;
  const unsigned color=vertex_interface==WiiBlendVertexInterface::PackedDualUv?3:
      (vertex_interface==WiiBlendVertexInterface::NativeSimple ||
       vertex_interface==WiiBlendVertexInterface::PackedDualSimple)?1:2;
  const unsigned a=color==3?4:3,b=a+1,mask=a+2,temporary=a+3,one=a+4;
  using Instruction = std::array<uint32_t, 3>;
  const auto broadcast = [](unsigned lane) {
    unsigned temporary = 0;
    for (unsigned i=0; i<4; ++i) temporary |= ((lane-i)&3) << (2*i);
    return temporary;
  };
  const auto alu = [](unsigned dest, unsigned mask, unsigned op,
                      unsigned a, unsigned b, unsigned c=0,
                      unsigned as=0, unsigned bs=0, unsigned cs=0,
                      bool negate_b=false, bool clamp=false) -> Instruction {
    return {0xC8000000u | (uint32_t(clamp)<<24) | (mask<<16) | dest,
            (as<<16) | (bs<<8) | cs | (uint32_t(negate_b)<<25),
            0xE0000000u | (op<<24) | (a<<16) | (b<<8) | c};
  };
  const auto texture = [](unsigned dest, unsigned sampler, bool uv1) -> Instruction {
    // Normalized 2D coordinates, implicit derivatives, filters from the native
    // sampler, RGBA destination. UV swizzles are absolute in fetch instructions.
    return {1u | (dest<<12) | (1u<<19) | (sampler<<20) |
                ((uv1 ? 14u : 4u)<<26),
            0x1F1FF688u, 0x00004000u};
  };
  std::vector<Instruction> fetches{texture(a,1,false), texture(b,3,dual)};
  std::vector<Instruction> arithmetic;
  const unsigned alpha=broadcast(3);
  if (masked) {
    fetches.push_back(texture(mask,2,false));
    // saturate(2 * (vertexAlpha * maskAlpha + vertexAlpha - 0.5)).
    // SETE makes one without depending on a donor effect's constant defaults.
    arithmetic.push_back(alu(one,15,4,color,color));
    arithmetic.push_back(alu(temporary,15,1,color,mask,0,alpha,alpha));
    arithmetic.push_back(alu(temporary,15,0,temporary,color,0,0,alpha));
    arithmetic.push_back(alu(temporary,15,0,temporary,temporary));
    arithmetic.push_back(alu(temporary,15,0,temporary,one,0,0,0,0,true,true));
  }
  // T1 + weight * (T0 - T1). Clamp after modulation, as the source TEV stage.
  arithmetic.push_back(alu(mask,7,0,a,b,0,0,0,0,true));
  arithmetic.push_back(alu(temporary,7,11,mask,masked?temporary:color,b,0,masked?0:alpha));
  arithmetic.push_back(alu(temporary,7,1,temporary,color,0,0,0,0,false,true));
  if (masked) arithmetic.push_back(alu(temporary,8,2,a,a));
  if(property_cutout_gt127){
    // Source masked material's final alpha is texture0 alpha, NOT its vertex
    // alpha blend weight. GREATER127 rejects cutoff>=finalAlpha even when
    // native fixed-function alpha testing is disabled. Default path unchanged.
    auto kill=alu(one,15,26,252,temporary,0,broadcast(0),alpha);
    kill[2]&=~0x80000000u; // First operand is c252, second is temporary.w.
    arithmetic.push_back(kill);
  }
  auto output=alu(0,15,2,temporary,temporary);
  output[0] |= 0x8000u; // Color export.
  if (!masked) output[0] |= 8u<<20; // Overlapping masks export constant alpha 1.
  arithmetic.push_back(output);

  struct Group { uint64_t control; std::vector<Instruction> instructions; };
  std::vector<Group> groups;
  constexpr uint64_t exec=(uint64_t(1)<<44)|(uint64_t(1)<<41);
  uint64_t sequence=0;
  for (unsigned i=0;i<fetches.size();++i) sequence|=uint64_t(1)<<(16+2*i);
  groups.push_back({exec|sequence,fetches});
  groups.push_back({(uint64_t(12)<<44)|(uint64_t(2)<<41),{}});
  for (size_t begin=0;begin<arithmetic.size();begin+=6) {
    const size_t end=std::min(begin+6,arithmetic.size());
    auto control=exec;
    if (end==arithmetic.size()) control=(control&~(uint64_t(15)<<44))|(uint64_t(2)<<44);
    groups.push_back({control,{arithmetic.begin()+begin,arithmetic.begin()+end}});
  }
  if (groups.size()%2) groups.push_back({0,{}});
  std::vector<uint64_t> controls;
  std::vector<uint32_t> body;
  for (auto& group:groups) {
    if (!group.instructions.empty()) {
      group.control |= groups.size()/2+body.size()/3;
      group.control |= uint64_t(group.instructions.size())<<12;
      for (const auto& instruction:group.instructions)
        body.insert(body.end(),instruction.begin(),instruction.end());
    }
    controls.push_back(group.control);
  }
  std::vector<uint32_t> result;
  for (size_t i=0;i<controls.size();i+=2) {
    const auto a=controls[i], b=controls[i+1];
    result.insert(result.end(),{uint32_t(a),uint32_t((a>>32)|((b&65535)<<16)),uint32_t(b>>16)});
  }
  result.insert(result.end(),body.begin(),body.end());
  return result;
}
} // namespace em2::em1

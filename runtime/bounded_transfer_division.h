#pragma once
#include <rex/graphics/format/dxbc.h>
#include <rex/graphics/xenos.h>
#include <filesystem>

namespace rex::ui::d3d12 { class D3D12Provider; }
namespace em2 {
// Transfer shader register contract: r1.w contains an already wrapped EDRAM
// tile index (0..2047), r2.x contains the unsigned source pitch. Return quotient
// in r1.w, remainder in r2.x; r2.yzw are scratch, as in the original UDiv site.
// This is deliberately NOT a general uint division replacement.
inline void EmitBoundedTransferDivision(rex::graphics::dxbc::Assembler& a) {
  namespace d = rex::graphics::dxbc;
  static_assert(rex::graphics::xenos::kEdramTileCount == 2048);
  // SM5 rcp has maximum relative error 2^-21. Integer n <= 2047 and n+2^-9
  // are exactly representable. Including float multiplication rounding, the
  // computed numerator differs from n by strictly (0, .0031). Consequently
  // truncation yields floor(n/d) for every positive integral d: adjacent
  // quotients are separated by at least 1/d. No correction iteration needed.
  // https://learn.microsoft.com/windows/win32/direct3dhlsl/rcp--sm5---asm-
  a.OpMov(d::Dest::R(2, 0b1000), d::Src::R(1, d::Src::kWWWW));
  a.OpUToF(d::Dest::R(2, 0b0010), d::Src::R(1, d::Src::kWWWW));
  a.OpUToF(d::Dest::R(2, 0b0100), d::Src::R(2, d::Src::kXXXX));
  a.OpRcp(d::Dest::R(2, 0b0100), d::Src::R(2, d::Src::kZZZZ));
  a.OpAdd(d::Dest::R(2, 0b0010), d::Src::R(2, d::Src::kYYYY),
          d::Src::LF(1.0f / 512.0f));
  a.OpMul(d::Dest::R(2, 0b0010), d::Src::R(2, d::Src::kYYYY),
          d::Src::R(2, d::Src::kZZZZ));
  a.OpFToU(d::Dest::R(1, 0b1000), d::Src::R(2, d::Src::kYYYY));
  // Match DXBC UDiv even for the invalid zero pitch: BOTH results are UINT_MAX.
  a.OpIEq(d::Dest::R(2, 0b0100), d::Src::R(2, d::Src::kXXXX), d::Src::LU(0));
  a.OpIMAd(d::Dest::R(2, 0b0001), -d::Src::R(1, d::Src::kWWWW),
           d::Src::R(2, d::Src::kXXXX), d::Src::R(2, d::Src::kWWWW));
  a.OpMovC(d::Dest::R(1, 0b1000), d::Src::R(2, d::Src::kZZZZ),
           d::Src::LU(UINT32_MAX), d::Src::R(1, d::Src::kWWWW));
  a.OpMovC(d::Dest::R(2, 0b0001), d::Src::R(2, d::Src::kZZZZ),
           d::Src::LU(UINT32_MAX), d::Src::R(2, d::Src::kXXXX));
}
bool TestBoundedTransferDivision(const rex::ui::d3d12::D3D12Provider&,
                                 const std::filesystem::path& output);
}

#pragma once
#include "ppc_context.h"
#include <rex/cvar.h>
REXCVAR_DECLARE(bool,em2_native_matrix_stores);
namespace em2 {
// Preserve the reviewed original x86 instruction operand order even when
// coalescing stores changes register allocation. NaN payload choice depends
// on that order; ordinary C++ SIMD add/multiply may be commuted by LLVM.
// Force VEX3 as LLVM's assembler can also commute VEX operands to shorten
// their encoding, including inside inline assembly.
inline simde__m128 MatrixOrderedMultiply(simde__m128 lhs,simde__m128 rhs) {
    simde__m128 result;
    __asm__("%{vex3%} vmulps %2, %1, %0":"=x"(result):"x"(lhs),"x"(rhs));
    return result;
}
inline simde__m128 MatrixOrderedAdd(simde__m128 lhs,simde__m128 rhs) {
    simde__m128 result;
    __asm__("%{vex3%} vaddps %2, %1, %0":"=x"(result):"x"(lhs),"x"(rhs));
    return result;
}
inline bool NativeMatrixStoresSupported(uint32_t destination) {
    destination&=~3u;
    const uint64_t end=uint64_t(destination)+64;
    return end<=0x100000000ull &&
        PPC_PHYS_HOST_OFFSET(destination)==PPC_PHYS_HOST_OFFSET(uint32_t(end-1)) &&
        !(uint64_t(destination)<0x80000000ull && end>0x7F000000ull);
}
inline void StoreNativeMatrixRow(uint32_t address,simde__m128i row,uint8_t* base) {
    // stvewx chooses a word using the destination's position in a 16-byte
    // vector and then byte-swaps that word. Pack exactly those four choices.
    alignas(16) static constexpr uint8_t masks[4][16]={
        {15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0},
        {11,10,9,8,7,6,5,4,3,2,1,0,15,14,13,12},
        {7,6,5,4,3,2,1,0,15,14,13,12,11,10,9,8},
        {3,2,1,0,15,14,13,12,11,10,9,8,7,6,5,4}};
    simde_mm_storeu_si128((simde__m128i*)PPC_RAW_ADDR(address),
        simde_mm_shuffle_epi8(row,simde_mm_load_si128((const simde__m128i*)masks[(address&15)/4])));
}
inline bool NativeMatrixStoresEnabled(){return REXCVAR_GET(em2_native_matrix_stores);}
bool TestNativeMatrixMath();
}

#pragma once
#include "ppc_context.h"

namespace em2 {
// Preserve the SDK's access width and E0 physical-heap bias, while describing
// the overwhelmingly virtual-address path as likely. This is a measured
// candidate, not an unchecked assumption that physical pointers never occur.
__attribute__((always_inline)) inline uint32_t LikelyGuestWord(uint32_t address,uint8_t* base) {
#ifdef _WIN32
    if(address>=0xE0000000u)[[unlikely]]
        return __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(base+address+0x1000));
    return __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(base+address));
#else
    return PPC_LOAD_U32(address);
#endif
}
// Experimental per-routine reader. Keep the SDK's exact address translation
// and four-byte access, without adding any CPU instruction requirement.
template<bool Barrier> __attribute__((always_inline)) uint32_t ExplicitGuestWord(uint32_t address,uint8_t* base) {
    const auto* pointer=reinterpret_cast<const volatile uint32_t*>(PPC_RAW_ADDR(address));
    uint32_t value;
    if constexpr(Barrier)
        __asm__ volatile("movl %1, %0\n\tbswapl %0" : "=r"(value) : "m"(*pointer) : "memory");
    else
        __asm__ volatile("movl %1, %0\n\tbswapl %0" : "=r"(value) : "m"(*pointer));
    return value;
}
}

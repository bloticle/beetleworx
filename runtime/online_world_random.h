#pragma once
#include <cstdint>
#include <string_view>
namespace em2::net {
struct Identity;
enum class InitializationPolicy:uint8_t;
// Snapshot once when Host/Join begins. The negotiated session policy, rather
// than this mutable debug preference, owns initialization after adoption.
InitializationPolicy ConfiguredNativeInitializationPolicy();
uint32_t NativeWorldInitializationSeed(const Identity&,std::string_view destination,uint64_t sequence);
uint32_t NativePlayerInitializationSeed(const Identity&,uint32_t slot,uint32_t mode,uint64_t sequence);
// Called at the audited native loader boundary, on the retained owner thread.
bool PrepareNativeOnlineWorldRandom(std::string_view destination);
bool PrepareNativeOnlinePlayerRandom(uint32_t slot,uint32_t mode);
bool TestNativeWorldInitializationSeed();
}

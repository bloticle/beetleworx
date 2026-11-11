#pragma once
#include <array>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>
#include <span>
#include <memory>
#include <string_view>
struct PPCContext;
namespace em2 {
struct NativeActor {uint32_t entity=0,motion=0,animation=0;bool ready=false;uint64_t identity=0;};
uint32_t DiscoverNativeAnimation(uint32_t entity,uint8_t* base);
bool NativeAnimationGraphHasEvent(uint32_t animation,uint32_t event,uint8_t* base);
bool NativeAnimationEvent(PPCContext&,uint8_t*,uint32_t animation,const std::string& name);
uint32_t NativeAnimationInterruptSerial(uint32_t animation);
struct NativeStimulus {unsigned type;float amount;};
// One native impact may carry both melee damage and knockback.
bool NativeStimuliSphere(PPCContext&,uint8_t*,uint32_t entity,std::span<const NativeStimulus>,float radius);
bool NativeStimulusSphere(PPCContext&,uint8_t*,uint32_t entity,unsigned type,float amount,float radius);
void UpdateNativeEngineActors(const std::array<NativeActor,2>&);
std::optional<std::string> QueueNativeEngineCommand(const std::vector<std::string>&);
void RunNativeEngineCommands(PPCContext&,uint8_t*);
int NativeConsoleTarget();
bool NativeConsoleWorldReady();
// version invalidates actor-bound console work when a player/motion changes.
// lifecycle changes only at an explicit native world transition, and remains
// stable while players, cameras and physics components settle or respawn.
struct NativeConsoleWorldStatus {bool ready=false;uint64_t version=0,lifecycle=0;};
NativeConsoleWorldStatus NativeConsoleWorldState();
void InvalidateNativeConsoleWorld();
void SetNativeConsoleTarget(int actor);
struct NativeGraphVariable {std::string name;float floating=0;int32_t integer=0;};
struct NativeGraphSnapshot {unsigned actor=0;uint64_t identity=0;std::vector<std::string> events;std::vector<NativeGraphVariable> variables;};
std::shared_ptr<const NativeGraphSnapshot> NativeAnimationSnapshot();
std::span<const std::string_view> NativeEngineCommandNames();
// UI-side completion reads immutable names copied on the game thread; it
// never traverses guest memory or the mutable native identifier caches.
std::optional<std::vector<std::string>> NativeEngineArgumentCompletions(std::string_view command,unsigned argument,std::string_view prefix);
bool TestNativeEngine();
bool TestNativeEngineMemory(PPCContext&,uint8_t*,uint32_t scratch);
}

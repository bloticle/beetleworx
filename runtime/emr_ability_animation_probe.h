#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
struct PPCContext;
namespace em2 {
// Developer QA after native kernel/image initialization, before world admission.
// Uses private output allocations; optionally samples the actor's already-bound
// native resources. Never activates a state or writes an actor's pose.
bool ProbeEMRAbilityClips(PPCContext&,uint8_t* base,const std::filesystem::path& directory,std::string& error,uint32_t actor_animation=0);
}

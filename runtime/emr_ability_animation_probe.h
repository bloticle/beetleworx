#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
struct PPCContext;
namespace em2 {
// Developer QA after native kernel/image initialization, before world admission.
// Uses private guest allocations only; does not attach a clip or touch an actor.
bool ProbeEMRAbilityClips(PPCContext&,uint8_t* base,const std::filesystem::path& directory,std::string& error);
}

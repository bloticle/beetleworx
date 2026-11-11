#pragma once
#include "em1_native_scene_pak.h"
#include <string>
#include <string_view>
#include <span>
#include <cstdint>
namespace rex::filesystem {class VirtualFileSystem;}
namespace em2::em1 {
struct NativeScenePakRequest {
  std::string basename;
  std::vector<NativeScenePakEntry> scenes;
  bool compress=true;
};
// One read-only VFS overlay for all archives in a level. Call on the loading
// thread; the scene spans need only remain valid until this call returns.
bool MountNativeScenePaks(rex::filesystem::VirtualFileSystem&,
    std::span<const NativeScenePakRequest>,std::string& error,
    std::vector<std::shared_ptr<const std::vector<uint8_t>>>* mounted_bytes=nullptr);
bool MountNativeScenePak(rex::filesystem::VirtualFileSystem&,std::string_view basename,
    std::span<const NativeScenePakEntry>,std::string& error);
// Startup-only immutable player-archive replacement. Uses the same verified
// VFS forwarding as source levels; no writes to the staged or original PAK.
bool MountNativePlayerPak(rex::filesystem::VirtualFileSystem&,
    std::shared_ptr<const std::vector<uint8_t>>,std::string& error);
// Process-lifetime, read-only native PAK containing the adapted original scene.
// The native archive pipeline publishes the raw scene before readiness checks.
bool MountSingleGsaPak(rex::filesystem::VirtualFileSystem&,std::string_view basename,
    std::string_view scene_name,std::span<const uint8_t> scene,std::string& error);
}

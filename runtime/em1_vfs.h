#pragma once
#include "em1_rvz.h"

namespace rex::filesystem { class VirtualFileSystem; }
namespace em2::em1 {
// Mounts original asset bytes at em1: without changing game: or any existing
// device. Caller must close all guest handles/tasks before VFS destruction.
// This alone does not authorize feeding original Wii formats to EM2 loaders.
bool MountRvz(rex::filesystem::VirtualFileSystem& filesystem,
               std::shared_ptr<RvzSource> source, std::string& error);
struct LevelCatalog {
  std::string source_identity,error;
  std::vector<std::string> descriptors;
};
// Immutable metadata captured at mount time; safe for console/render threads.
std::shared_ptr<const LevelCatalog> CurrentLevelCatalog();
// Loading-thread access to the mounted original source. Mount/unmount must be
// serialized with loaders; the returned shared owner survives normal reads.
std::shared_ptr<RvzSource> MountedRvz(rex::filesystem::VirtualFileSystem& filesystem);
}

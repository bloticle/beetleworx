#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>
namespace em2::net {
inline constexpr size_t kMaxCheckpointBundle=33*1024*1024;
struct CheckpointBundle {
    std::vector<uint8_t> bytes;
    std::array<uint8_t,32> identity{};
    uint64_t files=0,payload_bytes=0;
};
struct CheckpointBundleResult {std::optional<CheckpointBundle> value;std::string error;};
struct CampaignSaveSourceResult {std::optional<std::filesystem::path> root;std::string error;};
// UI preflight only: accept a title/user_data/content-type/SAVEGAME folder or
// save.dat selection, returning the title root expected by native startup.
// Checks presence/readability, not the internal validity of the retail save.
CampaignSaveSourceResult ResolveCampaignSaveSource(const std::filesystem::path& selection);
// Worker operations. Source must be frozen; destination must not exist.
CheckpointBundleResult PackCheckpoint(const std::filesystem::path& frozen,std::stop_token={});
struct CheckpointInstallResult {bool installed=false;std::string error;};
CheckpointInstallResult InstallCheckpoint(std::span<const uint8_t>,const std::array<uint8_t,32>& expected,
    const std::filesystem::path& new_directory,std::stop_token={});
bool TestCheckpointBundles(const std::filesystem::path& fixture);
bool TestCheckpointTransport(const std::filesystem::path& fixture);
bool WriteNewCheckpointBundle(std::span<const uint8_t>,const std::filesystem::path& new_file);
}

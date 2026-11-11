#pragma once
#include <filesystem>
#include <span>
#include <string>
#include <vector>
#include <cstdint>
#include <array>

namespace em2 {
inline constexpr std::array<const char*,7> kPatchedGameArchives={
    "UIOptions.pak","UIPauseOptions.pak","UIhud.pak","UIStartGame.pak",
    "UIMainMenu.pak","_Dynamic.pak","shaders.pak"};
// A bounded copy/literal patch. Copies reference only the validated original;
// neither the patch nor the result can name any filesystem paths.
bool DecodeContentPatch(std::span<const uint8_t> original,std::span<const uint8_t> patch,
                        std::vector<uint8_t>& output,std::string& error);
bool ApplyContentPatch(const std::filesystem::path& original,const std::filesystem::path& patch,
                       const std::filesystem::path& destination,std::string& error);
bool TestContentPatches(const std::filesystem::path& isolated_directory);
}

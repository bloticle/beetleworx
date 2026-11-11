#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <cstdint>
namespace em2::em1 {
// Existing executable, SDK tool mode only: load the user's real XEX without
// guest execution, graphics or audio output. Copies the enum into host data.
bool ReadHeadlessTargetAnimationVariables(const std::filesystem::path& game_directory,
    std::map<std::string,uint32_t>& output,std::string& error);
}

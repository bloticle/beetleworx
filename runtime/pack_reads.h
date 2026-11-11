#pragma once
#include <filesystem>
namespace em2 {
void ConfigurePackReads(const std::filesystem::path& read_only_content);
bool TestPackReads(const std::filesystem::path& directory);
}

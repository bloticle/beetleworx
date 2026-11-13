#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
namespace em2 {
// Return false to cancel. Both validation and extraction stream bounded chunks.
using DiscImportProgress=std::function<bool(std::wstring_view,uint64_t,uint64_t)>;
bool ImportXbox360Disc(const std::filesystem::path& image,const std::filesystem::path& destination,
                       const DiscImportProgress& progress,std::string& error);
bool TestDiscImageImport(const std::filesystem::path& isolated_directory);
// Read-only comparison of every disc file against an existing extracted copy.
bool VerifyXbox360Disc(const std::filesystem::path& image,const std::filesystem::path& reference,std::string& error);
}

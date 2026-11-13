#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace em2::em1 {
struct RvzFile { std::string path; uint64_t size = 0; };
// Owns one read-only source mount. All paths and caches belong to EM1 and this
// source identity; callers must not place results in EM2's unqualified cache.
class RvzSource {
 public:
  static std::unique_ptr<RvzSource> Open(const std::filesystem::path& image,
                                       std::string& error);
  static bool TestGenericSource(const std::filesystem::path& image,std::string& error);
  static bool TestFolderSource(const std::filesystem::path& isolated_directory,std::string& error);
  ~RvzSource();
  RvzSource(const RvzSource&) = delete;
  RvzSource& operator=(const RvzSource&) = delete;
  const std::string& DiscId() const;
  const std::string& SourceIdentity() const;
  const std::vector<RvzFile>& Files() const;
  bool ReadFile(std::string_view path, std::vector<uint8_t>& output,
                std::string& error);
  bool ReadFileRange(std::string_view path, uint64_t offset, uint64_t length,
                     std::vector<uint8_t>& output, std::string& error);
 private:
  struct Impl;
  explicit RvzSource(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
}  // namespace em2::em1

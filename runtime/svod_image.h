#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
namespace em2 {
class SvodImage {
 public:
  static std::unique_ptr<SvodImage> Open(const std::filesystem::path&,std::string& error);
  static std::filesystem::path FindPackage(const std::filesystem::path& folder,std::string& error);
  ~SvodImage();
  uint64_t Size() const;
  const std::array<uint8_t,2048>& Descriptor() const;
  std::span<const uint8_t> IdentityPrefix() const;
  bool Read(uint64_t offset,std::span<uint8_t> bytes,std::string& error);
 private:
  struct Impl;explicit SvodImage(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
}

#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include "archive_image.h"
namespace em2 {
using ZarFile=ArchiveFile;
class ZarImage : public ArchiveImage {
 public:
  static std::unique_ptr<ZarImage> Open(const std::filesystem::path&,std::string&,const std::function<bool(uint64_t,uint64_t)>& progress={});
  ~ZarImage() override;
  uint64_t Size() const override;
  std::span<const uint8_t> Identity() const override;
  const std::vector<ZarFile>& Files() const override;
  bool Read(uint64_t,std::span<uint8_t>,std::string&) override;
 private:
  struct Impl;explicit ZarImage(std::unique_ptr<Impl>);std::unique_ptr<Impl> impl_;
};
}

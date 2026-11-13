#pragma once
#include "archive_image.h"
#include <filesystem>
#include <functional>
#include <memory>
namespace em2 {
class ZipImage : public ArchiveImage {
 public:
  static std::unique_ptr<ZipImage> Open(const std::filesystem::path&,std::string&,const std::function<bool(uint64_t,uint64_t)>& progress={});
  ~ZipImage() override;
  uint64_t Size() const override;
  std::span<const uint8_t> Identity() const override;
  const std::vector<ArchiveFile>& Files() const override;
  bool Read(uint64_t,std::span<uint8_t>,std::string&) override;
 private:
  struct Impl;explicit ZipImage(std::unique_ptr<Impl>);std::unique_ptr<Impl> impl_;
};
}

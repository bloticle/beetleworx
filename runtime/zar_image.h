#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>
namespace em2 {
struct ZarFile {std::string path;uint64_t offset=0,size=0;bool directory=false;};
class ZarImage {
 public:
  static std::unique_ptr<ZarImage> Open(const std::filesystem::path&,std::string&,const std::function<bool(uint64_t,uint64_t)>& progress={});
  ~ZarImage();
  uint64_t Size() const;
  std::span<const uint8_t> Identity() const;
  const std::vector<ZarFile>& Files() const;
  bool Read(uint64_t,std::span<uint8_t>,std::string&);
 private:
  struct Impl;explicit ZarImage(std::unique_ptr<Impl>);std::unique_ptr<Impl> impl_;
};
}

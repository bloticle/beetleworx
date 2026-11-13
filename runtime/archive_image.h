#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace em2 {
struct ArchiveFile {std::string path;uint64_t offset=0,size=0;bool directory=false;};
class ArchiveImage {
 public:
  virtual ~ArchiveImage()=default;
  virtual uint64_t Size() const=0;
  virtual std::span<const uint8_t> Identity() const=0;
  virtual const std::vector<ArchiveFile>& Files() const=0;
  virtual bool Read(uint64_t,std::span<uint8_t>,std::string&)=0;
};
}

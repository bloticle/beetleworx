#pragma once
#include "em1_rvz.h"
#include "em1_inflate.h"
#include <array>
#include <functional>
#include <span>

namespace em2::em1 {
struct PakEntry {
  std::string path;
  std::array<uint8_t, 4> type{};
  uint32_t size = 0;
  uint32_t stored_size = 0;
};
struct PackedResource {
  PakEntry entry;
  std::vector<uint8_t> bytes;
  bool IsCompressed() const { return entry.size != entry.stored_size; }
};
// The inflater must consume the complete Zlib stream, verify its checksum, and
// fill exactly the output span. No guest pointer or unbounded allocation is
// exposed through this interface. The default is the statically linked native
// decoder. An explicitly absent inflater permits raw assets only.
using PakInflater = std::function<bool(std::span<const uint8_t>,
                                      std::span<uint8_t>, std::string&)>;

// An archive belongs to one original EM1 disc source. Opening reads only its
// directory; reading a resource fetches only the corresponding RVZ ranges.
// This is not an EM2 resource manager replacement or proof of native playback.
class PakSource {
 public:
  static std::unique_ptr<PakSource> Open(std::shared_ptr<RvzSource> source,
                                       std::string_view archive,
                                       std::string& error);
  ~PakSource();
  PakSource(const PakSource&) = delete;
  PakSource& operator=(const PakSource&) = delete;
  const std::string& OriginKey() const;
  const std::vector<PakEntry>& Entries() const;
  bool ReadPacked(std::string_view path, PackedResource& output,
                  std::string& error);
  bool ReadOriginal(std::string_view path, std::vector<uint8_t>& output,
                    std::string& error,
                    const PakInflater& inflate = InflateZlib);
 private:
  struct Impl;
  explicit PakSource(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};
}  // namespace em2::em1

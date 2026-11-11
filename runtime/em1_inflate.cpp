#include "em1_inflate.h"
#define Z_SOLO
#define Z_PREFIX
#include "em1_compat/zlib/zlib.h"
#include <cstdlib>

namespace em2::em1 {
namespace {
struct Allocator { size_t allocated = 0; };
voidpf Allocate(voidpf opaque, uInt count, uInt width) {
  auto& allocator = *static_cast<Allocator*>(opaque);
  constexpr size_t limit = 1024 * 1024;
  if (!count || !width || count > limit / width) return nullptr;
  const size_t bytes = size_t(count) * width;
  if (bytes > limit - allocator.allocated) return nullptr;
  auto* result = std::calloc(count, width);
  if (result) allocator.allocated += bytes;
  return result;
}
void Free(voidpf, voidpf allocation) { std::free(allocation); }
}

bool InflateZlib(std::span<const uint8_t> compressed,
                 std::span<uint8_t> output, std::string& error) {
  constexpr size_t maximum = 256 * 1024 * 1024;
  if (compressed.size() < 6 || compressed.size() > maximum ||
      output.size() > maximum) {
    error = "Invalid EM1 Zlib stream bounds";
    return false;
  }
  Allocator allocator;
  z_stream stream{};
  stream.opaque = &allocator;
  stream.zalloc = Allocate;
  stream.zfree = Free;
  if (inflateInit(&stream) != Z_OK) {
    error = "Cannot initialize EM1 Zlib decoder";
    return false;
  }
  struct End {
    z_stream* stream;
    ~End() { inflateEnd(stream); }
  } end{&stream};
  uint8_t empty_output = 0;
  stream.next_in = const_cast<Bytef*>(compressed.data());
  stream.avail_in = uInt(compressed.size());
  stream.next_out = output.empty() ? &empty_output : output.data();
  stream.avail_out = uInt(output.empty() ? 1 : output.size());
  if (inflate(&stream, Z_FINISH) != Z_STREAM_END || stream.avail_in != 0 ||
      stream.total_out != output.size()) {
    error = "Invalid EM1 Zlib stream, checksum, or exact output size";
    return false;
  }
  error.clear();
  return true;
}
}  // namespace em2::em1

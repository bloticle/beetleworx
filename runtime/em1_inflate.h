#pragma once
#include <cstdint>
#include <span>
#include <string>

namespace em2::em1 {
// Exact bounded Zlib stream: no gzip/raw-deflate fallback, trailing data,
// preset dictionaries, unchecked checksums, or size mismatches.
bool InflateZlib(std::span<const uint8_t> compressed,
                 std::span<uint8_t> output, std::string& error);
}

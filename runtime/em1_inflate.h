#pragma once
#include <cstdint>
#include <span>
#include <string>

namespace em2::em1 {
// Exact bounded Zlib stream: no gzip/raw-deflate fallback, trailing data,
// preset dictionaries, unchecked checksums, or size mismatches.
bool InflateZlib(std::span<const uint8_t> compressed,
                 std::span<uint8_t> output, std::string& error);
// CSO raw-DEFLATE block, with only the container's bounded alignment padding.
bool InflateDiscBlock(std::span<const uint8_t>,std::span<uint8_t>,size_t minimum_output,
                      size_t maximum_padding,size_t& produced,std::string& error);
}

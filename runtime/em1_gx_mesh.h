#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace em2::em1 {
struct GxMeshTopology {
  uint32_t attributes = 0;
  uint32_t source_index_bytes = 0;
  // Flattened complete index tuples in first-use order. No position-only weld:
  // color seams and every UV channel remain distinct. Values index each source
  // attribute's region, allowing byte-exact attribute copies and BKLT mapping.
  std::vector<uint32_t> attribute_indices;
  std::vector<uint32_t> triangle_indices;
};
// Only static GX draws whose attribute layout has been verified by the NIF
// caller. Counts must follow GX order: position, normal, colors, texture coords.
// Does not infer skin/matrix indices, quantization, or convert an entire NIF.
// Ambiguous widths require an exact oriented triangle-multiset match against
// the original position INDEX stream. Neither width is preferred heuristically.
// Failure keeps output.
bool DecodeGxMesh(std::span<const uint8_t> commands,
                  std::span<const uint32_t> attribute_counts,
                  GxMeshTopology& output, std::string& error,
                  std::span<const uint32_t> source_position_triangles={});
}

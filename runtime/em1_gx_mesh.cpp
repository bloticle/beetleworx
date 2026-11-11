#include "em1_gx_mesh.h"
#include <array>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace em2::em1 {
namespace {
constexpr size_t kMaxCommands = 16 * 1024 * 1024;
constexpr size_t kMaxReferences = 1 << 20;
constexpr size_t kMaxVertices = 1 << 18;
constexpr size_t kMaxTriangleIndices = 3 << 20;
constexpr size_t kMaxAttributes = 12;  // position + normal + 2 colors + 8 UVs
struct Primitive { uint32_t offset; uint16_t count; uint8_t kind; };
using Tuple = std::array<uint32_t, kMaxAttributes>;
uint16_t Short(std::span<const uint8_t> input, size_t offset) {
  return uint16_t(input[offset]) << 8 | input[offset + 1];
}
void Require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
bool Parse(std::span<const uint8_t> input, std::span<const uint32_t> counts,
           unsigned width, std::vector<Primitive>& result) {
  size_t at = 0, referenced = 0;
  while (at < input.size()) {
    const auto command = input[at++];
    if (!command) continue;  // GX NOP padding
    const auto kind = command & 0xf8;
    if ((kind != 0x80 && kind != 0x90 && kind != 0x98 && kind != 0xa0) ||
        input.size() - at < 2) return false;
    const auto count = Short(input, at); at += 2;
    if (!count || count > (input.size() - at) / (counts.size() * width) ||
        count > kMaxReferences - referenced ||
        (kind == 0x80 && count % 4) || (kind == 0x90 && count % 3)) return false;
    result.push_back({uint32_t(at), count, uint8_t(kind)});
    referenced += count;
    for (unsigned i = 0; i < count; ++i) {
      for (const auto limit : counts) {
        const auto value = width == 1 ? input[at] : Short(input, at);
        if (value >= limit) return false;
        at += width;
      }
    }
  }
  return !result.empty();
}
struct Hash {
  size_t operator()(const Tuple& tuple) const noexcept {
    uint64_t hash = 14695981039346656037ull;
    for (const auto value : tuple) { hash ^= value; hash *= 1099511628211ull; }
    return size_t(hash);
  }
};
using Face=std::array<uint32_t,3>;
void FaceInto(std::vector<Face>& faces,uint32_t a,uint32_t b,uint32_t c){
  if(a==b || b==c || a==c)return;
  Require(faces.size()<kMaxTriangleIndices/3,"GX triangle comparison exceeds limit");
  faces.push_back(std::min({Face{a,b,c},Face{b,c,a},Face{c,a,b}}));
}
bool MatchesPositions(std::span<const uint8_t> commands,std::span<const Primitive> primitives,
    size_t attributes,unsigned width,const std::vector<Face>& expected){
  std::vector<Face> faces;
  for(const auto& primitive:primitives){
    std::vector<uint32_t> positions;positions.reserve(primitive.count);
    for(size_t i=0,at=primitive.offset;i<primitive.count;++i,at+=attributes*width)
      positions.push_back(width==1?commands[at]:Short(commands,at));
    if(primitive.kind==0x80){
      for(size_t i=0;i<positions.size();i+=4){FaceInto(faces,positions[i],positions[i+1],positions[i+2]);FaceInto(faces,positions[i],positions[i+2],positions[i+3]);}
    }else if(primitive.kind==0x90){
      for(size_t i=0;i<positions.size();i+=3)FaceInto(faces,positions[i],positions[i+1],positions[i+2]);
    }else for(size_t i=2;i<positions.size();++i){
      if(primitive.kind==0xa0)FaceInto(faces,positions[0],positions[i-1],positions[i]);
      else if(i&1)FaceInto(faces,positions[i-1],positions[i-2],positions[i]);
      else FaceInto(faces,positions[i-2],positions[i-1],positions[i]);
    }
  }
  std::sort(faces.begin(),faces.end());return faces==expected;
}
}
bool DecodeGxMesh(std::span<const uint8_t> commands,
                  std::span<const uint32_t> attribute_counts,
                  GxMeshTopology& output, std::string& error,
                  std::span<const uint32_t> source_position_triangles) {
  try {
    Require(!commands.empty() && commands.size() <= kMaxCommands,
            "GX command buffer outside supported bounds");
    Require(!attribute_counts.empty() && attribute_counts.size() <= kMaxAttributes,
            "GX attribute count outside supported bounds");
    for (const auto count : attribute_counts)
      Require(count && count <= 65536, "GX source attribute count outside index range");
    std::vector<Primitive> byte_primitives, short_primitives;
    bool byte_valid = Parse(commands, attribute_counts, 1, byte_primitives);
    bool short_valid = Parse(commands, attribute_counts, 2, short_primitives);
    if(byte_valid && short_valid && !source_position_triangles.empty()){
      Require(source_position_triangles.size()<=kMaxTriangleIndices && source_position_triangles.size()%3==0,"Invalid original GX position triangles");
      for(auto index:source_position_triangles)Require(index<attribute_counts.front(),"Original GX position index exceeds source region");
      std::vector<Face> expected;
      for(size_t i=0;i<source_position_triangles.size();i+=3)
        FaceInto(expected,source_position_triangles[i],source_position_triangles[i+1],source_position_triangles[i+2]);
      std::sort(expected.begin(),expected.end());
      byte_valid=MatchesPositions(commands,byte_primitives,attribute_counts.size(),1,expected);
      short_valid=MatchesPositions(commands,short_primitives,attribute_counts.size(),2,expected);
    }
    Require(byte_valid != short_valid, byte_valid ? "Ambiguous GX index width"
                                                : "Unsupported or malformed GX draw stream");
    const unsigned width = byte_valid ? 1 : 2;
    auto& primitives = byte_valid ? byte_primitives : short_primitives;
    // Drop partial storage from the rejected interpretation before welding.
    std::vector<Primitive>().swap(byte_valid ? short_primitives : byte_primitives);
    GxMeshTopology converted;
    converted.attributes = uint32_t(attribute_counts.size());
    converted.source_index_bytes = width;
    std::unordered_map<Tuple, uint32_t, Hash> unique;
    auto triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
      if (a == b || b == c || a == c) return;
      Require(converted.triangle_indices.size() <= kMaxTriangleIndices - 3,
              "GX triangle output exceeds limit");
      converted.triangle_indices.insert(converted.triangle_indices.end(), {a, b, c});
    };
    std::vector<uint32_t> ids;
    for (const auto& primitive : primitives) {
      ids.clear(); ids.reserve(primitive.count);
      size_t at = primitive.offset;
      for (unsigned vertex = 0; vertex < primitive.count; ++vertex) {
        Tuple tuple{};
        for (size_t attribute = 0; attribute < attribute_counts.size(); ++attribute) {
          tuple[attribute] = width == 1 ? commands[at] : Short(commands, at);
          at += width;
        }
        const auto next = uint32_t(unique.size());
        auto found = unique.find(tuple);
        if (found == unique.end()) {
          Require(unique.size() < kMaxVertices, "GX unique vertex output exceeds limit");
          found = unique.emplace(tuple, next).first;
          converted.attribute_indices.insert(converted.attribute_indices.end(),
              tuple.begin(), tuple.begin() + attribute_counts.size());
        }
        ids.push_back(found->second);
      }
      if (primitive.kind == 0x80) {
        for (size_t i = 0; i < ids.size(); i += 4) {
          triangle(ids[i], ids[i+1], ids[i+2]); triangle(ids[i], ids[i+2], ids[i+3]);
        }
      } else if (primitive.kind == 0x90) {
        for (size_t i = 0; i < ids.size(); i += 3) triangle(ids[i], ids[i+1], ids[i+2]);
      } else {
        for (size_t i = 2; i < ids.size(); ++i) {
          if (primitive.kind == 0xa0) triangle(ids[0], ids[i-1], ids[i]);
          else if (i & 1) triangle(ids[i-1], ids[i-2], ids[i]);
          else triangle(ids[i-2], ids[i-1], ids[i]);
        }
      }
    }
    output = std::move(converted);
    error.clear();
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}
}

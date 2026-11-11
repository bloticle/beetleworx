#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Opaque block preservation: only explicitly adapted blocks and tables change.
namespace em2::em1::nif {
using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;
constexpr size_t kMaximum = 256 * 1024 * 1024;
inline void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
inline uint32_t U32(View bytes, size_t at, bool big) {
  Check(at <= bytes.size() && bytes.size() - at >= 4, "Truncated EM1 NIF word");
  if (big) return uint32_t(bytes[at]) << 24 | uint32_t(bytes[at+1]) << 16 |
                  uint32_t(bytes[at+2]) << 8 | bytes[at+3];
  return uint32_t(bytes[at+3]) << 24 | uint32_t(bytes[at+2]) << 16 |
         uint32_t(bytes[at+1]) << 8 | bytes[at];
}
inline uint16_t U16(View bytes, size_t at, bool big) {
  Check(at <= bytes.size() && bytes.size() - at >= 2, "Truncated EM1 NIF short");
  return big ? uint16_t(bytes[at]) << 8 | bytes[at+1] : uint16_t(bytes[at+1]) << 8 | bytes[at];
}
inline void Append(Bytes& output, View bytes) {
  Check(bytes.size() <= kMaximum - output.size(), "Adapted EM1 NIF exceeds size limit");
  output.insert(output.end(), bytes.begin(), bytes.end());
}
inline void Number(Bytes& output, uint32_t value, bool big, unsigned width = 4) {
  Check(width <= kMaximum - output.size(), "Adapted EM1 NIF exceeds size limit");
  for (unsigned i = 0; i < width; ++i)
    output.push_back(uint8_t(value >> (8 * (big ? width - 1 - i : i))));
}
struct Cursor {
  View data;
  size_t at = 0;
  bool big = true;
  View Take(size_t size) {
    Check(at <= data.size() && size <= data.size() - at, "Truncated EM1 NIF field");
    const auto result = data.subspan(at, size); at += size; return result;
  }
  uint32_t Word() { return U32(Take(4), 0, big); }
  uint16_t Short() { return U16(Take(2), 0, big); }
};
struct Block {
  uint16_t type = 0;
  View original;
  Bytes replacement;
  View Data() const { return replacement.empty() ? original : View(replacement); }
};
struct Nif {
  View prefix, strings, footer;
  bool big = true;
  uint32_t version = 0, user_version = 0, maximum_string = 0;
  std::vector<std::string_view> string_values;
  std::vector<std::string> added_strings;
  std::vector<std::string> types;
  std::vector<Block> blocks;
  explicit Nif(View data) {
    Check(data.size() <= kMaximum && data.size() >= 32, "Invalid EM1 NIF bounds");
    constexpr std::string_view signature = "Gamebryo File Format";
    Check(std::equal(signature.begin(), signature.end(), data.begin()), "Invalid EM1 NIF signature");
    const auto line_end = std::find(data.begin(), data.begin() + std::min<size_t>(128, data.size()), '\n');
    Check(line_end != data.begin() + std::min<size_t>(128, data.size()), "Missing EM1 NIF header line");
    const size_t start = size_t(line_end - data.begin()) + 1;
    Check(data.size() - start >= 13 && data[start+4] <= 1,
          "Unsupported EM1 NIF version/endian");
    version = U32(data, start, false);
    big = data[start+4] == 0;
    user_version = U32(data, start+5, false);
    // Native NiStream also accepts the original legacy NiTriShape container.
    // Its table layout is shared; preserve its version and opaque payloads.
    // Extra user-version layouts have not been audited for this legacy format.
    Check(version == 0x14060500 || (version == 0x14030109 && user_version == 0),
          "Unsupported EM1 NIF version/user version");
    const auto count = U32(data, start+9, false);
    Check(count <= 100000, "Too many EM1 NIF blocks");
    prefix = data.first(start+13);
    Cursor cursor{data, start+13, big};
    const auto type_count = cursor.Short();
    types.reserve(type_count);
    for (unsigned i = 0; i < type_count; ++i) {
      const auto length = cursor.Word();
      Check(length && length <= 4096, "Invalid EM1 NIF block type name");
      const auto value = cursor.Take(length);
      types.emplace_back(reinterpret_cast<const char*>(value.data()), value.size());
    }
    blocks.resize(count);
    for (auto& block : blocks) {
      block.type = cursor.Short();
      Check(block.type < types.size(), "Invalid EM1 NIF block type index");
    }
    std::vector<uint32_t> sizes(count);
    for (auto& size : sizes) size = cursor.Word();
    const size_t strings_at = cursor.at;
    const auto string_count = cursor.Word();
    maximum_string = cursor.Word();
    Check(string_count <= 100000, "Too many EM1 NIF strings");
    for (uint32_t i = 0; i < string_count; ++i) {
      const auto length = cursor.Word();
      Check(length <= maximum_string, "Invalid EM1 NIF string length");
      const auto value = cursor.Take(length);
      string_values.emplace_back(reinterpret_cast<const char*>(value.data()), value.size());
    }
    Check(cursor.Word() == 0, "EM1 NIF object groups require an additional adapter");
    strings = data.subspan(strings_at, cursor.at - strings_at);
    for (uint32_t i = 0; i < count; ++i) blocks[i].original = cursor.Take(sizes[i]);
    const size_t footer_at = cursor.at;
    const auto roots = cursor.Word();
    Check(roots <= count, "Invalid EM1 NIF root count");
    for (uint32_t i = 0; i < roots; ++i) {
      const auto root = cursor.Word();
      Check(root == UINT32_MAX || root < count, "Invalid EM1 NIF root index");
    }
    Check(cursor.at == data.size(), "Unexpected trailing EM1 NIF bytes");
    footer = data.subspan(footer_at);
  }
  std::string_view String(uint32_t index) const {
    Check(index < string_values.size() + added_strings.size(), "Invalid NIF string index");
    return index < string_values.size() ? string_values[index]
        : std::string_view(added_strings[index - string_values.size()]);
  }
  uint32_t AddString(std::string_view value) {
    for (uint32_t i = 0; i < string_values.size() + added_strings.size(); ++i)
      if (String(i) == value) return i;
    Check(string_values.size() + added_strings.size() < 100000, "Too many adapted NIF strings");
    added_strings.emplace_back(value);
    return uint32_t(string_values.size() + added_strings.size() - 1);
  }
  uint16_t Type(std::string_view value) {
    for (uint16_t i = 0; i < types.size(); ++i) if (types[i] == value) return i;
    Check(types.size() < UINT16_MAX, "Too many adapted NIF types");
    types.emplace_back(value); return uint16_t(types.size() - 1);
  }
  uint32_t AddBlock(std::string_view type, Bytes bytes) {
    Check(blocks.size() < 100000, "Too many adapted NIF blocks");
    const auto index = uint32_t(blocks.size());
    blocks.push_back({Type(type), {}, std::move(bytes)}); return index;
  }
  Bytes Write() const {
    Check(blocks.size() <= 100000 && types.size() <= UINT16_MAX, "Adapted NIF table exceeds limit");
    Bytes output;
    Append(output, prefix);
    for (unsigned i = 0; i < 4; ++i)
      output[prefix.size() - 4 + i] = uint8_t(blocks.size() >> (8*i));
    Number(output, uint32_t(types.size()), big, 2);
    for (const auto& type : types) {
      Number(output, uint32_t(type.size()), big);
      Append(output, View(reinterpret_cast<const uint8_t*>(type.data()), type.size()));
    }
    for (const auto& block : blocks) Number(output, block.type, big, 2);
    for (const auto& block : blocks) Number(output, uint32_t(block.Data().size()), big);
    if (added_strings.empty()) Append(output, strings);
    else {
      Number(output, uint32_t(string_values.size() + added_strings.size()), big);
      size_t maximum = maximum_string;
      for (const auto& value : added_strings) maximum = std::max(maximum, value.size());
      Number(output, uint32_t(maximum), big);
      for (uint32_t i = 0; i < string_values.size() + added_strings.size(); ++i) {
        const auto value = String(i);
        Number(output, uint32_t(value.size()), big);
        Append(output, View(reinterpret_cast<const uint8_t*>(value.data()), value.size()));
      }
      Number(output, 0, big);  // no object groups, validated by reader
    }
    for (const auto& block : blocks) Append(output, block.Data());
    Append(output, footer);
    return output;
  }
};
}

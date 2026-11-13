#include "em1_rvz.h"
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#define ZSTD_STATIC_LINKING_ONLY
#include "em1_compat/zstd/lib/zstd.h"
#include <algorithm>
#include <array>
#include <deque>
#include <fstream>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <unordered_map>
extern "C" {
void* em1_disc_open(const uint8_t*,size_t,uint8_t*,size_t);
void em1_disc_close(void*);
size_t em1_disc_count(void*,uint8_t*);
int em1_disc_info(void*,size_t,uint8_t*,size_t,uint64_t*);
int em1_disc_read(void*,size_t,uint64_t,uint8_t*,size_t,uint8_t*,size_t);
}

namespace em2::em1 {
namespace {
using Bytes = std::vector<uint8_t>;
void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
uint32_t U32(std::span<const uint8_t> bytes, size_t at = 0) {
  Check(at <= bytes.size() && bytes.size() - at >= 4, "Truncated RVZ word");
  return (uint32_t(bytes[at]) << 24) | (uint32_t(bytes[at + 1]) << 16) |
         (uint32_t(bytes[at + 2]) << 8) | bytes[at + 3];
}
uint64_t U64(std::span<const uint8_t> bytes, size_t at) {
  return (uint64_t(U32(bytes, at)) << 32) | U32(bytes, at + 4);
}
Bytes Hash(std::span<const uint8_t> data, LPCWSTR algorithm, size_t length) {
  BCRYPT_ALG_HANDLE provider = nullptr;
  Check(BCryptOpenAlgorithmProvider(&provider, algorithm, nullptr, 0) >= 0,
        "Cannot initialize source checksum provider");
  struct Close { BCRYPT_ALG_HANDLE handle; ~Close() { BCryptCloseAlgorithmProvider(handle, 0); } } close{provider};
  Check(data.size() <= ULONG_MAX, "Checksum input exceeds API limit");
  Bytes hash(length);
  Check(BCryptHash(provider, nullptr, 0, const_cast<PUCHAR>(data.data()),
                   ULONG(data.size()), hash.data(), ULONG(hash.size())) >= 0,
        "Source checksum failed");
  return hash;
}
bool MatchesHash(std::span<const uint8_t> data, std::span<const uint8_t> expected) {
  const auto actual = Hash(data, BCRYPT_SHA1_ALGORITHM, 20);
  return expected.size() == actual.size() && std::equal(actual.begin(), actual.end(), expected.begin());
}
std::string Hex(std::span<const uint8_t> data) {
  constexpr char digits[] = "0123456789abcdef";
  std::string text; text.reserve(data.size() * 2);
  for (auto byte : data) { text += digits[byte >> 4]; text += digits[byte & 15]; }
  return text;
}
std::string Key(std::string_view path) {
  Check(!path.empty() && path.front() != '/' && path.front() != '\\', "RVZ path must be relative");
  std::string key;
  for (char c : path) {
    Check(c != ':' && c != '\0', "Invalid RVZ path character");
    if (c == '\\') c = '/';
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    key += c;
  }
  size_t start = 0;
  while (start < key.size()) {
    auto end = key.find('/', start);
    if (end == std::string::npos) end = key.size();
    const auto part = std::string_view(key).substr(start, end - start);
    Check(!part.empty() && part != "." && part != "..", "Invalid RVZ path component");
    start = end + 1;
  }
  Check(key.back() != '/', "RVZ path must name a file");
  return key;
}
Bytes Decompress(std::span<const uint8_t> source, size_t maximum) {
  Check(!source.empty() && maximum <= 32 * 1024 * 1024, "Invalid Zstd size limit");
  ZSTD_frameHeader header{};
  Check(ZSTD_getFrameHeader(&header, source.data(), source.size()) == 0 &&
        header.windowSize <= maximum, "Invalid or oversized Zstd frame window");
  Check(ZSTD_findFrameCompressedSize(source.data(), source.size()) == source.size(),
        "Trailing or invalid Zstd frame data");
  if (header.frameContentSize != ZSTD_CONTENTSIZE_UNKNOWN)
    Check(header.frameContentSize <= maximum, "Zstd output exceeds source limits");
  Bytes output(maximum);
  const auto size = ZSTD_decompress(output.data(), output.size(), source.data(), source.size());
  Check(!ZSTD_isError(size) && size <= maximum, "Zstd decompression failed or exceeded bounds");
  output.resize(size);
  return output;
}
void Padding(std::span<const uint8_t> seed, size_t size, uint64_t offset, Bytes& output) {
  Check(seed.size() == 68, "Invalid RVZ padding seed");
  std::array<uint32_t, 521> state{};
  for (size_t i = 0; i < 17; ++i) state[i] = U32(seed, i * 4);
  for (size_t i = 17; i < state.size(); ++i)
    state[i] = (state[i - 17] << 23) ^ (state[i - 16] >> 9) ^ state[i - 1];
  auto advance = [&] {
    for (size_t i = 0; i < state.size(); ++i) state[i] ^= state[(i + 489) % state.size()];
  };
  for (int i = 0; i < 4; ++i) advance();
  const auto skip = size_t(offset % 0x8000);
  size_t generated = 0, index = 0;
  while (generated < skip + size) {
    if (index == state.size()) { advance(); index = 0; }
    const auto word = state[index++];
    const std::array<uint8_t, 4> bytes{uint8_t(word >> 24), uint8_t(word >> 18),
                                       uint8_t(word >> 8), uint8_t(word)};
    for (auto byte : bytes) {
      if (generated >= skip && generated < skip + size) output.push_back(byte);
      ++generated;
    }
  }
}
Bytes Unpack(std::span<const uint8_t> data, size_t expected, uint64_t offset) {
  Bytes output; output.reserve(expected);
  size_t position = 0;
  while (position < data.size()) {
    const auto word = U32(data, position); position += 4;
    const auto count = size_t(word & 0x7fffffff);
    Check(count <= expected - output.size(), "RVZ packed run exceeds group");
    const auto consumed = word >> 31 ? 68 : count;
    Check(consumed <= data.size() - position, "Truncated RVZ packed run");
    if (word >> 31) Padding(data.subspan(position, consumed), count, offset + output.size(), output);
    else output.insert(output.end(), data.begin() + position, data.begin() + position + consumed);
    position += consumed;
  }
  Check(output.size() == expected, "RVZ unpacked length mismatch");
  return output;
}
}  // namespace

struct RvzSource::Impl {
  struct Segment { uint32_t first, sectors, group, count; };
  struct Partition { std::array<Segment, 2> segments; uint64_t size = 0; };
  struct Group { uint32_t offset, size, packed; };
  struct Range { uint64_t offset, size; };
  struct Cache { uint32_t group; size_t expected; uint32_t alignment; Bytes bytes; };
  std::filesystem::path image_path;
  std::filesystem::file_time_type modified;
  std::ifstream file;
  uint64_t size = 0;
  uint32_t chunk = 0, compression = 5;
  Bytes Decode(std::span<const uint8_t> source,size_t maximum){
    if(!compression){Check(source.size()<=maximum,"Uncompressed RVZ block exceeds its bounds");return Bytes(source.begin(),source.end());}
    return Decompress(source,maximum);
  }
  std::string disc_id, identity;
  std::unique_ptr<void,void(*)(void*)> generic{nullptr,em1_disc_close};
  std::vector<Partition> partitions;
  std::vector<Group> groups;
  size_t game_partition = 0;
  std::unordered_map<std::string, Range> ranges;
  std::vector<RvzFile> files;
  std::deque<Cache> cache;
  std::mutex mutex;

  Bytes ReadAt(uint64_t offset, uint64_t length) {
    Check(offset <= size && length <= size - offset && length <= 32 * 1024 * 1024,
          "Read outside RVZ source or bounded metadata/group limit");
    Bytes result(size_t(length), uint8_t(0));
    file.clear(); file.seekg(std::streamoff(offset));
    Check(bool(file), "Cannot seek RVZ source");
    file.read(reinterpret_cast<char*>(result.data()), std::streamsize(length));
    Check(uint64_t(file.gcount()) == length, "Short RVZ source read");
    return result;
  }
  const Bytes& ReadGroup(uint32_t index, size_t expected, uint64_t offset) {
    Check(index < groups.size(), "Invalid RVZ group reference");
    const auto alignment = uint32_t(offset % 0x8000);
    auto found = std::find_if(cache.begin(), cache.end(), [&](const Cache& item) {
      return item.group == index && item.expected == expected && item.alignment == alignment;
    });
    if (found != cache.end()) {
      auto hit = std::move(*found); cache.erase(found); cache.push_front(std::move(hit));
      return cache.front().bytes;
    }
    const auto& entry = groups[index];
    const auto stored = entry.size & 0x7fffffff;
    Bytes bytes;
    if (!stored) bytes.resize(expected);
    else {
      Check(stored <= chunk + 131072, "Oversized RVZ compressed group");
      bytes = ReadAt(uint64_t(entry.offset) * 4, stored);
      const bool compressed = bool(entry.size >> 31);
      if (compressed) bytes = Decode(bytes, chunk + 131072);
      Check(bytes.size() >= 2, "Missing Wii hash exception list");
      const auto exceptions = (uint32_t(bytes[0]) << 8) | bytes[1];
      Check(exceptions <= 3328, "Invalid Wii hash exception count");
      size_t prefix = 2 + size_t(exceptions) * 22;
      if (!compressed) prefix = (prefix + 3) & ~size_t(3);
      Check(prefix <= bytes.size(), "Truncated Wii hash exception list");
      std::span payload(bytes.data() + prefix, bytes.size() - prefix);
      if (entry.packed) {
        Check(payload.size() == entry.packed, "RVZ packed length mismatch");
        bytes = Unpack(payload, expected, offset);
      } else {
        Check(payload.size() == expected, "Decrypted RVZ group length mismatch");
        Bytes stripped(payload.begin(), payload.end()); bytes = std::move(stripped);
      }
    }
    if (cache.size() >= 8) cache.pop_back();
    cache.push_front({index, expected, alignment, std::move(bytes)});
    return cache.front().bytes;
  }
  Bytes ReadPartition(size_t partition, uint64_t offset, uint64_t length) {
    Check(partition < partitions.size() && length <= 256 * 1024 * 1024,
          "Invalid Wii partition request");
    const auto& part = partitions[partition];
    Check(offset <= part.size && length <= part.size - offset, "Read outside Wii partition");
    Bytes output; output.reserve(size_t(length));
    const auto stride = uint64_t(chunk / 0x8000) * 0x7c00;
    while (length) {
      const Segment* match = nullptr; uint64_t start = 0, segment_size = 0;
      for (const auto& segment : part.segments) {
        if (!segment.sectors) continue;
        const auto base = uint64_t(segment.first - part.segments[0].first) * 0x7c00;
        const auto extent = uint64_t(segment.sectors) * 0x7c00;
        if (offset >= base && offset - base < extent) { match = &segment; start = base; segment_size = extent; break; }
      }
      Check(match != nullptr, "Wii partition request crosses an absent segment");
      const auto group = (offset - start) / stride;
      const auto inside = (offset - start) % stride;
      Check(group < match->count, "Wii partition group outside segment");
      const auto expected = size_t(std::min(stride, segment_size - group * stride));
      const auto& data = ReadGroup(match->group + uint32_t(group), expected, start + group * stride);
      Check(inside < data.size(), "Invalid Wii group offset");
      const auto count = size_t(std::min(length, uint64_t(data.size()) - inside));
      output.insert(output.end(), data.begin() + inside, data.begin() + inside + count);
      offset += count; length -= count;
    }
    return output;
  }
  std::unordered_map<std::string, Range> ReadFst(size_t partition) {
    auto boot = ReadPartition(partition, 0, 0x440);
    const auto at = uint64_t(U32(boot, 0x424)) * 4;
    const auto length = uint64_t(U32(boot, 0x428)) * 4;
    Check(length >= 12 && length <= 32 * 1024 * 1024, "Invalid Wii FST size");
    const auto fst = ReadPartition(partition, at, length);
    const auto count = U32(fst, 8);
    Check(count >= 1 && count <= 100000 && count <= fst.size() / 12 && U32(fst) >> 24 == 1 && U32(fst, 4) == 0,
          "Invalid Wii FST root");
    const size_t strings = size_t(count) * 12;
    struct Directory { uint32_t end, index; std::string path; };
    std::vector<Directory> stack{{count, 0, ""}};
    std::unordered_map<std::string, Range> result;
    for (uint32_t i = 1; i < count; ++i) {
      while (!stack.empty() && i >= stack.back().end) stack.pop_back();
      Check(!stack.empty(), "Invalid FST directory range");
      const auto word = U32(fst, size_t(i) * 12);
      const auto name_at = strings + (word & 0xffffff);
      Check(name_at < fst.size(), "Invalid FST name offset");
      const auto end = std::find(fst.begin() + name_at, fst.end(), uint8_t(0));
      Check(end != fst.end(), "Unterminated FST name");
      std::string name(reinterpret_cast<const char*>(fst.data() + name_at), size_t(end - fst.begin() - name_at));
      Check(!name.empty() && name.size() <= 255 && name.find_first_of("/\\:") == std::string::npos && name != "." && name != "..",
            "Invalid FST path component");
      const auto full = stack.back().path + name;
      Check(full.size() <= 2048 && stack.size() <= 256, "FST path/depth limit exceeded");
      if (word >> 24) {
        const auto stop = U32(fst, size_t(i) * 12 + 8);
        Check(word >> 24 == 1 && stop > i && stop <= stack.back().end &&
              U32(fst, size_t(i) * 12 + 4) == stack.back().index, "Invalid FST child directory");
        stack.push_back({stop, i, full + "/"});
      } else {
        const auto offset = uint64_t(U32(fst, size_t(i) * 12 + 4)) * 4;
        const auto bytes = uint64_t(U32(fst, size_t(i) * 12 + 8));
        Check(offset <= partitions[partition].size && bytes <= partitions[partition].size - offset,
              "FST file extends outside partition");
        Check(result.emplace(Key(full), Range{offset, bytes}).second, "Duplicate origin-qualified FST path");
      }
    }
    return result;
  }
  explicit Impl(const std::filesystem::path& image,bool other_format=false)
      : image_path(image), modified(std::filesystem::last_write_time(image)), file(image, std::ios::binary) {
    Check(bool(file), "Cannot open original RVZ read-only");
    size = std::filesystem::file_size(image);
    if(other_format){
      const auto path=image.u8string();std::array<uint8_t,1024> error{};
      generic.reset(em1_disc_open(reinterpret_cast<const uint8_t*>(path.data()),path.size(),error.data(),error.size()));
      Check(bool(generic),reinterpret_cast<const char*>(error.data()));std::array<uint8_t,6> id{};
      const auto count=em1_disc_count(generic.get(),id.data());Check(count>0 && count<=100000,"Invalid Wii disc file count");
      disc_id.assign(id.begin(),id.end());
      for(size_t i=0;i<count;++i){std::array<uint8_t,2049> name{};uint64_t bytes=0;
        Check(em1_disc_info(generic.get(),i,name.data(),name.size(),&bytes)==0,"Could not read Wii disc directory");
        const auto key=Key(reinterpret_cast<const char*>(name.data()));Check(ranges.emplace(key,Range{i,bytes}).second,"Duplicate Wii disc file");files.push_back({key,bytes});
      }
      auto fingerprint=ReadAt(0,std::min<uint64_t>(size,4096));const auto canonical=std::filesystem::weakly_canonical(image).generic_u8string();
      fingerprint.insert(fingerprint.end(),canonical.begin(),canonical.end());const auto stamp=std::to_string(modified.time_since_epoch().count());fingerprint.insert(fingerprint.end(),stamp.begin(),stamp.end());
      identity="em1-disc-v1:"+Hex(Hash(fingerprint,BCRYPT_SHA256_ALGORITHM,32));return;
    }
    const auto header = ReadAt(0, 0x48);
    Check(std::equal(header.begin(), header.begin() + 4, std::array<uint8_t, 4>{'R','V','Z',1}.begin()) &&
          MatchesHash(std::span(header).first(0x34), std::span(header).subspan(0x34, 20)), "Invalid RVZ primary header");
    const auto second_size = U32(header, 12);
    Check(U32(header, 8) <= 0x01000000 && second_size >= 220 && second_size <= 1024 * 1024 && U64(header, 44) == size,
          "Unsupported RVZ version or length");
    const auto iso_size = U64(header, 36);
    Check(iso_size >= 0x8000 && iso_size <= 16ull * 1024 * 1024 * 1024, "Invalid Wii logical disc size");
    const auto second = ReadAt(0x48, second_size);
    Check(MatchesHash(second, std::span(header).subspan(16, 20)), "Invalid RVZ disc-header checksum");
    compression=U32(second,4);
    Check(U32(second) == 2 && (compression==0 || compression==5), "This Wii RVZ compression is unsupported. Use an RVZ with Zstd or no compression.");
    chunk = U32(second, 12);
    Check(chunk >= 0x8000 && chunk <= 0x200000 && !(chunk & (chunk - 1)), "Unsupported RVZ chunk size");
    disc_id.assign(reinterpret_cast<const char*>(second.data() + 16), 6);
    const auto partition_count = U32(second, 144);
    Check(partition_count > 0 && partition_count <= 64 && U32(second, 148) == 48, "Invalid RVZ partition table");
    const auto table = ReadAt(U64(second, 152), uint64_t(partition_count) * 48);
    Check(MatchesHash(table, std::span(second).subspan(160, 20)), "Invalid RVZ partition-table checksum");
    const auto group_count = U32(second, 196), compressed = U32(second, 208);
    Check(group_count > 0 && group_count <= 1000000 && compressed <= 16 * 1024 * 1024,
          "Invalid RVZ group-table size");
    const auto packed_groups = ReadAt(U64(second, 200), compressed);
    const auto raw_groups = Decode(packed_groups, size_t(group_count) * 12);
    Check(raw_groups.size() == size_t(group_count) * 12, "RVZ group-table length mismatch");
    groups.reserve(group_count);
    for (size_t i = 0; i < group_count; ++i) {
      Group group{U32(raw_groups, i * 12), U32(raw_groups, i * 12 + 4), U32(raw_groups, i * 12 + 8)};
      const auto bytes = uint64_t(group.size & 0x7fffffff), offset = uint64_t(group.offset) * 4;
      Check(!bytes || (offset <= size && bytes <= size - offset), "RVZ group outside source file");
      groups.push_back(group);
    }
    for (size_t i = 0; i < partition_count; ++i) {
      Partition part{};
      for (size_t j = 0; j < 2; ++j) {
        const auto at = i * 48 + 16 + j * 16;
        auto& segment = part.segments[j];
        segment = {U32(table, at), U32(table, at + 4), U32(table, at + 8), U32(table, at + 12)};
        Check(uint64_t(segment.group) + segment.count <= group_count &&
              segment.count == (uint64_t(segment.sectors) * 0x8000 + chunk - 1) / chunk,
              "Invalid RVZ partition group span");
        if (segment.sectors) {
          Check((uint64_t(segment.first) + segment.sectors) * 0x8000 <= iso_size, "RVZ partition outside logical disc");
          Check(segment.first >= part.segments[0].first, "Invalid RVZ partition sector order");
          const auto start = uint64_t(segment.first - part.segments[0].first) * 0x7c00;
          if (j) Check(start >= uint64_t(part.segments[0].sectors) * 0x7c00, "Overlapping RVZ partition segments");
          part.size = std::max(part.size, start + uint64_t(segment.sectors) * 0x7c00);
        }
      }
      partitions.push_back(part);
    }
    size_t matches = 0;
    for (size_t i = 0; i < partitions.size(); ++i) {
      auto candidate = ReadFst(i);
      if (candidate.contains("packfiles/levels.pak")) {
        ++matches; game_partition = i; ranges = std::move(candidate);
      }
    }
    Check(matches == 1, "RVZ must contain exactly one Epic Mickey game partition");
    for (const auto& [path, range] : ranges) files.push_back({path, range.size});
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    // Header fingerprint plus canonical source and modification identity prevents
    // one mount's cache from serving another image. Asset cache adds content hash.
    auto identity_bytes = header;
    const auto canonical = std::filesystem::weakly_canonical(image).generic_u8string();
    identity_bytes.insert(identity_bytes.end(), canonical.begin(), canonical.end());
    Check(std::filesystem::file_size(image) == size && std::filesystem::last_write_time(image) == modified,
          "Original RVZ changed while opening; remount required");
    const auto stamp = modified.time_since_epoch().count();
    const auto stamp_text = std::to_string(stamp);
    identity_bytes.insert(identity_bytes.end(), stamp_text.begin(), stamp_text.end());
    identity = "em1-rvz-v1:" + Hex(Hash(identity_bytes, BCRYPT_SHA256_ALGORITHM, 32));
  }
};

RvzSource::RvzSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
RvzSource::~RvzSource() = default;
std::unique_ptr<RvzSource> RvzSource::Open(const std::filesystem::path& image, std::string& error) {
  try {std::unique_ptr<Impl> impl;try{impl=std::make_unique<Impl>(image);}catch(const std::exception&){impl=std::make_unique<Impl>(image,true);}
    auto result = std::unique_ptr<RvzSource>(new RvzSource(std::move(impl))); error.clear(); return result; }
  catch (const std::exception& exception) { error = exception.what(); return {}; }
}
const std::string& RvzSource::DiscId() const { return impl_->disc_id; }
bool RvzSource::TestGenericSource(const std::filesystem::path& image,std::string& error){
  try{auto original=Open(image,error);Check(bool(original),error.c_str());
    RvzSource generic(std::make_unique<Impl>(image,true));Check(original->DiscId()==generic.DiscId(),"Disc IDs differ between source adapters");
    auto expected=original->Files(),actual=generic.Files();auto less=[](const auto& a,const auto& b){return a.path<b.path;};std::sort(expected.begin(),expected.end(),less);std::sort(actual.begin(),actual.end(),less);
    Check(expected.size()==actual.size(),"Disc file counts differ between adapters");for(size_t i=0;i<expected.size();++i)Check(expected[i].path==actual[i].path && expected[i].size==actual[i].size,"Disc filesystem differs between adapters");
    for(size_t sample=0;sample<17 && !expected.empty();++sample){const auto& f=expected[sample*(expected.size()-1)/16];const auto n=std::min<uint64_t>(128,f.size);
      for(const auto offset:{uint64_t(0),f.size-n}){Bytes a,b;Check(original->ReadFileRange(f.path,offset,n,a,error),error.c_str());Check(generic.ReadFileRange(f.path,offset,n,b,error),error.c_str());Check(a==b,"Disc reader returned different file bytes");}}
    error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
const std::string& RvzSource::SourceIdentity() const { return impl_->identity; }
const std::vector<RvzFile>& RvzSource::Files() const { return impl_->files; }
bool RvzSource::ReadFile(std::string_view path, std::vector<uint8_t>& output, std::string& error) {
  try {
    const auto found = impl_->ranges.find(Key(path));
    Check(found != impl_->ranges.end(), "File absent from EM1 source mount");
    return ReadFileRange(path, 0, found->second.size, output, error);
  } catch (const std::exception& exception) { error = exception.what(); return false; }
}
bool RvzSource::ReadFileRange(std::string_view path, uint64_t offset, uint64_t length,
                             std::vector<uint8_t>& output, std::string& error) {
  try {
    std::lock_guard lock(impl_->mutex);
    Check(std::filesystem::file_size(impl_->image_path) == impl_->size &&
          std::filesystem::last_write_time(impl_->image_path) == impl_->modified,
          "Original RVZ changed; remount required before cached reads");
    const auto found = impl_->ranges.find(Key(path));
    Check(found != impl_->ranges.end(), "File absent from EM1 source mount");
    Check(offset <= found->second.size && length <= found->second.size - offset, "Read outside EM1 source file");
    if(impl_->generic){Check(length<=256*1024*1024,"Oversized EM1 disc read");Bytes bytes(size_t(length),uint8_t(0));std::array<uint8_t,1024> message{};
      Check(em1_disc_read(impl_->generic.get(),size_t(found->second.offset),offset,bytes.data(),bytes.size(),message.data(),message.size())==0,reinterpret_cast<const char*>(message.data()));
      output=std::move(bytes);error.clear();return true;}
    auto data = impl_->ReadPartition(impl_->game_partition, found->second.offset + offset, length);
    output = std::move(data); error.clear(); return true;
  } catch (const std::exception& exception) { error = exception.what(); return false; }
}
}  // namespace em2::em1

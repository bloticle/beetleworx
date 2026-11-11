#include "em1_pack_source.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace em2::em1 {
namespace {
constexpr uint64_t kMaximumResource = 256ull * 1024 * 1024;
constexpr uint64_t kMaximumDirectory = 16ull * 1024 * 1024;
void Require(bool condition, const char* error) {
  if (!condition) throw std::runtime_error(error);
}
std::string Canonical(std::string_view path) {
  Require(!path.empty() && path.size() <= 2048 && path.front() != '/' &&
              path.front() != '\\', "PAK path must be a bounded relative path");
  std::string result;
  result.reserve(path.size());
  for (unsigned char c : path) {
    Require(c >= 32 && c != 127 && c != ':', "Invalid PAK path character");
    if (c == '\\') c = '/';
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    result += char(c);
  }
  for (size_t start = 0; start < result.size();) {
    const auto end = result.find('/', start);
    const auto part = std::string_view(result).substr(start,
        end == std::string::npos ? result.size() - start : end - start);
    Require(!part.empty() && part != "." && part != "..",
            "Invalid PAK path component");
    start = end == std::string::npos ? result.size() : end + 1;
  }
  Require(result.back() != '/', "PAK path must name a resource");
  return result;
}
uint32_t Word(std::span<const uint8_t> data, size_t at, bool big) {
  Require(at <= data.size() && data.size() - at >= 4, "Truncated PAK word");
  if (big) return uint32_t(data[at]) << 24 | uint32_t(data[at + 1]) << 16 |
                  uint32_t(data[at + 2]) << 8 | data[at + 3];
  return uint32_t(data[at + 3]) << 24 | uint32_t(data[at + 2]) << 16 |
         uint32_t(data[at + 1]) << 8 | data[at];
}
}  // namespace

struct PakSource::Impl {
  std::shared_ptr<RvzSource> source;
  std::string archive, origin;
  std::vector<PakEntry> entries;
  std::vector<uint64_t> offsets;
  std::unordered_map<std::string, size_t> lookup;
  std::vector<uint8_t> Read(uint64_t at, uint64_t length) {
    std::vector<uint8_t> result;
    std::string error;
    if (!source->ReadFileRange(archive, at, length, result, error))
      throw std::runtime_error(error);
    return result;
  }
};
PakSource::PakSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PakSource::~PakSource() = default;
const std::string& PakSource::OriginKey() const { return impl_->origin; }
const std::vector<PakEntry>& PakSource::Entries() const { return impl_->entries; }

std::unique_ptr<PakSource> PakSource::Open(std::shared_ptr<RvzSource> source,
                                        std::string_view archive,
                                        std::string& error) {
  try {
    Require(source && source->DiscId().starts_with("SEM"),
            "PAK origin must be an original Epic Mickey Wii disc");
    auto impl = std::make_unique<Impl>();
    impl->source = std::move(source);
    impl->archive = Canonical(archive);
    Require(impl->archive.starts_with("packfiles/") &&
                impl->archive.ends_with(".pak"), "Expected an EM1 packfile path");
    const auto& files = impl->source->Files();
    const auto file = std::find_if(files.begin(), files.end(),
        [&](const RvzFile& entry) { return entry.path == impl->archive; });
    Require(file != files.end() && file->size >= 32, "EM1 archive is absent or truncated");
    const auto header = impl->Read(0, 32);
    const bool big = std::equal(header.begin(), header.begin() + 4, " KAP");
    Require(big || std::equal(header.begin(), header.begin() + 4, "PAK "),
            "Invalid EM1 PAK signature");
    Require(Word(header, 4, big) == 2 && Word(header, 8, big) == 0,
            "Unsupported EM1 PAK version");
    const uint64_t directory_at = Word(header, 12, big);
    const uint64_t directory_size = Word(header, 16, big);
    Require(directory_at >= 32 && directory_at <= file->size &&
                directory_size >= 4 && directory_size <= kMaximumDirectory &&
                directory_size <= file->size - directory_at,
            "Invalid EM1 PAK directory bounds");
    const auto directory = impl->Read(directory_at, directory_size);
    const uint32_t count = Word(directory, 0, big);
    Require(count <= 100000 && uint64_t(count) * 24 + 4 <= directory.size(),
            "Invalid EM1 PAK entry count");
    const size_t strings = 4 + size_t(count) * 24;
    auto string_at = [&](uint32_t offset) {
      Require(offset < directory.size() - strings, "Invalid EM1 PAK string offset");
      const size_t start = strings + offset;
      const auto last = std::min(directory.size(), start + 2049);
      const auto end = std::find(directory.begin() + start, directory.begin() + last, 0);
      Require(end != directory.begin() + last, "Unterminated or oversized EM1 PAK string");
      return std::string(directory.begin() + start, end);
    };
    uint64_t payload = directory_at + directory_size;
    impl->entries.reserve(count);
    impl->offsets.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      const size_t at = 4 + size_t(i) * 24;
      PakEntry entry;
      entry.size = Word(directory, at, big);
      entry.stored_size = Word(directory, at + 4, big);
      const uint32_t aligned = Word(directory, at + 8, big);
      Require(entry.size <= kMaximumResource && entry.stored_size <= kMaximumResource &&
                  aligned % 32 == 0 && entry.stored_size <= aligned &&
                  payload <= file->size && aligned <= file->size - payload,
              "Invalid or oversized EM1 PAK resource bounds");
      const auto folder = string_at(Word(directory, at + 12, big));
      const auto name = string_at(Word(directory, at + 20, big));
      Require(!name.empty() && name.find_first_of("/\\") == std::string::npos,
              "Invalid EM1 PAK resource filename");
      entry.path = Canonical(folder.empty() ? name : folder + '/' + name);
      std::copy_n(directory.begin() + at + 16, 4, entry.type.begin());
      Require(impl->lookup.emplace(entry.path, impl->entries.size()).second,
              "Duplicate EM1 PAK resource path");
      impl->entries.push_back(std::move(entry));
      impl->offsets.push_back(payload);
      payload += aligned;
    }
    impl->origin = impl->source->SourceIdentity() + ":pak-v1:" +
                   std::to_string(impl->archive.size()) + ':' + impl->archive;
    error.clear();
    return std::unique_ptr<PakSource>(new PakSource(std::move(impl)));
  } catch (const std::exception& exception) {
    error = exception.what();
    return nullptr;
  }
}

bool PakSource::ReadPacked(std::string_view path, PackedResource& output,
                           std::string& error) {
  try {
    const auto found = impl_->lookup.find(Canonical(path));
    Require(found != impl_->lookup.end(), "EM1 resource is absent from this archive");
    PackedResource result;
    result.entry = impl_->entries[found->second];
    result.bytes = impl_->Read(impl_->offsets[found->second], result.entry.stored_size);
    output = std::move(result);
    error.clear();
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

bool PakSource::ReadOriginal(std::string_view path, std::vector<uint8_t>& output,
                             std::string& error, const PakInflater& inflate) {
  try {
    PackedResource resource;
    if (!ReadPacked(path, resource, error)) return false;
    if (resource.IsCompressed()) {
      Require(bool(inflate), "This EM1 resource requires a native Zlib inflater");
      std::vector<uint8_t> original(resource.entry.size);
      if (!inflate(resource.bytes, original, error)) {
        if (error.empty()) error = "EM1 resource inflation failed";
        return false;
      }
      output = std::move(original);
    } else {
      output = std::move(resource.bytes);
    }
    error.clear();
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}
}  // namespace em2::em1

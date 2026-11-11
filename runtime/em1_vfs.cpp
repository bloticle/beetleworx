#include "em1_level_source.h"
#include <atomic>
#include "em1_vfs.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <rex/filesystem/vfs.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace em2::em1 {
bool CopyRvzBytes(void* destination, const void* source, size_t length);
namespace {
using namespace rex::filesystem;
using rex::X_STATUS;
constexpr std::string_view kDevice = "\\Device\\Em1Disc";
constexpr uint32_t kWriteAccess = FileAccess::kGenericWrite | FileAccess::kGenericAll |
    FileAccess::kFileWriteData | FileAccess::kFileAppendData | 0x10 | 0x100 |
    0x10000 | 0x40000 | 0x80000;
std::string Relative(std::string_view path) {
  if (path.size() > 2048) throw std::runtime_error("Oversized EM1 VFS path");
  // Device-local paths are supplied after the VFS consumes the mount prefix.
  while (!path.empty() && (path.front() == '/' || path.front() == '\\')) path.remove_prefix(1);
  while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.remove_suffix(1);
  std::string result;
  for (unsigned char c : path) {
    if (c < 32 || c == 127 || c == ':') throw std::runtime_error("Invalid EM1 VFS path");
    if (c == '\\') c = '/';
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    result += char(c);
  }
  for (size_t at = 0; at < result.size();) {
    const auto end = result.find('/', at);
    const auto part = std::string_view(result).substr(at,
        end == std::string::npos ? result.size() - at : end - at);
    if (part.empty() || part == "." || part == "..") throw std::runtime_error("Invalid EM1 VFS component");
    at = end == std::string::npos ? result.size() : end + 1;
  }
  return result;
}
class RvzFileHandle final : public File {
 public:
  RvzFileHandle(uint32_t access, Entry* entry, std::shared_ptr<RvzSource> source,
                std::string path, uint64_t size, bool directory)
      : File(access, entry), source_(std::move(source)), path_(std::move(path)),
        size_(size), directory_(directory) {}
  void Destroy() override { delete this; }
  X_STATUS ReadSync(void* output, size_t length, size_t offset, size_t* read) override {
    if (!read) return X_STATUS_INVALID_PARAMETER;
    *read = 0;
    if (directory_) return X_STATUS_FILE_IS_A_DIRECTORY;
    if (!(file_access_ & (FileAccess::kGenericRead | FileAccess::kFileReadData)))
      return X_STATUS_ACCESS_DENIED;
    if (!length) return X_STATUS_SUCCESS;
    if (!output) return X_STATUS_INVALID_PARAMETER;
    if (offset >= size_) return X_STATUS_END_OF_FILE;
    const uint64_t wanted = std::min<uint64_t>(length, size_ - offset);
    try {
      while (*read < wanted) {
        const uint64_t chunk = std::min<uint64_t>(wanted - *read, 8 * 1024 * 1024);
        std::vector<uint8_t> bytes;
        std::string error;
        if (!source_->ReadFileRange(path_, uint64_t(offset) + *read, chunk, bytes, error))
          return X_STATUS_UNSUCCESSFUL;
        if (bytes.size() != chunk || *read > UINTPTR_MAX - reinterpret_cast<uintptr_t>(output))
          return X_STATUS_INVALID_PARAMETER;
        if (!CopyRvzBytes(static_cast<uint8_t*>(output) + *read, bytes.data(), bytes.size()))
          return X_STATUS_ACCESS_VIOLATION;
        *read += bytes.size();
      }
      return X_STATUS_SUCCESS;
    } catch (const std::bad_alloc&) { return X_STATUS_NO_MEMORY; }
    catch (...) { return X_STATUS_UNSUCCESSFUL; }
  }
  X_STATUS WriteSync(const void*, size_t, size_t, size_t* written) override {
    if (written) *written = 0;
    return X_STATUS_ACCESS_DENIED;
  }
  X_STATUS SetLength(size_t) override { return X_STATUS_ACCESS_DENIED; }
 private:
  std::shared_ptr<RvzSource> source_;
  std::string path_;
  uint64_t size_;
  bool directory_;
};
class RvzEntry final : public Entry {
 public:
  RvzEntry(Device* device, Entry* parent, std::string_view path,
            std::shared_ptr<RvzSource> source, uint64_t size, bool directory)
      : Entry(device, parent, path), source_(std::move(source)), source_path_(Relative(path)),
        directory_(directory) {
    attributes_ = kFileAttributeReadOnly | (directory ? kFileAttributeDirectory : kFileAttributeNormal);
    size_ = size_t(size);
    allocation_size_ = (size_ + 511) & ~size_t(511);
    create_timestamp_ = access_timestamp_ = write_timestamp_ = 0;
  }
  RvzEntry* Add(std::unique_ptr<RvzEntry> child) {
    auto* result = child.get(); children_.push_back(std::move(child)); return result;
  }
  bool Directory() const { return directory_; }
  X_STATUS Open(uint32_t access, File** output) override {
    if (!output) return X_STATUS_INVALID_PARAMETER;
    *output = nullptr;
    if (access & kWriteAccess) return X_STATUS_ACCESS_DENIED;
    try {
      *output = new RvzFileHandle(access, this, source_, source_path_, size_, directory_);
      return X_STATUS_SUCCESS;
    } catch (const std::bad_alloc&) { return X_STATUS_NO_MEMORY; }
  }
 private:
  std::shared_ptr<RvzSource> source_;
  std::string source_path_;
  bool directory_;
};
class RvzDevice final : public Device {
 public:
  explicit RvzDevice(std::shared_ptr<RvzSource> source)
      : Device(kDevice), source_(std::move(source)) {}
  std::shared_ptr<RvzSource> Source() const { return source_; }
  bool Initialize() override {
    root_ = std::make_unique<RvzEntry>(this, nullptr, "", source_, 0, true);
    entries_.emplace("", root_.get());
    for (const auto& file : source_->Files()) {
      const auto path = Relative(file.path);
      RvzEntry* parent = root_.get();
      for (size_t start = 0; start < path.size();) {
        const auto slash = path.find('/', start);
        const bool directory = slash != std::string::npos;
        const auto prefix = path.substr(0, directory ? slash : path.size());
        const auto found = entries_.find(prefix);
        if (found != entries_.end()) {
          if (!directory || !found->second->Directory())
            throw std::runtime_error("Conflicting EM1 VFS entries");
          parent = found->second;
        } else {
          if (entries_.size() >= 100000) throw std::runtime_error("Too many EM1 VFS entries");
          std::string guest_path = prefix;
          std::replace(guest_path.begin(), guest_path.end(), '/', '\\');
          parent = parent->Add(std::make_unique<RvzEntry>(this, parent, guest_path,
              source_, directory ? 0 : file.size, directory));
          entries_.emplace(prefix, parent);
        }
        start = directory ? slash + 1 : path.size();
      }
    }
    return true;
  }
  void Dump(rex::string::StringBuffer* buffer) override { if (root_) root_->Dump(buffer, 0); }
  Entry* ResolvePath(std::string_view path) override {
    try {
      const auto entry = entries_.find(Relative(path));
      return entry == entries_.end() ? nullptr : entry->second;
    } catch (...) { return nullptr; }
  }
  const std::string& name() const override { return name_; }
  uint32_t attributes() const override { return kFileAttributeReadOnly; }
  uint32_t component_name_max_length() const override { return 255; }
  uint32_t total_allocation_units() const override { return 16 * 1024 * 1024; }
  uint32_t available_allocation_units() const override { return 0; }
  uint32_t sectors_per_allocation_unit() const override { return 1; }
  uint32_t bytes_per_sector() const override { return 512; }
 private:
  std::shared_ptr<RvzSource> source_;
  std::unique_ptr<RvzEntry> root_;
  std::unordered_map<std::string, RvzEntry*> entries_;
  std::string name_ = "Epic Mickey original RVZ assets";
};
}  // namespace

static std::atomic<std::shared_ptr<const LevelCatalog>> level_catalog;
std::shared_ptr<const LevelCatalog> CurrentLevelCatalog(){return level_catalog.load(std::memory_order_acquire);}

bool MountRvz(rex::filesystem::VirtualFileSystem& filesystem,
               std::shared_ptr<RvzSource> source, std::string& error) {
  try {
    if (!source || !source->DiscId().starts_with("SEM"))
      throw std::runtime_error("EM1 mount requires an original Epic Mickey Wii source");
    std::string existing;
    if (filesystem.FindSymbolicLink("em1:", existing) || filesystem.ResolvePath(kDevice))
      throw std::runtime_error("An EM1 device is already mounted; existing handles must close before remounting");
    auto catalog=std::make_shared<LevelCatalog>();
    catalog->source_identity=source->SourceIdentity();
    ListLevelDescriptors(source,catalog->descriptors,catalog->error);
    if(!catalog->error.empty() || catalog->descriptors.empty())
      throw std::runtime_error(catalog->error.empty()?"The EM1 disc contains no level descriptors":catalog->error);
    auto device = std::make_unique<RvzDevice>(std::move(source));
    if (!device->Initialize() || !filesystem.RegisterDevice(std::move(device)))
      throw std::runtime_error("Cannot register the read-only EM1 source device");
    if (!filesystem.RegisterSymbolicLink("em1:", kDevice)) {
      filesystem.UnregisterDevice(kDevice);
      throw std::runtime_error("Cannot register the EM1 source namespace");
    }
    level_catalog.store(std::move(catalog),std::memory_order_release);
    error.clear();
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}
std::shared_ptr<RvzSource> MountedRvz(rex::filesystem::VirtualFileSystem& filesystem) {
  std::string target;
  if (!filesystem.FindSymbolicLink("em1:", target) || target != kDevice) return {};
  auto* entry = filesystem.ResolvePath("em1:");
  auto* device = entry ? dynamic_cast<RvzDevice*>(entry->device()) : nullptr;
  return device ? device->Source() : nullptr;
}
}  // namespace em2::em1

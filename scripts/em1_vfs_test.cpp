#include "em1_vfs.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <rex/filesystem/vfs.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}
struct FileCloser {
  void operator()(rex::filesystem::File* file) const { if (file) file->Destroy(); }
};
}
int wmain(int argc, wchar_t** argv) {
  try {
    using namespace em2::em1;
    using namespace rex::filesystem;
    using rex::X_STATUS;
    Check(argc == 3, "Usage: em1_vfs_test original.rvz existing-fixture-directory");
    std::ofstream trace(std::filesystem::path(argv[2]) / "vfs-test-progress.log");
    auto step = [&](const char* text) { trace << text << std::endl; };
    rex::InitLogging(nullptr, spdlog::level::err);
    struct ShutdownLog { ~ShutdownLog() { rex::ShutdownLogging(); } } shutdown_log;
    std::string error;
    const auto before = std::filesystem::last_write_time(argv[1]);
    std::shared_ptr<RvzSource> source = RvzSource::Open(argv[1], error);
    Check(bool(source), error);
    VirtualFileSystem vfs;
    auto game = std::make_unique<HostPathDevice>("\\Device\\OriginalGame", argv[2], true);
    Check(game->Initialize() && vfs.RegisterDevice(std::move(game)), "Fixture device initialization failed");
    Check(vfs.RegisterSymbolicLink("game:", "\\Device\\OriginalGame"), "Fixture alias failed");
    auto* original_game = vfs.ResolvePath("game:\\");
    Check(original_game != nullptr, "Fixture root missing");
    Check(MountRvz(vfs, source, error), error);
    step("mounted");
    Check(vfs.ResolvePath("game:\\") == original_game, "EM1 mount changed game:");
    auto* root = vfs.ResolvePath("em1:\\");
    Check(root && root->GetChild("packfiles") && root->device()->is_read_only(), "Missing EM1 directory tree");
    Check(!vfs.ResolvePath("em1:\\packfiles\\..\\escape"), "Unsafe path resolved");
    auto* entry = vfs.ResolvePath("em1:\\PACKFILES\\DBC_1ST_ZONEC.PAK");
    Check(entry && entry->size() == 12268960, "Wrong mounted archive metadata");
    File* raw = nullptr;
    FileAction action{};
    Check(vfs.OpenFile(nullptr, "em1:\\packfiles\\dbc_1st_zonec.pak", FileDisposition::kOpen,
        FileAccess::kGenericRead, false, true, &raw, &action) == X_STATUS_SUCCESS && raw,
        "Cannot open mounted original archive");
    std::unique_ptr<File, FileCloser> file(raw);
    step("opened");
    std::vector<uint8_t> expected;
    Check(source->ReadFile("packfiles/dbc_1st_zonec.pak", expected, error), error);
    std::vector<uint8_t> actual(expected.size());
    size_t read = 0;
    Check(file->ReadSync(actual.data(), actual.size(), 0, &read) == X_STATUS_SUCCESS &&
              read == expected.size() && actual == expected, "Mounted range bytes differ");
    step("full-read");
    std::array<uint8_t, 20> tail{};
    Check(file->ReadSync(tail.data(), tail.size(), expected.size() - 3, &read) == X_STATUS_SUCCESS &&
              read == 3 && std::equal(tail.begin(), tail.begin()+3, expected.end()-3), "Partial EOF mismatch");
    Check(file->ReadSync(tail.data(), 1, expected.size(), &read) == X_STATUS_END_OF_FILE && read == 0,
          "EOF status mismatch");
    Check(file->ReadSync(nullptr, 0, SIZE_MAX, &read) == X_STATUS_SUCCESS && read == 0,
          "Zero-length read mismatch");
    step("eof");
    void* guard = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    Check(guard != nullptr, "Cannot create guarded test page");
    const auto fault_status = file->ReadSync(guard, 32, 0, &read);
    step("guarded");
    VirtualFree(guard, 0, MEM_RELEASE);
    Check(fault_status == X_STATUS_ACCESS_VIOLATION && read == 0, "Guarded destination did not reject");
    size_t written = 999;
    Check(file->WriteSync(tail.data(), tail.size(), 0, &written) == X_STATUS_ACCESS_DENIED && written == 0 &&
              file->SetLength(0) == X_STATUS_ACCESS_DENIED, "Mounted source accepted a write");
    File* denied = nullptr;
    Check(entry->Open(FileAccess::kGenericWrite, &denied) == X_STATUS_ACCESS_DENIED && !denied,
          "Mounted source granted write access");
    Check(!MountRvz(vfs, source, error), "Existing mounted source was replaced");
    Check(file->ReadSync(tail.data(), tail.size(), 0, &read) == X_STATUS_SUCCESS && read == tail.size(),
          "Failed duplicate mount damaged an open handle");
    Check(std::filesystem::last_write_time(argv[1]) == before, "Original source timestamp changed");
    file.reset();
    step("complete");
    std::cout << "PASS: native em1: mount, full archive/EOF reads, directory tree, permissions, guarded buffer, unchanged game:/source\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
  }
}

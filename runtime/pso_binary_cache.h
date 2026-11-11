#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
namespace em2 {
class PsoBinaryStore;
std::shared_ptr<PsoBinaryStore> StartPsoBinaryStore(const std::filesystem::path& root);
void StopPsoBinaryStore(std::shared_ptr<PsoBinaryStore>& store);
struct PsoBinaryStats {uint64_t calls,keys,hits,accepted,rejected,saved,uncacheable,dropped;};
PsoBinaryStats ReadPsoBinaryStats();
bool TestPsoBinaryCache(const std::filesystem::path& directory);
bool VerifyPsoBinaryGpuTest();
}

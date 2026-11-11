#pragma once
#include <filesystem>
#include <memory>
#include <cstdint>
namespace rex::ui::d3d12 {class D3D12Provider;}
namespace rex::graphics::d3d12 {class D3D12CommandProcessor;}
namespace em2 {
class PcShaderLibrary;
struct PcShaderLibraryStats {uint64_t hits=0,misses=0,loaded=0,rejected=0,saved=0,dropped=0;};
// The renderer storage lease owns this library through GPU shutdown. Disk I/O
// loads before native preloading and writes on a bounded background queue.
std::shared_ptr<PcShaderLibrary> OpenPcShaderLibrary(const std::filesystem::path&,const std::filesystem::path& seed={});
void UsePcShaderLibrary(const std::shared_ptr<PcShaderLibrary>&);
void FlushPcShaderLibrary(const std::shared_ptr<PcShaderLibrary>&);
bool SavePcShaderLibraryPack(const std::shared_ptr<PcShaderLibrary>&,const std::filesystem::path& output);
PcShaderLibraryStats GetPcShaderLibraryStats();
bool TestPcShaderLibrary(const rex::ui::d3d12::D3D12Provider&,const std::filesystem::path&);
bool TestRecordedPcShaderLibrary(rex::graphics::d3d12::D3D12CommandProcessor&,const std::filesystem::path& source,const std::filesystem::path& seed,const std::filesystem::path& output,unsigned scale);
// Isolated diagnostics may select a library (including null for native reference)
// on their own thread without changing the production cache or preferences.
class PcShaderLibraryScope {
public:
    explicit PcShaderLibraryScope(std::shared_ptr<PcShaderLibrary>);
    ~PcShaderLibraryScope();
    PcShaderLibraryScope(const PcShaderLibraryScope&)=delete;
private:
    std::shared_ptr<PcShaderLibrary> previous_;bool had_previous_;
};
}

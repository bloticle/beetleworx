#pragma once
#include <filesystem>
#include <memory>
namespace rex::graphics {class GraphicsSystem;}
namespace em2 {
class PsoBinaryStore;
class PcShaderLibrary;
std::filesystem::path RendererShaderStorageRoot(const std::filesystem::path& executable_directory);
std::filesystem::path PackagedPcShaderLibraryRoot(const std::filesystem::path& executable_directory);
class RendererShaderStorageLease {
public:
    explicit RendererShaderStorageLease(void* handle):handle_(handle){}
    ~RendererShaderStorageLease();
    void EnablePsoCache(const std::filesystem::path& root);
    RendererShaderStorageLease(const RendererShaderStorageLease&)=delete;
    RendererShaderStorageLease& operator=(const RendererShaderStorageLease&)=delete;
private:
    void* handle_;
    std::shared_ptr<PsoBinaryStore> pso_store_;
    std::shared_ptr<PcShaderLibrary> translated_store_;
};
// A lease means a request was dispatched, not that every SDK preload succeeded.
// Keep it alive until AFTER renderer shutdown; only one instance may write.
std::unique_ptr<RendererShaderStorageLease> InitializeRendererShaderStorage(rex::graphics::GraphicsSystem&,const std::filesystem::path& executable_directory,bool blocking=false);
bool VerifyRendererShaderStorageFiles(const std::filesystem::path& executable_directory);
void ConfigureRendererShaderStorageTest();
bool PopulateRendererShaderStorageTest(rex::graphics::GraphicsSystem&,const std::filesystem::path& directory);
bool VerifyPreloadedRendererShaderStorageTest(rex::graphics::GraphicsSystem&);
}

namespace em2 {
// Execute on the initialized GPU worker, never the application/UI thread.
bool TestTargetedScreenUpscale(rex::graphics::GraphicsSystem&,const std::filesystem::path&);
bool TestTargetedSceneLighting(rex::graphics::GraphicsSystem&,const std::filesystem::path&);
bool PrepareArchivePcShaders(rex::graphics::GraphicsSystem&,const std::filesystem::path& catalog,const std::filesystem::path& output,const std::filesystem::path& observed_cache);
bool TestRecordedPcShaderCaches(rex::graphics::GraphicsSystem&,const std::filesystem::path& source,const std::filesystem::path& seed,const std::filesystem::path& output);
}

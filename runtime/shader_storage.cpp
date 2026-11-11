#include "shader_storage.h"
#include "pso_binary_cache.h"
#include "pc_shader_library.h"
#include "shadow_kernel.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/graphics/graphics_system.h>
#include <windows.h>
#include <chrono>

REXCVAR_DEFINE_BOOL(em2_persistent_shader_cache,true,"GPU","Persist/preload native shader and pipeline storage across runs");
std::filesystem::path em2::PackagedPcShaderLibraryRoot(const std::filesystem::path& executable_directory){return executable_directory/"pc-assets"/"pc-shader-library"/EM2_RENDERER_CACHE_ABI/"native-profile-v1";}
std::filesystem::path em2::RendererShaderStorageRoot(const std::filesystem::path& directory) {
    // The namespace follows the graphics SDK, not the changing game EXE hash.
    // Offline saves and isolated online sessions share this installation cache.
    const auto original=directory/"cache"/"renderer-v1"/EM2_RENDERER_CACHE_ABI;
    return NativeShadowSamples()==4?original:original/NativeShadowCacheKey();
}
em2::RendererShaderStorageLease::~RendererShaderStorageLease(){StopPsoBinaryStore(pso_store_);translated_store_.reset();CloseHandle(handle_);}
void em2::RendererShaderStorageLease::EnablePsoCache(const std::filesystem::path& root){
    try{std::wstring module(32768,L'\0');const auto length=GetModuleFileNameW(nullptr,module.data(),DWORD(module.size()));if(!length || length>=module.size())throw std::runtime_error("executable path unavailable");module.resize(length);
        const auto seed=PackagedPcShaderLibraryRoot(std::filesystem::path(module).parent_path());
        translated_store_=OpenPcShaderLibrary(root,seed);UsePcShaderLibrary(translated_store_);
    }catch(const std::exception& error){REXLOG_WARN("PC shader library unavailable: {}; native translation continues",error.what());}
    try{pso_store_=StartPsoBinaryStore(root);}catch(const std::exception& error){REXLOG_WARN("Compiled PSO cache unavailable: {}; native shader persistence continues",error.what());}
}
std::unique_ptr<em2::RendererShaderStorageLease> em2::InitializeRendererShaderStorage(rex::graphics::GraphicsSystem& graphics,const std::filesystem::path& directory,bool blocking) {
    if(!REXCVAR_GET(em2_persistent_shader_cache))return {};
    try {
        const auto root=RendererShaderStorageRoot(std::filesystem::absolute(directory));
        std::filesystem::create_directories(root);
        const auto owner=CreateFileW((root/".owner.lock").c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(owner==INVALID_HANDLE_VALUE){REXLOG_WARN("Renderer shader cache busy/unavailable (Win32 {}); continuing without persistence",GetLastError());return {};}
        auto lease=std::make_unique<RendererShaderStorageLease>(owner);
        const auto probe=root/(".write-test-"+std::to_string(GetCurrentProcessId())+"-"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto file=CreateFileW(probe.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        if(file==INVALID_HANDLE_VALUE){REXLOG_WARN("Renderer shader cache unavailable (Win32 {}); continuing without persistence",GetLastError());return {};}
        CloseHandle(file);
        REXLOG_INFO("Initializing persistent renderer shader/pipeline storage: {} (title 42560816, blocking {})",root.string(),blocking);
        lease->EnablePsoCache(root);
        graphics.InitializeShaderStorage(root,0x42560816,blocking);
        return lease;
    }catch(const std::exception& error){REXLOG_WARN("Renderer shader cache unavailable: {}; continuing without persistence",error.what());return {};}
}
bool em2::VerifyRendererShaderStorageFiles(const std::filesystem::path& directory) {
    try {
        const auto root=RendererShaderStorageRoot(std::filesystem::absolute(directory));
        unsigned files=0;uint64_t bytes=0;
        for(const auto& entry:std::filesystem::recursive_directory_iterator(root))if(entry.is_regular_file()) {
            ++files;bytes+=entry.file_size();REXLOG_INFO("Shader storage diagnostic file: {} ({} bytes)",entry.path().string(),entry.file_size());
        }
        REXLOG_INFO("Renderer shader storage diagnostic: {} files, {} bytes",files,bytes);
        return files!=0 && bytes!=0;
    }catch(const std::exception& error){REXLOG_ERROR("Shader storage verification failed: {}",error.what());return false;}
}

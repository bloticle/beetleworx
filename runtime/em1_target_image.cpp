#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "em1_target_image.h"
#include "em1_animation_variables.h"
#include "ppc_config.h"
#include <rex/runtime.h>
#include <rex/logging.h>
#include <windows.h>
#include <chrono>
#include <algorithm>
#include <stdexcept>

namespace em2::em1 {
namespace {
struct PrivateDirectory {
  std::filesystem::path parent,path;
  PrivateDirectory(){
    parent=std::filesystem::canonical(std::filesystem::temp_directory_path());
    path=parent/("EM2Recomp-image-audit-"+std::to_string(GetCurrentProcessId())+"-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if(!std::filesystem::create_directory(path))throw std::runtime_error("Could not create private image-audit directory");
    std::filesystem::create_directory(path/"updates");
  }
  ~PrivateDirectory(){
    // Delete only this newly created task directory, after resolving it and
    // excluding reparse points. Never traverse a link into game/user content.
    try{
      if(std::filesystem::canonical(path).parent_path()!=parent ||
          !path.filename().wstring().starts_with(L"EM2Recomp-image-audit-"))return;
      const auto attributes=GetFileAttributesW(path.c_str());
      if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))return;
      for(const auto& entry:std::filesystem::recursive_directory_iterator(path)){
        const auto flags=GetFileAttributesW(entry.path().c_str());
        if(flags==INVALID_FILE_ATTRIBUTES || (flags&FILE_ATTRIBUTE_REPARSE_POINT))return;
      }
      std::filesystem::remove_all(path);
    }catch(const std::exception& e){REXLOG_WARN("[em1] private image-audit cleanup: {}",e.what());}
  }
};
bool Readable(uint8_t* base,uint32_t address,size_t size){
  if(!base || !size || address<uint32_t(PPC_IMAGE_BASE) ||
      uint64_t(address)+size>uint64_t(PPC_IMAGE_BASE)+uint64_t(PPC_IMAGE_SIZE))return false;
  auto at=reinterpret_cast<uintptr_t>(base)+address;const auto end=at+size;
  while(at<end){MEMORY_BASIC_INFORMATION info{};
    if(!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof(info)) || info.State!=MEM_COMMIT ||
        (info.Protect&(PAGE_NOACCESS|PAGE_GUARD)))return false;
    const auto next=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
    if(next<=at)return false;at=std::min(next,end);
  }
  return true;
}
}
bool ReadHeadlessTargetAnimationVariables(const std::filesystem::path& game_directory,
    std::map<std::string,uint32_t>& output,std::string& error){
  try{
    if(rex::Runtime::instance()){error="Target image audit requires an isolated pre-runtime call";return false;}
    PrivateDirectory private_directory;
    rex::Runtime runtime(game_directory,private_directory.path,private_directory.path/"updates");
    const auto setup=runtime.Setup(true);
    // SDK tool mode retains its NOP audio/XMA worker for import compatibility;
    // it has no audio output driver. A graphics backend is never needed here.
    if(XFAILED(setup) || !runtime.is_tool_mode() || runtime.graphics_system()){
      error="SDK tool-mode target initialization failed or created a graphics subsystem";return false;
    }
    if(XFAILED(runtime.LoadXexImage("game:\\default.xex"))){error="SDK could not load the user's target default.xex";return false;}
    auto* base=runtime.virtual_membase();
    auto word=[&](uint32_t at,uint32_t& value){
      if(!Readable(base,at,4))return false;
      const auto* b=base+at;value=uint32_t(b[0])<<24|uint32_t(b[1])<<16|uint32_t(b[2])<<8|b[3];return true;
    };
    auto string=[&](uint32_t at,std::string& value){
      if(!Readable(base,at,128))return false;
      const auto* b=base+at;const auto* end=std::find(b,b+128,uint8_t(0));
      if(end==b+128)return false;value.assign(reinterpret_cast<const char*>(b),size_t(end-b));return true;
    };
    std::map<std::string,uint32_t> prepared;
    if(!ReadXboxAnimationVariables(word,string,prepared,error))return false;
    REXLOG_INFO("[em1] checked native target animation variables: count={}; SDK tool mode, no guest execution/window/GPU, NOP audio",prepared.size());
    output=std::move(prepared);error.clear();return true;
  }catch(const std::exception& e){error=e.what();return false;}
}
}

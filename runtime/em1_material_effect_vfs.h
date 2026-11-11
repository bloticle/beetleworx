#pragma once
#include "em1_material_effects.h"
#include <rex/filesystem/vfs.h>
#include <rex/filesystem/file.h>
#include <rex/kernel/kernel_state.h>
#include <memory>

namespace em2::em1 {
// Read the exact archive the native loader sees, never a guessed host path.
// Headless preparation without a mounted runtime intentionally returns0.
inline uint32_t MountedSimpleMaterialEffects(rex::filesystem::VirtualFileSystem& fs) {
  using rex::X_STATUS; // SDK status macros use this unqualified typedef.
  try {
    constexpr const char* path="d:\\packfiles\\shaders.pak";
    auto* entry=fs.ResolvePath(path);if(!entry || entry->size()<32 || entry->size()>512ull*1024*1024)return 0;
    rex::filesystem::File* raw=nullptr;rex::filesystem::FileAction action{};
    const auto status=fs.OpenFile(nullptr,path,rex::filesystem::FileDisposition::kOpen,
        rex::filesystem::FileAccess::kGenericRead,false,true,&raw,&action);
    const auto close=[](rex::filesystem::File* file){if(file)file->Destroy();};
    std::unique_ptr<rex::filesystem::File,decltype(close)> owner(raw,close);
    // Transparent native PAK forwarding deliberately opens its original File,
    // whose Entry differs from the forwarding directory's proxy. The opened
    // mounted path remains authoritative; never require proxy pointer equality.
    if(status!=X_STATUS_SUCCESS || !raw || !raw->entry())return 0;
    const size_t file_size=raw->entry()->size();
    if(file_size<32 || file_size>512ull*1024*1024)return 0;
    std::array<uint8_t,32> header{};size_t read=0;
    if(raw->ReadSync(header.data(),header.size(),0,&read)!=X_STATUS_SUCCESS || read!=header.size())return 0;
    const bool big=header[0]==' ' && header[1]=='K' && header[2]=='A' && header[3]=='P';
    if(!big && !(header[0]=='P' && header[1]=='A' && header[2]=='K' && header[3]==' '))return 0;
    auto word=[&](size_t at){uint32_t v=0;for(unsigned i=0;i<4;++i)v|=uint32_t(header[at+i])<<(8*(big?3-i:i));return v;};
    const size_t payload=size_t(word(12))+word(16);
    if(payload<36 || payload>16*1024*1024 || payload>file_size)return 0;
    std::vector<uint8_t> directory(payload);
    if(raw->ReadSync(directory.data(),directory.size(),0,&read)!=X_STATUS_SUCCESS || read!=directory.size())return 0;
    return ReadSimpleMaterialEffectDirectory(directory,file_size);
  }catch(...){return 0;}
}
inline uint32_t MountedSimpleMaterialEffects() {
  auto* kernel=rex::kernel::kernel_state();if(!kernel || !kernel->file_system())return 0;
  // Ribbon-only availability must not invalidate every cached level model.
  return MountedSimpleMaterialEffects(*kernel->file_system())&127u;
}
}

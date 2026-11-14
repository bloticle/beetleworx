#pragma once
#include "online_lobby.h"
#include <filesystem>
#include <span>
namespace em2::net {
struct ClothingTexture {std::vector<uint8_t> bytes;uint32_t changed_pixels=0,mip_levels=0;};
// Recognized default diffuse textures only. Unknown/custom layouts are rejected
// without altering the output. Produces portable NiPixelData for the native
// texture loader; no shader, original archive or shared texture is modified.
bool RecolorOnlinePants(std::span<const uint8_t>,LobbyCharacter,uint8_t color,ClothingTexture&,std::string& error);
bool TestOnlineClothingTextures();
// Set after content mounting, before guest execution. Variants stay in memory.
void ConfigureOnlineClothingArchive(const std::filesystem::path&);
std::string OnlineClothingSourceKey();
bool ReadMountedOnlineClothingVariant(LobbyCharacter,uint8_t,ClothingTexture&,std::string& error);
bool TestOnlineClothingArchive(const std::filesystem::path& fixture);
// Explicit canonical-executable asset diagnostic, not a public setup path.
bool WriteOnlineClothingVariants(const std::filesystem::path& source,const std::filesystem::path& destination,std::string& error);
}

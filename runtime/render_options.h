#pragma once
#include <filesystem>
#include <cstdint>
namespace em2 {
bool TestRenderOptions();
bool TestNativeShadowShader(const std::filesystem::path& output={});
bool TestNativeBloomShader(const std::filesystem::path& output={});
void LogLightingProfile();
void ApplyLiveLightingSettings(uint8_t* base);
}

#pragma once
#include <string>
#include <string_view>
#include <map>
#include <cstdint>
namespace em2::em1 {
// Read-only host checks in the existing executable, before creating a window.
// Success establishes importability only, never campaign gameplay readiness.
bool CheckLevelAssets(std::string_view rvz,std::string_view descriptor,std::string& error,
                      std::string_view callbacks={},const std::map<std::string,uint32_t>* native_variables=nullptr,
                      std::string_view native_triangle_path={},bool native_triangle_accelerated=false);
}

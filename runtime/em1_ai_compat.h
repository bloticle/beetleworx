#pragma once
#include <cstdint>
#include <string>
struct PPCContext;
namespace em2::em1 {
bool TryLegacyBodylessAi(PPCContext&,uint8_t*);
bool TestLegacyBodylessAi(std::string& error);
}

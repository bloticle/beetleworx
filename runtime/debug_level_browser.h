#pragma once
#include <string>
#include <string_view>
struct PPCContext;
namespace em2 {
void DrawDebugLevelBrowser();
std::string QueueDebugLevelLoad(std::string_view destination);
void TickDebugLevelLoads(PPCContext&,unsigned char* base);
bool TestDebugLevelCatalog();
}

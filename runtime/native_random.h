#pragma once
#include <cstdint>
namespace em2::net {struct Identity;}
namespace em2 {
void BeginNativeOnlineRandom(const net::Identity&);
void EndNativeOnlineRandom();
// Loader-thread only. Seed the next real native rand once; never restore the
// previous startup stream. An explicit native srand remains authoritative.
void BeginNativeOnlineWorldRandom(uint32_t seed);
class VisualRandomScope {
public:
    VisualRandomScope();~VisualRandomScope();
    VisualRandomScope(const VisualRandomScope&)=delete;
    VisualRandomScope& operator=(const VisualRandomScope&)=delete;
};
bool TestNativeRandom();
}

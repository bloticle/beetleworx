#pragma once
#include <cstdint>
struct PPCContext;
namespace em2 {
bool TryMovieTextureAllocation(PPCContext&,uint8_t*);
bool ReleaseMovieTextureAllocation(PPCContext&,uint8_t*);
bool TestMovieTextureMemory();
}

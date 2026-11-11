#pragma once
#include <cstdint>
namespace rex::graphics {class GraphicsSystem;class CommandProcessor;}
namespace em2 {
bool IsDeduplicatedConstantRange(uint32_t index,uint32_t count);
bool IsBulkGpuRegisterRange(uint32_t index,uint32_t count);
// GPU-thread local count, also used to verify actual parser dispatch in diagnostics.
uint64_t GpuConstantBatchCount();
bool IsGpuCommandProfilingEnabled(const rex::graphics::CommandProcessor&);
bool TestGpuConstantBackend(rex::graphics::GraphicsSystem&);
}

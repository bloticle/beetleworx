#pragma once
#include <cstdint>
namespace em2 {
extern thread_local uint32_t transfer_benchmark_draws;
void TransferBenchmarkAtSwap();
}

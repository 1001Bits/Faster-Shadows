// Private diagnostic ABI; never enabled in the shipping DLL.
#pragma once
#include <cstdint>
namespace fps {
struct BenchmarkCadence {
    std::uint64_t epoch;
    std::uint32_t enabled, cadence, count, plannedSkipMask;
};
static_assert(sizeof(BenchmarkCadence) == 24);
}

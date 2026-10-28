#pragma once
#include <windows.h>
#include <algorithm>
#include <thread>

namespace extractor {
// Default worker count: every logical CPU thread, limited so that each worker can hold
// a decoded package of up to 512 MiB in the physical memory that is free right now.
inline unsigned defaultWorkers() {
    unsigned workers = std::clamp(std::thread::hardware_concurrency(), 1u, 64u);
    MEMORYSTATUSEX memory{sizeof(memory)};
    if (GlobalMemoryStatusEx(&memory)) {
        const auto fit = memory.ullAvailPhys / (512ull * 1024 * 1024);
        workers = static_cast<unsigned>(std::clamp<unsigned long long>(fit, 1, workers));
    }
    return workers;
}
}

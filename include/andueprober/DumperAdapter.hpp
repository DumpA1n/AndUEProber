#pragma once

#include "Reflection.hpp"

namespace andueprober {

struct DumperOptions {
    std::size_t maximumInputBytes = 4 * 1024 * 1024;
    std::size_t maximumItems = 65536;
    // Applies to the complete header and each intermediate formatting buffer.
    // The supported range is 1 through 64 MiB; it is not a total heap limit.
    std::size_t maximumOutputBytes = 8 * 1024 * 1024;
    const std::atomic<bool>* cancelled = nullptr;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
};
struct DumperHeaderResult {
    Status status;
    std::string header;
};

// Emits the frozen data-layout subset into namespace andueprober_sdk. The header
// contains explicit padding and compile-time size, alignment and offset checks.
// Addresses are uint64_t values; no engine pointers, methods or live readers are
// accessed. The result is in memory; publishExport owns filesystem publication.
// Checkpoints bound CPU work between formatting chunks, not scheduler latency.
DumperHeaderResult buildDumperHeader(const FrozenReflection&, const DumperOptions& = {});
// Identifies the pinned source and checked transformed emitter translation unit.
std::string_view dumperDependencyIdentity() noexcept;

} // namespace andueprober

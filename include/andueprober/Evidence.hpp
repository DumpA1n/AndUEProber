#pragma once

#include "Core.hpp"

namespace andueprober {

struct EvidenceLimits {
    std::size_t maximumNodes = 4096;
    std::size_t maximumDependencies = 16384;
    std::size_t maximumMetadataBytes = 4 * 1024 * 1024;
    std::size_t maximumTextBytes = 1024;
    const std::atomic<bool>* cancelled = nullptr;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
};

// Validates only the roots and their transitive dependency closure. Every node
// requires a current validated value/version and nonempty named UTF-8 evidence;
// dependency versions must match and cycles are rejected. Unrelated offsets and
// diagnostics are excluded. Roots and snapshot remain immutable during this call.
// Metadata accounting covers entry sizes and text bytes, not total heap usage.
// The iterative traversal checks cancellation/deadline between bounded entries.
// Memory-reader identity checks remain the caller's responsibility.
Status validateEvidenceClosure(const Snapshot&, std::span<const std::string> roots,
    const EvidenceLimits& = {});

} // namespace andueprober

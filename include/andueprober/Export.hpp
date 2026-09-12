#pragma once

#include "Core.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace andueprober {
struct ExportFile { std::string relativePath; std::string content; };
enum class ExportOperation { CreateDirectory, Open, Write, Flush, Close, Publish, Finalize };
struct ExportOptions {
    std::filesystem::path root;
    const std::atomic<bool>* cancelled = nullptr;
    // Invoked before an operation. Exceptions become Internal. Failed operations
    // retain incomplete output; marker retraction failure is explicitly uncertain.
    std::function<Status(ExportOperation, const std::filesystem::path&)> beforeOperation;
    std::string toolRevision;
    std::map<std::string, std::string> dependencyRevisions;
    std::size_t maximumFiles = 4096;
    std::size_t maximumFileBytes = 16 * 1024 * 1024;
    std::size_t maximumTotalBytes = 64 * 1024 * 1024;
    std::size_t maximumManifestBytes = 4 * 1024 * 1024;
    std::size_t maximumMetadataEntries = 16384;
    std::size_t maximumPathBytes = 1024;
    // Checked between bounded CPU work and filesystem operations. A blocked
    // kernel operation cannot be interrupted by this deadline or cancellation.
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
};
struct ExportResult {
    Status status;
    std::filesystem::path publishedDirectory;
    std::filesystem::path incompleteDirectory;
    // Failed marker retraction, directory flush or close leaves completion durability uncertain.
    bool completionUncertain = false;
};
// Snapshot, files and options must remain immutable for this call; the cancellation
// flag is the cross-thread control. Use a frozen Session snapshot in analysis workflows.
ExportResult publishExport(const Snapshot&, const std::vector<ExportFile>&, const ExportOptions&);
// Checks marker presence, not content integrity or durable storage. A failed result
// with completionUncertain requires inspection even if a marker remains present.
bool isCompletedExport(const std::filesystem::path& directory);
}

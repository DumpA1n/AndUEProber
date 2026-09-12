#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <sys/types.h>
#include "andueprober/Core.hpp"
#include "andueprober/Probe.hpp"
#include "andueprober/Names.hpp"
#include "andueprober/Relations.hpp"
#include <string>

enum class EDumpStatus { Idle, Running, Success, Failed, Cancelled };

// ============================================================
//  Phase 1: Auto-detect game and prepare probing infrastructure
// ============================================================

struct GameDetectionResult {
    bool Success = false;
    std::string GameName;
    std::string PackageName;
    uintptr_t GUObjectArrayPtr = 0;  // absolute VA of FUObjectArray
    uintptr_t ObjectsFieldAddr = 0;  // address TO READ to get Objects pointer
    uintptr_t UEBaseAddress = 0;     // UE module base address
    uintptr_t NamePoolPtr = 0;
    int32_t NumElementsPerChunk = 0;    // 0 = flat (FUObjectItem*), >0 = chunked (FUObjectItem**)
};

// Selects a normal-linker module by exact name and uses bounded discovery only.
andueprober::Status DetectAndPrepareGame(GameDetectionResult& result);

andueprober::Status FullSdkExportAdmission();
andueprober::Status RunAutomaticProfilePhase(int phase, andueprober::Snapshot&);
andueprober::Status RunFullSdkDump(const andueprober::Snapshot&, const std::string& outputRoot,
    const std::atomic<bool>& cancelled, std::string& outputDirectory);
andueprober::Status InspectTargetMemory(std::uintptr_t address, std::uint32_t size,
    andueprober::Snapshot&);

// The composition root owns the cancellation flag for the worker lifetime.
void ConfigureProbeOperation(const std::atomic<bool>* cancelled);
andueprober::Status ConfigureTargetProcess(pid_t pid, std::string packageName);
bool ProbeCancelled();

void CaptureProbeIdentity(andueprober::Snapshot&);

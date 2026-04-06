#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

enum class EDumpStatus { Idle, Running, Success, Failed };

// ============================================================
//  Phase 1: Auto-detect game and prepare probing infrastructure
// ============================================================

struct GameDetectionResult {
    bool success = false;
    std::string gameName;
    std::string packageName;
    uintptr_t guobjectArrayPtr = 0;  // absolute VA of FUObjectArray
    uintptr_t objectsFieldAddr = 0;  // address TO READ to get FUObjectItem** (Objects)
    uintptr_t ueBaseAddress = 0;     // UE module base address
};

// Initialize KittyMemoryEx kMgr for self-process, iterate profiles,
// match by AppID, call GetGUObjectArrayPtr. Returns GObjects info.
bool DetectAndPrepareGame(GameDetectionResult& result);

// Resolve FName by calling the matched profile's GetNameByID.
// Only valid after DetectAndPrepareGame returns true.
std::string ProfileGetNameByID(int32_t id);

// ============================================================
//  Phase 2: After UEProber probing — set offsets and dump
// ============================================================

struct ProbedOffsets {
    // UObject
    uintptr_t objFlags = 0, objIndex = 0, objClass = 0, objName = 0, objOuter = 0;
    // UField
    uintptr_t fieldNext = 0;
    // UStruct
    uintptr_t structSuper = 0, structChildren = 0, structChildProps = 0, structSize = 0;
    // UFunction
    uintptr_t funcFlags = 0, funcNumParams = 0, funcParamSize = 0, funcFunc = 0;
    // FField
    uintptr_t ffieldClass = 0, ffieldNext = 0, ffieldName = 0, ffieldFlags = 0;
    // FProperty
    uintptr_t fpropArrayDim = 0, fpropElemSize = 0, fpropFlags = 0, fpropOffset = 0, fpropSize = 0;
};

// Set probed offsets into the matched profile, then run UEDumper.Init + Dump.
void StartDumpWithProbedOffsets(
    const ProbedOffsets& offsets,
    std::atomic<EDumpStatus>& status,
    std::string& outError,
    std::string& outDir
);


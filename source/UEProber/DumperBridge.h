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
    bool Success = false;
    std::string GameName;
    std::string PackageName;
    uintptr_t GUObjectArrayPtr = 0;  // absolute VA of FUObjectArray
    uintptr_t ObjectsFieldAddr = 0;  // address TO READ to get Objects pointer
    uintptr_t UEBaseAddress = 0;     // UE module base address
    uintptr_t DecryptFNameAddr = 0;     // FName resolver entry: profile-specific decrypt/name call; zero means unavailable.
    int32_t NumElementsPerChunk = 0;    // 0 = flat (FUObjectItem*), >0 = chunked (FUObjectItem**)
};

// Initialize KittyMemoryEx kMgr for self-process, iterate profiles,
// match by AppID, call GetGUObjectArrayPtr. Returns GObjects info.
bool DetectAndPrepareGame(GameDetectionResult& result);

// Bridge: memory operations via KittyMemoryEx kMgr (avoids KittyMemory/KittyMemoryEx
// header conflicts in translation units that already include KittyMemory).
ssize_t KMgrReadMem(uintptr_t address, void* buffer, size_t size);
size_t  KMgrWriteMem(uintptr_t address, void* buffer, size_t size);
bool    KMgrRead(uintptr_t address, void* buffer, size_t size);
bool    KMgrIsValidPtr(uintptr_t address);

// Resolve FName by calling the matched profile's GetNameByID.
// Only valid after DetectAndPrepareGame returns true.
std::string ProfileGetNameByID(int32_t id);

// In-memory FName layout for the matched profile (engine-version aware: UE5.1
// reordered Number<->DisplayIndex, case-preserving adds DisplayIndex, outline
// drops the inline Number). Sentinels: numberOffset < 0 => outline-number build
// (no inline Number); displayOffset < 0 => not case-preserving.
struct FNameLayout {
    int32_t comparisonOffset = -1;
    int32_t displayOffset = -1;
    int32_t numberOffset = -1;
    int32_t size = 0;
};
FNameLayout ProfileGetFNameLayout();

// Offset-based GObjects iteration via the matched profile's UEVars (wired by
// InitUEVars in DetectAndPrepareGame, like AndUEDumper). Returns raw object
// pointers; the prober reads every field at probed offsets, so it needs no
// typed object/FName layout of its own.
void*   BridgeGetObjectByIndex(int32_t index);
int32_t BridgeGetObjectNum();

// Call the matched profile's findProcessEvent.
// Returns true if found; writes absolute address and vtable index.
bool ProfileFindProcessEvent(uint8_t* uObject, uintptr_t* pe_address_out, int* pe_index_out);

// ============================================================
//  Phase 2: After UEProber probing — set offsets and dump
// ============================================================

struct ProbedOffsets {
    // UObject
    uintptr_t objFlags = 0, objIndex = 0, objClass = 0, objName = 0, objOuter = 0;
    // UField
    uintptr_t fieldNext = 0;
    // UEnum
    uintptr_t uenumNames = 0;
    // UStruct
    uintptr_t structSuper = 0, structChildren = 0, structChildProps = 0, structSize = 0;
    // UClass
    uintptr_t uclassCastFlags = 0, uclassDefaultObject = 0;
    // UFunction
    uintptr_t funcFlags = 0, funcNumParams = 0, funcParamSize = 0, funcFunc = 0;
    // FField
    uintptr_t ffieldClass = 0, ffieldNext = 0, ffieldName = 0, ffieldFlags = 0;
    uintptr_t ffieldOwner = 0;  // FFieldVariant Owner offset (prober Phase5_ProbeFFieldOwner)
    // FProperty
    uintptr_t fpropArrayDim = 0, fpropElemSize = 0, fpropFlags = 0, fpropOffset = 0, fpropSize = 0;
    // FProperty subclass-tail start can differ from fpropSize when leading metadata exists.
    uintptr_t fpropSubBase = 0;
    // FEnumProperty UnderlyingType and Enum offsets for the selected layout.
    uintptr_t fenumUnderlying = 0, fenumEnum = 0;
    // Per-subclass FArray/FSet/FMap tail offsets override fpropSubBase.
    // DFM-style alt: individual container subclasses have their own per-class
    // leading-metadata pad that the global fpropSubBase value doesn't capture
    // (prober Phase5_ProbeFContainerPropertyTails).
    uintptr_t farrayInner = 0;
    uintptr_t fsetElement = 0;
    uintptr_t fmapKey = 0, fmapValue = 0;
};

// Set probed offsets into the matched profile, then run UEDumper.Init + Dump.
void StartDumpWithProbedOffsets(
    const ProbedOffsets& offsets,
    std::atomic<EDumpStatus>& status,
    std::string& outError,
    std::string& outDir
);


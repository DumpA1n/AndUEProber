#include "DumperBridge.h"
#include "BuildInfo.hpp"
#if ANDUEPROBER_HAS_PROCESS_MEMORY
#include "andueprober/ProcessMemory.hpp"
#endif

#include <chrono>
#include <string>
#include <mutex>
#include <limits>
#include "andueprober/Discovery.hpp"
#include <array>
#include <unistd.h>

#include "UE/UEOffsets.hpp"

#include "GameProfiles/IGameProfileEx.hpp"

// Profile definitions from the pinned AndUEDumper snapshot.
#include "UE/UEGameProfiles/BlackClover.hpp"
#include "UE/UEGameProfiles/Dislyte.hpp"
#include "UE/UEGameProfiles/Farlight.hpp"
#include "UE/UEGameProfiles/MortalKombat.hpp"
#include "UE/UEGameProfiles/PES.hpp"
#include "UE/UEGameProfiles/Torchlight.hpp"
#include "UE/UEGameProfiles/WutheringWaves.hpp"
#include "UE/UEGameProfiles/RealBoxing2.hpp"
#include "UE/UEGameProfiles/OdinValhalla.hpp"
#include "UE/UEGameProfiles/Injustice2.hpp"
#include "UE/UEGameProfiles/RooftopsParkour.hpp"
#include "UE/UEGameProfiles/BabyYellow.hpp"
#include "UE/UEGameProfiles/TowerFantasy.hpp"
#include "UE/UEGameProfiles/BladeSoul.hpp"
#include "UE/UEGameProfiles/Lineage2.hpp"
#include "UE/UEGameProfiles/NightCrows.hpp"
#include "UE/UEGameProfiles/Case2.hpp"
#include "UE/UEGameProfiles/KingArthur.hpp"
#include "UE/UEGameProfiles/Century.hpp"
#include "UE/UEGameProfiles/HelloNeighbor.hpp"
#include "UE/UEGameProfiles/HelloNeighborND.hpp"
#include "UE/UEGameProfiles/SFG2.hpp"
#include "UE/UEGameProfiles/ArkUltimate.hpp"
#include "UE/UEGameProfiles/Auroria.hpp"
#include "UE/UEGameProfiles/LineageW.hpp"
#include "UE/UEGameProfiles/RLSideswipe.hpp"
#include "GameProfiles/PUBG.hpp"
#include "GameProfiles/PUBGMHD.hpp"
#include "GameProfiles/DeltaForce.hpp"
#include "GameProfiles/NiZhan.hpp"
#include "GameProfiles/RocoKingdom.hpp"
#include "GameProfiles/ArenaBreakout.hpp"
#include "GameProfiles/Valorant.hpp"

#include "Utils/Logger.hpp"

// ============================================================
//  Bridge: bounded public memory reads without dependency types in the inspector
// ============================================================

namespace {
std::recursive_mutex g_AdapterMutex;
thread_local const std::atomic<bool>* g_Cancelled = nullptr;
thread_local andueprober::ReadBudget g_ReadBudget;
thread_local andueprober::ModuleImage g_ModuleImage;
thread_local andueprober::DiscoveryValue g_ObjectDiscovery;
thread_local std::uintptr_t g_ObjectArrayAddress = 0;
thread_local andueprober::ObjectArrayProfile g_ObjectArrayProfile;
#if ANDUEPROBER_HAS_PROCESS_MEMORY
thread_local std::unique_ptr<andueprober::ProcessMemory> g_Reader;
#endif
}
void ConfigureProbeOperation(const std::atomic<bool>* cancelled) {
    g_Cancelled = cancelled;
    g_ReadBudget = {};
    g_ReadBudget.remainingBytes = 64 * 1024 * 1024;
    g_ReadBudget.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    g_ReadBudget.cancelled = cancelled;
#if ANDUEPROBER_HAS_PROCESS_MEMORY
    if (g_Reader) g_ReadBudget.generation = g_Reader->generation();
#endif
}
bool ProbeCancelled() {
    return (g_Cancelled && g_Cancelled->load()) || std::chrono::steady_clock::now() >= g_ReadBudget.deadline;
}
void CaptureProbeIdentity(andueprober::Snapshot& snapshot) {
#if ANDUEPROBER_HAS_PROCESS_MEMORY
    if (g_Reader) {
        snapshot.moduleIdentity = g_Reader->identity();
        snapshot.generation = g_Reader->generation();
        snapshot.layoutIdentity = andueprober::objectArrayLayoutIdentity(g_ObjectArrayProfile);
        if (g_ObjectDiscovery.address) {
            andueprober::Offset candidate;
            candidate.value = static_cast<uint32_t>(*g_ObjectDiscovery.address - g_ModuleImage.loadBias);
            candidate.validation = andueprober::Validation::Candidate;
            candidate.origin = andueprober::Origin::Profile;
            candidate.evidence.push_back(g_ObjectDiscovery.evidence);
            andueprober::publishOffset(snapshot, "Module::GUObjectArray", std::move(candidate));
        }
    }
#endif
}
// ============================================================
//  Global state: matched profile for the running game
// ============================================================


static const std::vector<std::unique_ptr<IGameProfileEx>>& GetExProfiles()
{
    static const auto profiles = [] {
        std::vector<std::unique_ptr<IGameProfileEx>> values;
        values.push_back(std::make_unique<GameProfileEx<PESProfile>>());
        values.push_back(std::make_unique<GameProfileEx<DislyteProfile>>());
        values.push_back(std::make_unique<GameProfileEx<MortalKombatProfile>>());
        values.push_back(std::make_unique<GameProfileEx<FarlightProfile>>());
        values.push_back(std::make_unique<GameProfileEx<TorchlightProfile>>());
        values.push_back(std::make_unique<GameProfileEx<ArenaBreakoutProfile>>());
        values.push_back(std::make_unique<GameProfileEx<BlackCloverProfile>>());
        values.push_back(std::make_unique<GameProfileEx<WutheringWavesProfile>>());
        values.push_back(std::make_unique<GameProfileEx<RealBoxing2Profile>>());
        values.push_back(std::make_unique<GameProfileEx<OdinValhallaProfile>>());
        values.push_back(std::make_unique<GameProfileEx<Injustice2Profile>>());
        values.push_back(std::make_unique<GameProfileEx<RooftopParkourProfile>>());
        values.push_back(std::make_unique<GameProfileEx<BabyYellowProfile>>());
        values.push_back(std::make_unique<GameProfileEx<TowerFantasyProfile>>());
        values.push_back(std::make_unique<GameProfileEx<BladeSoulProfile>>());
        values.push_back(std::make_unique<GameProfileEx<Lineage2Profile>>());
        values.push_back(std::make_unique<GameProfileEx<Case2Profile>>());
        values.push_back(std::make_unique<GameProfileEx<CenturyProfile>>());
        values.push_back(std::make_unique<GameProfileEx<KingArthurProfile>>());
        values.push_back(std::make_unique<GameProfileEx<NightCrowsProfile>>());
        values.push_back(std::make_unique<GameProfileEx<HelloNeighborProfile>>());
        values.push_back(std::make_unique<GameProfileEx<HelloNeighborNDProfile>>());
        values.push_back(std::make_unique<GameProfileEx<SFG2Profile>>());
        values.push_back(std::make_unique<GameProfileEx<ArkUltimateProfile>>());
        values.push_back(std::make_unique<GameProfileEx<AuroriaProfile>>());
        values.push_back(std::make_unique<GameProfileEx<LineageWProfile>>());
        values.push_back(std::make_unique<GameProfileEx<RLSideswipeProfile>>());
        values.push_back(std::make_unique<GameProfileEx<PUBGProfile>>());
        values.push_back(std::make_unique<GameProfileEx<PUBGMHDProfile>>());
        values.push_back(std::make_unique<GameProfileEx<DeltaForceProfile>>());
        values.push_back(std::make_unique<GameProfileEx<NiZhanProfile>>());
        values.push_back(std::make_unique<GameProfileEx<RocoKingdomProfile>>());
        values.push_back(std::make_unique<GameProfileEx<ValorantProfile>>());
        return values;
    }();
    return profiles;
}

// ============================================================
//  Phase 1: Detect game and prepare
// ============================================================

andueprober::Status DetectAndPrepareGame(GameDetectionResult& result)
{
    using namespace andueprober;
    std::lock_guard lock(g_AdapterMutex);
    result = {};
    g_ObjectArrayAddress = 0;
    g_ObjectDiscovery = {};
    g_ModuleImage = {};
#if !ANDUEPROBER_HAS_PROCESS_MEMORY
    return {Error::Unsupported, "Analysis requires the explicit AndSwapChainHook::Memory dependency"};
#else
    g_Reader.reset();
    const std::string package = getprogname();
    IGameProfileEx* selected = nullptr;
    for (const auto& profile : GetExProfiles()) {
        for (const auto& name : profile->AsGameProfile()->GetAppIDs()) {
            if (name != package) continue;
            if (selected) return {Error::InvalidEvidence, "Multiple profiles match the selected process"};
            selected = profile.get();
        }
    }
    if (!selected) return {Error::Unsupported, "No bounded profile is configured for this process"};
    if (!selected->SupportsBoundedDiscovery()) return {Error::Unsupported, "The matched upstream profile has no bounded discovery provider"};
    auto reader = std::make_unique<ProcessMemory>();
    const std::array<std::string, 2> moduleNames{"libUE4.so", "libUnreal.so"};
    auto status = reader->openByName(moduleNames, g_ReadBudget);
    if (!status) return status;
    ModuleImage image;
    status = readModuleImage(*reader, reader->elfAddress(), reader->loadBias(), reader->identity(), g_ReadBudget, image);
    if (!status) return status;
    DiscoveryValue discovery;
    status = selected->DiscoverObjectArray(*reader, image, g_ReadBudget, discovery);
    if (!status) return status;
    if (!discovery.address || *discovery.address < image.loadBias || *discovery.address - image.loadBias > UINT32_MAX)
        return {Error::InvalidEvidence, "The object-array candidate requires a bounded module-relative address"};
    result.UEBaseAddress = image.loadBias;
    result.GUObjectArrayPtr = *discovery.address;
    auto* profileOffsets = selected->AsGameProfile()->GetOffsets();
    if (!profileOffsets) return {Error::InvalidEvidence, "The profile has no data-only object-array layout"};
    // Read Objects pointer from FUObjectArray
    if (profileOffsets->FUObjectArray.ObjObjects > std::numeric_limits<uintptr_t>::max() - result.GUObjectArrayPtr)
        return {Error::InvalidEvidence, "The profile object-array layout exceeds its bounds"};
    uintptr_t objObjectsAddr = result.GUObjectArrayPtr + profileOffsets->FUObjectArray.ObjObjects;
    for (auto field : {profileOffsets->TUObjectArray.Objects, profileOffsets->TUObjectArray.NumElements,
        profileOffsets->TUObjectArray.MaxElements, profileOffsets->TUObjectArray.NumChunks,
        profileOffsets->TUObjectArray.MaxChunks, profileOffsets->FUObjectItem.Object, profileOffsets->FUObjectItem.Size})
        if (field > 4096) return {Error::InvalidEvidence, "The profile object-array layout exceeds its bounds"};
    if (profileOffsets->TUObjectArray.Objects > std::numeric_limits<uintptr_t>::max() - objObjectsAddr ||
        profileOffsets->TUObjectArray.NumElementsPerChunk > 1024 * 1024) return {Error::InvalidEvidence, "The profile object-array layout exceeds its bounds"};
    g_ObjectArrayAddress = objObjectsAddr;
    g_ObjectArrayProfile.identity = selected->AsGameProfile()->GetAppName() + "@" + andueprober::build::dumperRevision;
    g_ObjectArrayProfile.objects = profileOffsets->TUObjectArray.Objects;
    g_ObjectArrayProfile.count = profileOffsets->TUObjectArray.NumElements;
    g_ObjectArrayProfile.capacity = profileOffsets->TUObjectArray.MaxElements;
    g_ObjectArrayProfile.chunkCount = profileOffsets->TUObjectArray.NumChunks;
    g_ObjectArrayProfile.chunkCapacity = profileOffsets->TUObjectArray.MaxChunks;
    g_ObjectArrayProfile.itemObject = profileOffsets->FUObjectItem.Object;
    g_ObjectArrayProfile.itemStride = profileOffsets->FUObjectItem.Size;
    g_ObjectArrayProfile.elementsPerChunk = profileOffsets->TUObjectArray.NumElementsPerChunk;
    result.ObjectsFieldAddr = objObjectsAddr + profileOffsets->TUObjectArray.Objects;
    LOGI("Objects field addr: %p (offset: 0x%lX)",
         (void*)result.ObjectsFieldAddr, result.ObjectsFieldAddr - result.UEBaseAddress);

    result.NumElementsPerChunk = static_cast<int32_t>(profileOffsets->TUObjectArray.NumElementsPerChunk);
    LOGI("TUObjectArray: elementsPerChunk=%d", result.NumElementsPerChunk);

    uint32_t count = 0;
    status = readObjectCount(*reader, g_ObjectArrayAddress, g_ObjectArrayProfile, g_ReadBudget, count);
    if (!status) return status;
    if (count) {
        uintptr_t first = 0;
        status = readObjectAt(*reader, g_ObjectArrayAddress, g_ObjectArrayProfile, 0, g_ReadBudget, first);
        if (!status) return status;
    }
    discovery.evidence.samples = count ? 1 : 0;
    discovery.evidence.sampleIdentities.push_back("observed-object-count:" + std::to_string(count));
    discovery.evidence.sampleIdentities.push_back("object-array-layout:" + objectArrayLayoutIdentity(g_ObjectArrayProfile));
    g_Reader = std::move(reader);
    g_ModuleImage = std::move(image);
    g_ObjectDiscovery = std::move(discovery);
    result.GameName = selected->AsGameProfile()->GetAppName();
    result.PackageName = package;
    result.Success = true;
    return {};
#endif
}

andueprober::Status FullSdkExportAdmission() {
    return {andueprober::Error::Unsupported, "Full SDK export requires complete property and container schemas; the available adapter emits only declared data layouts"};
}

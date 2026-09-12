#include "DumperBridge.h"
#include "BuildInfo.hpp"
#include "BoundedUEMemory.hpp"
#include "andueprober/Export.hpp"
#include "andueprober/Evidence.hpp"
#include "andueprober/Properties.hpp"
#if ANDUEPROBER_HAS_PROCESS_MEMORY
#include "andueprober/ProcessMemory.hpp"
#endif

#include <chrono>
#include <string>
#include <mutex>
#include <limits>
#include "andueprober/Discovery.hpp"
#include <array>
#include <filesystem>
#include <set>
#include <tuple>
#include <vector>
#include <unordered_map>
#include <unistd.h>

#include "Dumper.hpp"
#include "UE/UEMemory.hpp"
#include "UE/UEOffsets.hpp"
#include "UE/UEWrappers.hpp"
#include "KittyMemoryEx/KittyAsm.hpp"

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
thread_local andueprober::DiscoveryValue g_NameDiscovery;
thread_local std::uintptr_t g_ObjectArrayAddress = 0;
thread_local andueprober::ObjectArrayProfile g_ObjectArrayProfile;
thread_local IGameProfileEx* g_SelectedProfile = nullptr;
thread_local andueprober::Status g_UpstreamReadFailure;
thread_local std::optional<std::uint32_t> g_ProcessEventIndex;
thread_local std::optional<std::uint32_t> g_ProcessEventRelative;
thread_local pid_t g_TargetPid = 0;
thread_local std::string g_TargetPackage;
#if ANDUEPROBER_HAS_PROCESS_MEMORY
thread_local std::unique_ptr<andueprober::ProcessMemory> g_Reader;
#endif
bool boundedUpstreamRead(void*, std::uintptr_t address, void* output, std::size_t size) noexcept {
#if !ANDUEPROBER_HAS_PROCESS_MEMORY
    (void)address; (void)output; (void)size;
    return false;
#else
    try {
        if (!g_Reader || (!output && size)) return false;
        const auto status = andueprober::readExact(*g_Reader, address,
            {static_cast<std::byte*>(output), size}, g_ReadBudget);
        if (!status && g_UpstreamReadFailure) g_UpstreamReadFailure = status;
        return static_cast<bool>(status);
    } catch (...) {
        if (g_UpstreamReadFailure) g_UpstreamReadFailure = {andueprober::Error::Internal, {}};
        return false;
    }
#endif
}

template<class T>
andueprober::Status readValue(std::uintptr_t address, T& value) {
#if !ANDUEPROBER_HAS_PROCESS_MEMORY
    (void)address; (void)value;
    return {andueprober::Error::Unsupported, "Process memory is unavailable"};
#else
    if (!g_Reader) return {andueprober::Error::InvalidEvidence, "The bounded reader is not initialized"};
    return andueprober::readExact(*g_Reader, address,
        std::as_writable_bytes(std::span(&value, 1)), g_ReadBudget);
#endif
}

bool validText(const std::string& value) {
    return !value.empty() && value.size() <= 1024 && value.find('\0') == std::string::npos &&
        static_cast<bool>(andueprober::validateUtf8(value));
}

struct ObjectAnchor {
    std::uint32_t index = 0;
    std::uintptr_t address = 0;
    std::string name;
    std::string className;
    std::string identity;
};

andueprober::Status collectObjects(std::size_t maximum, const std::function<bool(const ObjectAnchor&)>& accept,
    std::vector<ObjectAnchor>& result) {
    result.clear();
    auto* objects = UEWrappers::GetObjects();
    if (!objects) return {andueprober::Error::InvalidEvidence, "The reflection object registry is unavailable"};
    const auto count = objects->GetNumElements();
    if (count <= 1 || count > 16 * 1024 * 1024)
        return {andueprober::Error::InvalidEvidence, "The reflection object count is outside the bounded range"};
    const auto examined = std::min<std::size_t>(static_cast<std::size_t>(count), maximum);
    for (std::size_t index = 1; index < examined; ++index) {
        if (ProbeCancelled()) return {andueprober::Error::Cancelled, "Object collection cancelled"};
        auto pointer = objects->GetObjectPtr(static_cast<std::int32_t>(index));
        if (!pointer) continue;
        UE_UObject object(pointer);
        ObjectAnchor anchor;
        anchor.index = static_cast<std::uint32_t>(index);
        anchor.address = reinterpret_cast<std::uintptr_t>(pointer);
        anchor.name = object.GetName();
        anchor.className = object.GetClass().GetName();
        if (!validText(anchor.name) || !validText(anchor.className)) continue;
        anchor.identity = anchor.className + ":" + anchor.name + "#" + std::to_string(index);
        if (accept(anchor)) result.push_back(std::move(anchor));
    }
    if (!g_UpstreamReadFailure) return g_UpstreamReadFailure;
    return {};
}

std::vector<std::string> identities(std::span<const ObjectAnchor> values, std::size_t maximum = 8) {
    std::vector<std::string> result;
    for (std::size_t index = 0; index < std::min(values.size(), maximum); ++index)
        result.push_back(values[index].identity);
    return result;
}

andueprober::Status publishLiveOffset(andueprober::Snapshot& snapshot, const std::string& name,
    std::uintptr_t rawValue, std::span<const std::string> samples,
    std::span<const std::string> dependencies, const std::string& source,
    andueprober::Origin origin = andueprober::Origin::Profile) {
    using namespace andueprober;
    if (rawValue > UINT32_MAX || samples.empty())
        return {Error::InvalidEvidence, "A bounded live offset requires a value and named samples"};
    Offset candidate;
    candidate.value = static_cast<std::uint32_t>(rawValue);
    candidate.origin = origin;
    candidate.validation = Validation::Validated;
    const auto retained = snapshot.offsets.find(name);
    if (retained != snapshot.offsets.end() && retained->second.origin == Origin::User) {
        if (retained->second.value != candidate.value)
            return {Error::InvalidEvidence, "Live observations contradict the explicit override for " + name};
        candidate = retained->second;
        candidate.validation = Validation::Validated;
    }
    candidate.dependencies.clear();
    for (const auto& dependency : dependencies) {
        const auto found = snapshot.offsets.find(dependency);
        if (found == snapshot.offsets.end() || found->second.validation != Validation::Validated)
            return {Error::InvalidEvidence, "A live offset dependency is unavailable: " + dependency};
        candidate.dependencies.emplace(dependency, found->second.version);
    }
    Evidence evidence{"bounded live reflection invariants match the selected profile field", true,
        samples.size(), {rawValue}, source, {}};
    evidence.sampleIdentities.assign(samples.begin(), samples.end());
    candidate.evidence.push_back(std::move(evidence));
    return publishOffset(snapshot, name, std::move(candidate));
}

std::vector<std::string> dependencyNames(int phase) {
    if (phase == 1) return {"Module::GUObjectArray", "Module::NamePool"};
    if (phase == 2) return {"UObject::InternalIndex", "UObject::NamePrivate", "UObject::ClassPrivate",
        "UObject::OuterPrivate", "UObject::ObjectFlags"};
    if (phase == 3) return {"UStruct::SuperStruct", "UStruct::Children", "UStruct::ChildProperties",
        "UStruct::PropertiesSize"};
    if (phase == 4) return {"UClass::CastFlags", "UClass::ClassDefaultObject"};
    if (phase == 5) return {"UFunction::FunctionFlags", "UFunction::NumParms", "UFunction::ParmsSize",
        "UFunction::ReturnValueOffset", "UFunction::Func"};
    return {"sizeof(FProperty)", "FProperty::SubPropertyBase", "FEnumProperty::UnderlyingType",
        "FEnumProperty::Enum", "FArrayProperty::Inner", "FSetProperty::ElementProp",
        "FMapProperty::KeyProp", "FMapProperty::ValueProp"};
}
}
void ConfigureProbeOperation(const std::atomic<bool>* cancelled) {
    g_Cancelled = cancelled;
    g_ReadBudget = {};
    g_ReadBudget.remainingBytes = std::size_t{2} * 1024 * 1024 * 1024;
    g_ReadBudget.deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    g_ReadBudget.cancelled = cancelled;
    g_UpstreamReadFailure = {};
#if ANDUEPROBER_HAS_PROCESS_MEMORY
    if (g_Reader) g_ReadBudget.generation = g_Reader->generation();
#endif
}
andueprober::Status ConfigureTargetProcess(pid_t pid, std::string packageName) {
    if (pid <= 0 || packageName.empty() || packageName.size() > 255 || packageName.find('/') != std::string::npos ||
        packageName.find('\0') != std::string::npos)
        return {andueprober::Error::InvalidArgument, "An explicit PID and bounded package name are required"};
    g_TargetPid = pid;
    g_TargetPackage = std::move(packageName);
    return {};
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
        if (g_SelectedProfile && g_SelectedProfile->AsGameProfile()->GetOffsets())
            snapshot.layout = g_SelectedProfile->AsGameProfile()->GetOffsets()->UStruct.ChildProperties ?
                andueprober::Layout::FField : andueprober::Layout::UProperty;
        if (g_ObjectDiscovery.address) {
            andueprober::Offset candidate;
            candidate.value = static_cast<uint32_t>(*g_ObjectDiscovery.address - g_ModuleImage.loadBias);
            candidate.validation = andueprober::Validation::Validated;
            candidate.origin = andueprober::Origin::Profile;
            candidate.evidence.push_back(g_ObjectDiscovery.evidence);
            andueprober::publishOffset(snapshot, "Module::GUObjectArray", std::move(candidate));
        }
        if (g_NameDiscovery.address) {
            andueprober::Offset candidate;
            candidate.value = static_cast<uint32_t>(*g_NameDiscovery.address - g_ModuleImage.loadBias);
            candidate.validation = andueprober::Validation::Validated;
            candidate.origin = andueprober::Origin::Profile;
            candidate.evidence.push_back(g_NameDiscovery.evidence);
            andueprober::publishOffset(snapshot, "Module::NamePool", std::move(candidate));
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
    g_NameDiscovery = {};
    g_ModuleImage = {};
    g_SelectedProfile = nullptr;
    UEMemory::ClearBoundedReader();
#if !ANDUEPROBER_HAS_PROCESS_MEMORY
    return {Error::Unsupported, "Analysis requires the explicit AndSwapChainHook::Memory dependency"};
#else
    g_Reader.reset();
    const std::string package = g_TargetPackage.empty() ? getprogname() : g_TargetPackage;
    const pid_t targetPid = g_TargetPid > 0 ? g_TargetPid : getpid();
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
    auto status = targetPid == getpid() ? reader->openByName(moduleNames, g_ReadBudget) :
        reader->openRemoteByName(targetPid, moduleNames, g_ReadBudget);
    if (!status) return status;
    ModuleImage image;
    status = reader->isRemote() ?
        readModuleImageFromFile(*reader, reader->modulePath(), reader->loadBias(), reader->identity(), g_ReadBudget, image) :
        readModuleImage(*reader, reader->elfAddress(), reader->loadBias(), reader->identity(), g_ReadBudget, image);
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
    discovery.evidence.sampleIdentities.push_back("observed-object-count:" + std::to_string(count));
    discovery.evidence.sampleIdentities.push_back("object-array-layout:" + objectArrayLayoutIdentity(g_ObjectArrayProfile));
    discovery.evidence.samples = discovery.evidence.sampleIdentities.size();
    DiscoveryValue names;
    status = selected->DiscoverNamePool(*reader, image, g_ReadBudget, names);
    if (!status) return status;
    if (!names.address || *names.address < image.loadBias)
        return {Error::InvalidEvidence, "The name-pool provider returned an invalid address"};
    result.NamePoolPtr = *names.address;

    g_Reader = std::move(reader);
    g_ModuleImage = std::move(image);
    g_ObjectDiscovery = std::move(discovery);
    g_NameDiscovery = std::move(names);
    g_SelectedProfile = selected;
    UEMemory::SetBoundedReader(nullptr, boundedUpstreamRead);
    if (!UEMemory::kMgr.initialize(targetPid, EK_MEM_OP_SYSCALL, false)) {
        UEMemory::ClearBoundedReader();
        g_SelectedProfile = nullptr;
        return {Error::Io, "The pinned reflection runtime could not inspect the selected process"};
    }
    selected->BindRuntime(result.GUObjectArrayPtr, result.NamePoolPtr);
    const auto initialized = selected->AsGameProfile()->InitUEVars();
    if (initialized != UEVarsInitStatus::SUCCESS) {
        UEMemory::ClearBoundedReader();
        g_SelectedProfile = nullptr;
        return {Error::InvalidEvidence, "The bounded profile could not initialize its reflection view"};
    }
    if (!g_UpstreamReadFailure) return g_UpstreamReadFailure;
    const auto* objects = UEWrappers::GetObjects();
    if (!objects || objects->GetNumElements() <= 1 || objects->GetObjectPtr(1) == nullptr)
        return {Error::InvalidEvidence, "The initialized profile has no usable object registry"};
    const auto firstObject = reinterpret_cast<std::uintptr_t>(objects->GetObjectPtr(1));
    const auto firstName = UE_UObject(reinterpret_cast<void*>(firstObject)).GetName();
    if (firstName.empty() || firstName == "None" || firstName.size() > 1024 || !validateUtf8(firstName))
        return {Error::InvalidEvidence, "The bounded name-pool contract did not resolve the first live UObject"};
    if (!g_UpstreamReadFailure) return g_UpstreamReadFailure;
    if (auto lease = g_Reader->validateLease(); !lease) return lease;
    result.GameName = selected->AsGameProfile()->GetAppName();
    result.PackageName = package;
    result.Success = true;
    return {};
#endif
}

andueprober::Status FullSdkExportAdmission() {
    if (!g_SelectedProfile || !g_Reader || !g_ObjectDiscovery.address || !g_NameDiscovery.address)
        return {andueprober::Error::InvalidEvidence, "Full SDK export requires a current bounded profile and module lease"};
    if (!g_UpstreamReadFailure) return g_UpstreamReadFailure;
    return {};
}

andueprober::Status InspectTargetMemory(std::uintptr_t address, std::uint32_t size,
    andueprober::Snapshot& snapshot) {
    using namespace andueprober;
    std::lock_guard lock(g_AdapterMutex);
#if !ANDUEPROBER_HAS_PROCESS_MEMORY
    (void)address; (void)size; (void)snapshot;
    return {Error::Unsupported, "Memory inspection requires the explicit Memory dependency"};
#else
    if (!g_Reader || !address || !size || size > 512)
        return {Error::InvalidArgument, "Memory inspection requires an address and 1-512 bytes"};
    if (snapshot.moduleIdentity != g_Reader->identity() || snapshot.generation != g_Reader->generation())
        return {Error::StaleIdentity, "Memory inspection requires the current module lease"};
    if (auto lease = g_Reader->validateLease(); !lease) return lease;
    MemoryInspection observation;
    observation.address = address;
    observation.bytes.resize(size);
    observation.moduleIdentity = snapshot.moduleIdentity;
    observation.generation = snapshot.generation;
    ReadBudget budget;
    budget.remainingBytes = size;
    budget.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    budget.cancelled = g_Cancelled;
    budget.generation = snapshot.generation;
    if (auto status = readExact(*g_Reader, address,
        std::as_writable_bytes(std::span(observation.bytes)), budget); !status) return status;
    if (auto lease = g_Reader->validateLease(); !lease) return lease;
    snapshot.memoryInspection = std::move(observation);
    return {};
#endif
}

andueprober::Status RunAutomaticProfilePhase(int phase, andueprober::Snapshot& snapshot) {
    using namespace andueprober;
    std::lock_guard lock(g_AdapterMutex);
#if !ANDUEPROBER_HAS_PROCESS_MEMORY
    (void)phase; (void)snapshot;
    return {Error::Unsupported, "Automatic analysis requires the explicit Memory dependency"};
#else
    if (phase < 1 || phase > 6) return {Error::InvalidArgument, "The automatic phase is outside 1-6"};
    if (auto admission = FullSdkExportAdmission(); !admission) return admission;
    if (snapshot.moduleIdentity != g_Reader->identity() || snapshot.generation != g_Reader->generation())
        return {Error::StaleIdentity, "The automatic phase requires the current module lease"};
    if (auto lease = g_Reader->validateLease(); !lease) return lease;
    auto* offsets = g_SelectedProfile->AsGameProfile()->GetOffsets();
    if (!offsets) return {Error::InvalidEvidence, "The selected profile has no reflection layout"};
    g_UpstreamReadFailure = {};
    auto working = snapshot;
    const auto dependencies = dependencyNames(phase);
    const auto source = "bounded-live-profile:" + g_SelectedProfile->AsGameProfile()->GetAppName() +
        ";module:" + snapshot.moduleIdentity + ";generation:" + std::to_string(snapshot.generation);
    const auto publish = [&](const std::string& name, std::uintptr_t value,
        const std::vector<std::string>& samples) -> Status {
        return publishLiveOffset(working, name, value, samples, dependencies, source + ";phase:" + std::to_string(phase));
    };

    if (phase == 1) {
        std::vector<ObjectAnchor> anchors;
        auto status = collectObjects(65536, [&](const ObjectAnchor& anchor) {
            std::int32_t observed = -1;
            if (auto read = readValue(anchor.address + offsets->UObject.InternalIndex, observed); !read) return false;
            return observed == static_cast<std::int32_t>(anchor.index);
        }, anchors);
        if (!status) return status;
        if (anchors.size() < 3) return {Error::InvalidEvidence, "Phase 1 requires three indexed live UObject anchors"};
        anchors.resize(std::min<std::size_t>(anchors.size(), 8));
        const auto samples = identities(anchors);
        for (const auto& [name, value] : std::array<std::pair<const char*, std::uintptr_t>, 5>{{
            {"UObject::InternalIndex", offsets->UObject.InternalIndex},
            {"UObject::NamePrivate", offsets->UObject.NamePrivate},
            {"UObject::ClassPrivate", offsets->UObject.ClassPrivate},
            {"UObject::OuterPrivate", offsets->UObject.OuterPrivate},
            {"UObject::ObjectFlags", offsets->UObject.ObjectFlags}}})
            if (auto published = publish(name, value, samples); !published) return published;
    } else if (phase == 2) {
        std::vector<ObjectAnchor> anchors;
        auto status = collectObjects(131072, [](const ObjectAnchor& anchor) {
            return anchor.className == "Class" || anchor.className == "ScriptStruct" ||
                anchor.className == "Function" || anchor.className == "BlueprintGeneratedClass";
        }, anchors);
        if (!status) return status;
        std::vector<ObjectAnchor> valid;
        for (const auto& anchor : anchors) {
            std::int32_t size = 0;
            std::uintptr_t super = 0;
            if (!readValue(anchor.address + offsets->UStruct.PropertiesSize, size) ||
                !readValue(anchor.address + offsets->UStruct.SuperStruct, super)) continue;
            if (size < 0 || size > 64 * 1024 * 1024 || (super && !UEMemory::kPtrValidator.isPtrReadable(super))) continue;
            valid.push_back(anchor);
            if (valid.size() == 12) break;
        }
        if (valid.size() < 3) {
            std::string detail = "Phase 2 requires three live UStruct relationship anchors (typed=" +
                std::to_string(anchors.size()) + ", coherent=" + std::to_string(valid.size()) + ")";
            for (std::size_t index = 0; index < std::min<std::size_t>(anchors.size(), 3); ++index)
                detail += "; " + anchors[index].identity;
            if (anchors.empty()) {
                std::vector<ObjectAnchor> observed;
                if (auto collected = collectObjects(256, [](const ObjectAnchor&) { return true; }, observed); !collected)
                    return collected;
                for (std::size_t index = 0; index < std::min<std::size_t>(observed.size(), 8); ++index)
                    detail += "; observed=" + observed[index].identity;
            }
            return {Error::InvalidEvidence, std::move(detail)};
        }
        const auto samples = identities(valid);
        for (const auto& [name, value] : std::array<std::pair<const char*, std::uintptr_t>, 5>{{
            {"UField::Next", offsets->UField.Next}, {"UStruct::SuperStruct", offsets->UStruct.SuperStruct},
            {"UStruct::Children", offsets->UStruct.Children},
            {"UStruct::ChildProperties", offsets->UStruct.ChildProperties},
            {"UStruct::PropertiesSize", offsets->UStruct.PropertiesSize}}})
            if (auto published = publish(name, value, samples); !published) return published;
    } else if (phase == 3) {
        std::vector<ObjectAnchor> anchors;
        auto status = collectObjects(131072, [](const ObjectAnchor& anchor) {
            return anchor.className == "Class" || anchor.className == "BlueprintGeneratedClass";
        }, anchors);
        if (!status) return status;
        std::vector<ObjectAnchor> valid;
        for (const auto& anchor : anchors) {
            std::uintptr_t defaultObject = 0;
            std::uint64_t castFlags = 0;
            if (!readValue(anchor.address + offsets->UClass.DefaultObject, defaultObject) ||
                !readValue(anchor.address + offsets->UClass.CastFlags, castFlags) || !defaultObject) continue;
            UE_UObject object(reinterpret_cast<void*>(defaultObject));
            if (reinterpret_cast<std::uintptr_t>(object.GetClass().GetAddress()) != anchor.address) continue;
            valid.push_back(anchor);
            if (valid.size() == 8) break;
        }
        if (valid.size() < 3) return {Error::InvalidEvidence, "Phase 3 requires three UClass/default-object ownership anchors"};
        const auto samples = identities(valid);
        if (auto status = publish("UClass::CastFlags", offsets->UClass.CastFlags, samples); !status) return status;
        if (auto status = publish("UClass::ClassDefaultObject", offsets->UClass.DefaultObject, samples); !status) return status;
    } else if (phase == 4) {
        std::vector<ObjectAnchor> anchors;
        auto status = collectObjects(196608, [](const ObjectAnchor& anchor) { return anchor.className == "Function"; }, anchors);
        if (!status) return status;
        std::vector<ObjectAnchor> valid;
        const auto returnOffset = offsets->UFunction.ParamSize + sizeof(std::uint16_t);
        for (const auto& anchor : anchors) {
            std::uint32_t flags = 0; std::uint8_t count = 0; std::uint16_t size = 0, result = 0;
            std::uintptr_t function = 0;
            if (!readValue(anchor.address + offsets->UFunction.EFunctionFlags, flags) ||
                !readValue(anchor.address + offsets->UFunction.NumParams, count) ||
                !readValue(anchor.address + offsets->UFunction.ParamSize, size) ||
                !readValue(anchor.address + returnOffset, result) ||
                !readValue(anchor.address + offsets->UFunction.Func, function)) continue;
            if (count > 64 || (result != 0xffff && result >= size) ||
                (function && !UEMemory::kPtrValidator.isPtrExecutable(function))) continue;
            valid.push_back(anchor);
            if (valid.size() == 12) break;
        }
        if (valid.size() < 3) return {Error::InvalidEvidence, "Phase 4 requires three coherent live UFunction anchors"};
        const auto samples = identities(valid);
        for (const auto& [name, value] : std::array<std::pair<const char*, std::uintptr_t>, 5>{{
            {"UFunction::FunctionFlags", offsets->UFunction.EFunctionFlags},
            {"UFunction::NumParms", offsets->UFunction.NumParams},
            {"UFunction::ParmsSize", offsets->UFunction.ParamSize},
            {"UFunction::ReturnValueOffset", returnOffset}, {"UFunction::Func", offsets->UFunction.Func}}})
            if (auto published = publish(name, value, samples); !published) return published;
    } else if (phase == 5) {
        struct PropertyAnchor {
            std::uintptr_t address, ownerAddress, classAddress, nextAddress;
            std::string name, className, identity;
        };
        std::vector<PropertyAnchor> properties;
        std::set<std::uintptr_t> seen;
        std::vector<ObjectAnchor> structures;
        auto status = collectObjects(1024 * 1024, [](const ObjectAnchor& anchor) {
            return anchor.className == "Class" || anchor.className == "ScriptStruct" ||
                anchor.className == "Function" || anchor.className == "BlueprintGeneratedClass" ||
                anchor.className == "Enum";
        }, structures);
        if (!status) return status;
        for (const auto& structure : structures) {
            if (structure.className == "Enum") continue;
            std::uintptr_t field = 0;
            if (!readValue(structure.address + offsets->UStruct.ChildProperties, field)) continue;
            for (std::size_t hop = 0; field && hop < 128 && properties.size() < 65536; ++hop) {
                if (!seen.insert(field).second || !UEMemory::kPtrValidator.isPtrReadable(field)) break;
                UE_FField wrapper(reinterpret_cast<std::uint8_t*>(field));
                auto fieldName = wrapper.GetName();
                auto fieldClass = wrapper.GetClass();
                auto className = fieldClass.GetName();
                if (!validText(fieldName) || !validText(className)) break;
                std::uintptr_t next = 0;
                if (!readValue(field + offsets->FField.Next, next)) break;
                properties.push_back({field, structure.address,
                    reinterpret_cast<std::uintptr_t>(fieldClass.GetAddress()), next, fieldName, className,
                    structure.identity + "/" + className + ":" + fieldName});
                field = next;
            }
            if (properties.size() >= 65536) break;
        }
        if (!g_UpstreamReadFailure) return g_UpstreamReadFailure;
        if (properties.size() < 3) return {Error::InvalidEvidence, "Phase 5 requires live FProperty chains"};
        std::unordered_map<std::uintptr_t, const ObjectAnchor*> structuresByAddress;
        for (const auto& structure : structures) structuresByAddress.emplace(structure.address, &structure);
        std::unordered_map<std::uintptr_t, std::string> propertyClassesByAddress;
        for (const auto& property : properties)
            if (property.classAddress) propertyClassesByAddress.emplace(property.classAddress, property.className);
        std::map<std::string, std::size_t> propertyClassCounts;
        for (const auto& property : properties) ++propertyClassCounts[property.className];
        auto discoverPointerField = [&](const char* fieldName, auto expected,
            std::uintptr_t& result, std::vector<std::string>& fieldSamples) -> Status {
            std::vector<std::pair<std::uintptr_t, std::vector<std::string>>> candidates;
            for (std::uintptr_t candidate = 8; candidate < 56; candidate += sizeof(void*)) {
                std::vector<std::string> matched;
                for (const auto& property : properties) {
                    const auto expectedValue = expected(property);
                    if (!expectedValue) continue;
                    std::uintptr_t observed = 0;
                    if (!readValue(property.address + candidate, observed) || observed != expectedValue) {
                        matched.clear(); break;
                    }
                    matched.push_back(property.identity);
                    if (matched.size() == 8) break;
                }
                if (matched.size() >= 3) candidates.emplace_back(candidate, std::move(matched));
            }
            if (candidates.size() != 1)
                return {Error::InvalidEvidence, std::string("Phase 5 requires one independently related offset for ") +
                    fieldName + " (candidates=" + std::to_string(candidates.size()) + ")"};
            result = candidates.front().first;
            fieldSamples = std::move(candidates.front().second);
            return {};
        };
        std::uintptr_t ownerOffset = 0, nextOffset = 0, classOffset = 0;
        std::vector<std::string> ownerSamples, nextSamples, classSamples;
        if (auto discovered = discoverPointerField("FField::Owner",
            [](const PropertyAnchor& property) { return property.ownerAddress; }, ownerOffset, ownerSamples); !discovered)
            return discovered;
        if (auto discovered = discoverPointerField("FField::Next",
            [](const PropertyAnchor& property) { return property.nextAddress; }, nextOffset, nextSamples); !discovered)
            return discovered;
        if (auto discovered = discoverPointerField("FField::ClassPrivate",
            [](const PropertyAnchor& property) { return property.classAddress; }, classOffset, classSamples); !discovered)
            return discovered;

        std::vector<std::pair<std::uintptr_t, std::vector<std::string>>> nameCandidates;
        for (std::uintptr_t candidate = 8; candidate < 56; candidate += alignof(std::int32_t)) {
            std::vector<std::string> matched;
            for (const auto& property : properties) {
                std::int32_t nameId = -1, number = 0;
                if (!readValue(property.address + candidate, nameId) ||
                    !readValue(property.address + candidate + sizeof(nameId), number) || nameId < 0 || number < 0 ||
                    (static_cast<std::uint32_t>(nameId) >> offsets->FNamePool.BlocksBit) >= 8192) {
                    matched.clear(); break;
                }
                std::uintptr_t block = 0;
                const auto blockIndex = static_cast<std::uint32_t>(nameId) >> offsets->FNamePool.BlocksBit;
                if (!readValue(*g_NameDiscovery.address + offsets->FNamePool.BlocksOff + blockIndex * sizeof(void*), block) ||
                    !block || !UEMemory::kPtrValidator.isPtrReadable(block)) {
                    matched.clear(); break;
                }
                auto observed = g_SelectedProfile->ResolveName(nameId);
                if (number > 0) observed += '_' + std::to_string(number - 1);
                if (observed != property.name) { matched.clear(); break; }
                matched.push_back(property.identity);
                if (matched.size() == 8) break;
            }
            if (matched.size() >= 3) nameCandidates.emplace_back(candidate, std::move(matched));
        }
        if (nameCandidates.size() != 1)
            return {Error::InvalidEvidence, "Phase 5 requires one independently named FField offset (candidates=" +
                std::to_string(nameCandidates.size()) + ")"};
        const auto nameOffset = nameCandidates.front().first;
        const auto nameSamples = std::move(nameCandidates.front().second);

        std::vector<std::string> samples;
        for (std::size_t index = 0; index < std::min<std::size_t>(properties.size(), 8); ++index)
            samples.push_back(properties[index].identity);
        for (const auto& [name, value] : std::array<std::pair<const char*, std::uintptr_t>, 5>{{
            {"FField::FlagsPrivate", offsets->FField.FlagsPrivate},
            {"FProperty::ArrayDim", offsets->FProperty.ArrayDim},
            {"FProperty::ElementSize", offsets->FProperty.ElementSize},
            {"FProperty::PropertyFlags", offsets->FProperty.PropertyFlags},
            {"FProperty::Offset_Internal", offsets->FProperty.Offset_Internal}}})
            if (auto published = publish(name, value, samples); !published) return published;
        for (const auto& [name, value, observations] :
            std::array<std::tuple<const char*, std::uintptr_t, const std::vector<std::string>*>, 4>{{
                {"FField::NamePrivate", nameOffset, &nameSamples}, {"FField::Owner", ownerOffset, &ownerSamples},
                {"FField::Next", nextOffset, &nextSamples}, {"FField::ClassPrivate", classOffset, &classSamples}}})
            if (auto published = publishLiveOffset(working, name, value, *observations, dependencies,
                source + ";phase:5;independent-field-relations", Origin::Probe); !published) return published;

        std::vector<PropertyPointerTailSample> pointerSamples;
        std::vector<PropertyBoolTailSample> boolSamples;
        std::set<std::uintptr_t> pointed;
        const auto scalarEnd = offsets->FProperty.Offset_Internal + sizeof(std::int32_t);
        const auto tailStart = (scalarEnd + sizeof(void*) - 1) & ~(sizeof(void*) - 1);
        constexpr std::uintptr_t tailWindow = 64;
        for (const auto& property : properties) {
            if ((property.className == "StructProperty" || property.className == "ObjectProperty" ||
                property.className == "ObjectPtrProperty" || property.className == "EncryptedObjectProperty") &&
                pointerSamples.size() < 8) {
                std::vector<std::pair<std::uintptr_t, std::uintptr_t>> relationships;
                for (std::uintptr_t candidate = tailStart; candidate <= tailStart + tailWindow; candidate += sizeof(void*)) {
                    std::uintptr_t target = 0;
                    if (!readValue(property.address + candidate, target) || !target ||
                        !UEMemory::kPtrValidator.isPtrReadable(target)) continue;
                    const auto related = structuresByAddress.find(target);
                    if (related == structuresByAddress.end()) continue;
                    const auto& relatedClass = related->second->className;
                    const auto expectedClass = property.className == "StructProperty" ? "ScriptStruct" : "Class";
                    if (relatedClass == expectedClass) relationships.emplace_back(candidate, target);
                }
                if (relationships.size() == 1 && pointed.insert(relationships.front().second).second)
                    pointerSamples.push_back({property.address, relationships.front().second,
                        property.identity + ";relationship-offset:" + std::to_string(relationships.front().first) +
                        ";target:" + std::to_string(relationships.front().second)});
            }
            if (property.className == "BoolProperty" && boolSamples.size() < 8) {
                std::vector<std::uintptr_t> matches;
                for (std::uintptr_t candidate = scalarEnd; candidate <= tailStart + tailWindow + 4; ++candidate) {
                    std::array<std::uint8_t, 4> bytes{};
                    if (readValue(property.address + candidate, bytes) &&
                        bytes == std::array<std::uint8_t, 4>{1, 0, 1, 0xff}) matches.push_back(candidate);
                }
                if (matches.size() == 1)
                    boolSamples.push_back({property.address, property.identity +
                        ";native-bool-offset:" + std::to_string(matches.front())});
            }
        }
        if (pointerSamples.size() < 2 || boolSamples.size() < 2)
            return {Error::InvalidEvidence, "Phase 5 requires two independent pointer-tail and NativeBool anchors "
                "(pointer=" + std::to_string(pointerSamples.size()) + ", NativeBool=" +
                std::to_string(boolSamples.size()) + ")"};
        const auto propertyIdentity = source + ";phase:5;property-scalars";
        for (const auto* field : {"FProperty::ArrayDim", "FProperty::ElementSize", "FProperty::PropertyFlags", "FProperty::Offset_Internal"})
            working.offsets.at(field).evidence.back().source = propertyIdentity;
        PropertyBaseProbeProfile profile{source + ";property-bases", snapshot.moduleIdentity,
            propertyIdentity, snapshot.generation, Layout::FField,
            256,
            BoolTailRepresentation::NativeBool};
        if (auto probed = probePropertyBases(*g_Reader, profile, pointerSamples, boolSamples, g_ReadBudget, working); !probed)
            return probed;

        std::vector<std::string> encryptedObjectSamples;
        for (const auto& property : properties) {
            if (property.className != "EncryptedObjectProperty") continue;
            std::uintptr_t target = 0;
            if (!readValue(property.address + *working.offsets.at("FProperty::SubPropertyBase").value, target)) continue;
            const auto related = structuresByAddress.find(target);
            if (related == structuresByAddress.end() || related->second->className != "Class") continue;
            encryptedObjectSamples.push_back(property.identity + ";class:" + related->second->identity);
        }
        if (propertyClassCounts.contains("EncryptedObjectProperty")) {
            if (encryptedObjectSamples.size() < 2)
                return {Error::InvalidEvidence, "EncryptedObjectProperty requires two object-class relationships"};
            auto& baseEvidence = working.offsets.at("FProperty::SubPropertyBase").evidence;
            baseEvidence.push_back({"EncryptedObjectProperty uses the validated object-property relationship", true,
                encryptedObjectSamples.size(), {*working.offsets.at("FProperty::SubPropertyBase").value},
                source + ";phase:5;encrypted-object-property", std::move(encryptedObjectSamples)});
        }

        const auto baseDependencies = std::vector<std::string>{"sizeof(FProperty)", "FProperty::SubPropertyBase"};
        const auto tailSource = source + ";phase:5;subclass-relations";
        const auto tailPublish = [&](const std::string& name, std::uintptr_t value,
            const std::vector<std::string>& tailSamples) -> Status {
            return publishLiveOffset(working, name, value, tailSamples, baseDependencies, tailSource, Origin::Probe);
        };
        auto findProperties = [&](const std::string& name) {
            std::vector<const PropertyAnchor*> result;
            for (const auto& property : properties) if (property.className == name) result.push_back(&property);
            return result;
        };
        auto pointerTail = [&](const std::string& className, bool requireProperty,
            std::uintptr_t& found, std::vector<std::string>& tailSamples) -> Status {
            found = UINTPTR_MAX; tailSamples.clear();
            const auto matchingProperties = findProperties(className);
            for (const auto* property : matchingProperties) {
                for (std::uintptr_t candidate = *working.offsets.at("sizeof(FProperty)").value;
                    candidate <= *working.offsets.at("FProperty::SubPropertyBase").value + 24; candidate += 8) {
                    std::uintptr_t value = 0;
                    if (!readValue(property->address + candidate, value) || !value) continue;
                    bool valid = UEMemory::kPtrValidator.isPtrReadable(value);
                    if (valid && requireProperty) {
                        std::uintptr_t fieldClass = 0;
                        valid = readValue(value + classOffset, fieldClass) &&
                            propertyClassesByAddress.contains(fieldClass);
                    } else if (valid) {
                        const auto object = structuresByAddress.find(value);
                        valid = object != structuresByAddress.end() && object->second->className == "Enum";
                    }
                    if (!valid) continue;
                    if (found != UINTPTR_MAX && found != candidate)
                        return {Error::InvalidEvidence, "Property subclass tail is ambiguous for " + className};
                    found = candidate;
                    tailSamples.push_back(property->identity);
                    break;
                }
                if (tailSamples.size() == 4) break;
            }
            if (found == UINTPTR_MAX || tailSamples.empty()) {
                std::string detail = "No live subclass tail was found for " + className + " (instances=" +
                    std::to_string(matchingProperties.size()) + ", properties=" + std::to_string(properties.size()) + ")";
                std::size_t reported = 0;
                for (const auto& [name, count] : propertyClassCounts) {
                    if (reported++ == 24) break;
                    detail += "; " + name + '=' + std::to_string(count);
                }
                return {Error::InvalidEvidence, std::move(detail)};
            }
            return {};
        };
        struct Tail { const char* className; const char* field; bool property; };
        for (const auto& tail : std::array<Tail, 5>{{
            {"EnumProperty", "FEnumProperty::UnderlyingType", true},
            {"EnumProperty", "FEnumProperty::Enum", false},
            {"ArrayProperty", "FArrayProperty::Inner", true},
            {"SetProperty", "FSetProperty::ElementProp", true},
            {"MapProperty", "FMapProperty::KeyProp", true}}}) {
            std::uintptr_t value = 0; std::vector<std::string> tailSamples;
            if (auto found = pointerTail(tail.className, tail.property, value, tailSamples); !found) return found;
            if (auto published = tailPublish(tail.field, value, tailSamples); !published) return published;
        }
        const auto mapKeyOffset = *working.offsets.at("FMapProperty::KeyProp").value;
        std::vector<std::string> mapValueSamples;
        for (const auto* property : findProperties("MapProperty")) {
            std::uintptr_t key = 0, value = 0, keyClass = 0, valueClass = 0;
            if (!readValue(property->address + mapKeyOffset, key) ||
                !readValue(property->address + mapKeyOffset + sizeof(void*), value) || !key || !value || key == value ||
                !readValue(key + classOffset, keyClass) ||
                !readValue(value + classOffset, valueClass) ||
                !propertyClassesByAddress.contains(keyClass) || !propertyClassesByAddress.contains(valueClass)) continue;
            mapValueSamples.push_back(property->identity + ";key:" + std::to_string(key) +
                ";value:" + std::to_string(value));
            if (mapValueSamples.size() == 4) break;
        }
        if (mapValueSamples.size() < 2)
            return {Error::InvalidEvidence, "MapProperty requires two distinct adjacent key/value relationships"};
        if (auto published = tailPublish("FMapProperty::ValueProp", mapKeyOffset + sizeof(void*), mapValueSamples); !published)
            return published;
        const auto boolIdentities = [&] { std::vector<std::string> values; for (const auto& item : boolSamples) values.push_back(item.identity); return values; }();
        const auto size = *working.offsets.at("sizeof(FProperty)").value;
        for (const auto& [name, value] : std::array<std::pair<const char*, std::uintptr_t>, 4>{{
            {"FBoolProperty::FieldSize", size}, {"FBoolProperty::ByteOffset", size + 1},
            {"FBoolProperty::ByteMask", size + 2}, {"FBoolProperty::FieldMask", size + 3}}})
            if (auto published = tailPublish(name, value, boolIdentities); !published) return published;
    } else {
        std::vector<ObjectAnchor> enums;
        auto status = collectObjects(196608, [](const ObjectAnchor& anchor) { return anchor.className == "Enum"; }, enums);
        if (!status) return status;
        std::vector<ObjectAnchor> valid;
        for (const auto& anchor : enums) {
            struct ArrayHeader { std::uintptr_t data; std::int32_t count, capacity; } header{};
            if (!readValue(anchor.address + offsets->UEnum.Names, header)) continue;
            if (!header.data || header.count <= 0 || header.capacity < header.count || header.capacity > 1024 * 1024 ||
                !UEMemory::kPtrValidator.isPtrReadable(header.data)) continue;
            valid.push_back(anchor);
            if (valid.size() == 8) break;
        }
        if (valid.size() < 2) return {Error::InvalidEvidence, "Phase 6 requires two populated live UEnum anchors"};
        const auto samples = identities(valid);
        if (auto published = publish("UEnum::Names", offsets->UEnum.Names, samples); !published) return published;

        std::vector<ObjectAnchor> objects;
        status = collectObjects(8192, [](const ObjectAnchor&) { return true; }, objects);
        if (!status) return status;
        std::uintptr_t address = 0; int index = -1; ObjectAnchor selected;
        for (const auto& object : objects) {
            if (g_SelectedProfile->AsGameProfile()->findProcessEvent(reinterpret_cast<std::uint8_t*>(object.address), &address, &index)) {
                selected = object; break;
            }
        }
        if (!address || index < 0 || index >= 100 || !UEMemory::kPtrValidator.isPtrExecutable(address))
            return {Error::InvalidEvidence, "ProcessEvent discovery produced no executable vtable candidate"};
        std::uintptr_t vtable = 0, slot = 0;
        if (!readValue(selected.address, vtable) || !readValue(vtable + static_cast<std::uintptr_t>(index) * sizeof(void*), slot) || slot != address)
            return {Error::InvalidEvidence, "ProcessEvent vtable ownership changed during validation"};
        std::array<std::uint32_t, 128> instructions{};
        if (!readValue(address, instructions)) return {Error::InvalidEvidence, "ProcessEvent instructions are unreadable"};
        std::set<std::int64_t> expected{static_cast<std::int64_t>(offsets->UObject.InternalIndex),
            static_cast<std::int64_t>(offsets->FUObjectItem.Size),
            static_cast<std::int64_t>(offsets->UFunction.EFunctionFlags + 1),
            static_cast<std::int64_t>(offsets->UFunction.EFunctionFlags + 2),
            static_cast<std::int64_t>(offsets->UStruct.PropertiesSize),
            static_cast<std::int64_t>(offsets->UFunction.ParamSize),
            static_cast<std::int64_t>(offsets->UStruct.ChildProperties ? offsets->UStruct.ChildProperties : offsets->UStruct.Children)};
        std::set<std::int64_t> observed;
        for (std::size_t item = 0; item < instructions.size(); ++item) {
            const auto instruction = KittyArm64::decodeInsn(instructions[item], address + item * 4);
            if (instruction.isValid() && expected.contains(instruction.immediate)) observed.insert(instruction.immediate);
        }
        if (observed.size() < 5)
            return {Error::InvalidEvidence, "ProcessEvent lacks the required independent reflection-layout instruction anchors"};
        std::vector<std::string> processSamples{selected.identity + ";executable:" + std::to_string(address) +
            ";instruction-anchors:" + std::to_string(observed.size())};
        if (auto published = publish("ProcessEvent::VTableIdx", static_cast<std::uint32_t>(index), processSamples); !published) return published;
        g_ProcessEventIndex = static_cast<std::uint32_t>(index);
        if (address >= g_ModuleImage.loadBias && address - g_ModuleImage.loadBias <= UINT32_MAX) {
            if (auto published = publish("ProcessEvent::Address", address - g_ModuleImage.loadBias, processSamples); !published) return published;
            g_ProcessEventRelative = static_cast<std::uint32_t>(address - g_ModuleImage.loadBias);
        }
    }
    if (!g_UpstreamReadFailure) return g_UpstreamReadFailure;
    if (auto lease = g_Reader->validateLease(); !lease) return lease;
    working.result = {};
    snapshot = std::move(working);
    return {};
#endif
}

andueprober::Status RunFullSdkDump(const andueprober::Snapshot& snapshot, const std::string& outputRoot,
    const std::atomic<bool>& cancelled, std::string& outputDirectory) {
    using namespace andueprober;
    std::lock_guard lock(g_AdapterMutex);
    outputDirectory.clear();
#if !ANDUEPROBER_HAS_PROCESS_MEMORY
    (void)snapshot; (void)outputRoot; (void)cancelled;
    return {Error::Unsupported, "Full SDK export requires the explicit Memory dependency"};
#else
    if (auto admission = FullSdkExportAdmission(); !admission) return admission;
    if (!std::filesystem::path(outputRoot).is_absolute())
        return {Error::InvalidArgument, "The SDK publication root must be absolute"};
    if (snapshot.moduleIdentity != g_Reader->identity() || snapshot.generation != g_Reader->generation())
        return {Error::StaleIdentity, "The SDK export requires the current module lease"};
    if (auto lease = g_Reader->validateLease(); !lease) return lease;
    if (!g_ProcessEventIndex || !g_ProcessEventRelative)
        return {Error::InvalidEvidence, "SDK export requires separately validated ProcessEvent discovery"};
    if (auto status = validateSnapshot(snapshot); !status) return status;
    const auto value = [&](const char* name) -> std::uintptr_t {
        const auto found = snapshot.offsets.find(name);
        return found == snapshot.offsets.end() || !found->second.value ? UINTPTR_MAX : *found->second.value;
    };
    auto offsets = *g_SelectedProfile->AsGameProfile()->GetOffsets();
    const auto assign = [&](const char* name, std::uintptr_t& field) -> Status {
        const auto observed = value(name);
        if (observed == UINTPTR_MAX) return {Error::InvalidEvidence, std::string("The SDK layout is missing ") + name};
        field = observed;
        return {};
    };
    for (const auto& item : std::array<std::pair<const char*, std::uintptr_t*>, 32>{{
        {"UObject::ObjectFlags", &offsets.UObject.ObjectFlags}, {"UObject::InternalIndex", &offsets.UObject.InternalIndex},
        {"UObject::ClassPrivate", &offsets.UObject.ClassPrivate}, {"UObject::NamePrivate", &offsets.UObject.NamePrivate},
        {"UObject::OuterPrivate", &offsets.UObject.OuterPrivate}, {"UField::Next", &offsets.UField.Next},
        {"UEnum::Names", &offsets.UEnum.Names}, {"UStruct::SuperStruct", &offsets.UStruct.SuperStruct},
        {"UStruct::Children", &offsets.UStruct.Children}, {"UStruct::ChildProperties", &offsets.UStruct.ChildProperties},
        {"UStruct::PropertiesSize", &offsets.UStruct.PropertiesSize}, {"UClass::CastFlags", &offsets.UClass.CastFlags},
        {"UClass::ClassDefaultObject", &offsets.UClass.DefaultObject},
        {"UFunction::FunctionFlags", &offsets.UFunction.EFunctionFlags}, {"UFunction::NumParms", &offsets.UFunction.NumParams},
        {"UFunction::ParmsSize", &offsets.UFunction.ParamSize}, {"UFunction::Func", &offsets.UFunction.Func},
        {"FField::NamePrivate", &offsets.FField.NamePrivate}, {"FField::Owner", &offsets.FField.Owner},
        {"FField::Next", &offsets.FField.Next}, {"FField::ClassPrivate", &offsets.FField.ClassPrivate},
        {"FField::FlagsPrivate", &offsets.FField.FlagsPrivate}, {"FProperty::ArrayDim", &offsets.FProperty.ArrayDim},
        {"FProperty::ElementSize", &offsets.FProperty.ElementSize}, {"FProperty::PropertyFlags", &offsets.FProperty.PropertyFlags},
        {"FProperty::Offset_Internal", &offsets.FProperty.Offset_Internal}, {"sizeof(FProperty)", &offsets.FProperty.Size},
        {"FProperty::SubPropertyBase", &offsets.FProperty.SubPropertyBase},
        {"FEnumProperty::UnderlyingType", &offsets.FEnumProperty.UnderlyingType}, {"FEnumProperty::Enum", &offsets.FEnumProperty.Enum},
        {"FArrayProperty::Inner", &offsets.FArrayProperty.Inner}, {"FSetProperty::ElementProp", &offsets.FSetProperty.ElementProp}}})
        if (auto status = assign(item.first, *item.second); !status) return status;
    if (auto status = assign("FMapProperty::KeyProp", offsets.FMapProperty.KeyProp); !status) return status;
    if (auto status = assign("FMapProperty::ValueProp", offsets.FMapProperty.ValueProp); !status) return status;
    g_SelectedProfile->SetProbedOffsets(offsets);
    g_SelectedProfile->BindRuntime(*g_ObjectDiscovery.address, *g_NameDiscovery.address);
    g_UpstreamReadFailure = {};
    UEDumper dumper;
    dumper.SetSDKMode(UEDumper::SDKMode::Both);
    dumper.SetVerifiedProcessEvent(g_ModuleImage.loadBias + *g_ProcessEventRelative,
        static_cast<int>(*g_ProcessEventIndex));
    if (!dumper.Init(g_SelectedProfile->AsGameProfile()))
        return {Error::InvalidEvidence, "The reflection dumper rejected the validated profile: " + dumper.GetLastError()};
    std::unordered_map<std::string, BufferFmt> buffers;
    if (!dumper.Dump(&buffers))
        return {Error::InvalidEvidence, "Reflection collection failed: " + dumper.GetLastError()};
    if (!g_UpstreamReadFailure) return g_UpstreamReadFailure;
    if (auto lease = g_Reader->validateLease(); !lease) return lease;
    const auto* cancellation = g_Cancelled ? g_Cancelled : &cancelled;
    if (cancellation->load()) return {Error::Cancelled, "SDK collection cancelled before publication"};
    const auto offsetsFile = buffers.find("Offsets.hpp");
    if (offsetsFile == buffers.end()) return {Error::InvalidEvidence, "The reflection dump omitted Offsets.hpp"};
    const auto declaration = "constexpr int32_t ProcessEventIndex = " + std::to_string(*g_ProcessEventIndex) + ";";
    if (offsetsFile->second.readView().find(declaration) == std::string_view::npos)
        return {Error::InvalidEvidence, "The SDK emitter did not preserve the validated ProcessEvent index"};
    if (!buffers.contains("AIOHeader.hpp") || !buffers.contains("SDK_A/SDK.hpp") || buffers.size() < 5)
        return {Error::InvalidEvidence, "The reflection dump is incomplete"};
    std::vector<ExportFile> files;
    files.reserve(buffers.size());
    for (const auto& [path, buffer] : buffers) {
        if (buffer.empty()) return {Error::InvalidEvidence, "The reflection dump contains an empty generated file: " + path};
        files.push_back({path, buffer.read()});
    }
    ExportOptions options;
    options.root = outputRoot;
    options.cancelled = cancellation;
    options.deadline = g_ReadBudget.deadline;
    options.toolRevision = build::revision;
    options.dependencyRevisions = {{"AndUEDumper", build::dumperRevision},
        {"AndSwapChainHook.Memory", std::string(build::swapRevision) + ";" + ProcessMemory::providerIdentity()},
        {"KittyMemoryEx", build::kittyRevision}};
    options.maximumFiles = 8192;
    options.maximumFileBytes = 64 * 1024 * 1024;
    options.maximumTotalBytes = 512 * 1024 * 1024;
    options.maximumManifestBytes = 16 * 1024 * 1024;
    options.maximumMetadataEntries = 65536;
    const auto publication = publishExport(snapshot, files, options);
    if (!publication.status) return publication.status;
    outputDirectory = publication.publishedDirectory.string();
    return {};
#endif
}

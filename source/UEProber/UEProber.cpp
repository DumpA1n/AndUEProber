#include "UEProber.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <thread>
#include <unistd.h>

// ============================================================
// Construction.
// ============================================================

UEProber::UEProber(std::string outputRoot) : m_OutputRoot(std::move(outputRoot)) {
    for (int i = 0; i < 7; ++i)
        m_PhaseStatus[i] = EPhaseStatus::NotStarted;
}

bool UEProber::HasConfirmed(const std::string& name) {
    const auto observed = m_CoreSnapshot.offsets.find(name);
    return observed != m_CoreSnapshot.offsets.end() && observed->second.value &&
        observed->second.validation == andueprober::Validation::Validated;
}

// ============================================================
// Logging.
// ============================================================

void UEProber::Log(const std::string& text) {
    m_Log.push_back(text);
    if (m_Log.size() > 500)
        m_Log.erase(m_Log.begin(), m_Log.begin() + 100);
}

void UEProber::LogError(const std::string& text) {
    Log("[ERR] " + text);
}

// ============================================================
// Phase 1: UObject fields.
// ============================================================

andueprober::Status UEProber::SetUserOverride(const std::string& name, std::uint32_t offset) {
    andueprober::Offset value;
    value.value = offset;
    value.origin = andueprober::Origin::User;
    value.validation = andueprober::Validation::Candidate;
    if (auto status = andueprober::publishOffset(m_CoreSnapshot, name, std::move(value)); !status) return status;
    m_UserOverrides[name] = offset;
    InvalidateDerivedState();
    return {};
}
void UEProber::InvalidateDerivedState() {
    for (auto& phase : m_PhaseStatus) phase = EPhaseStatus::NotStarted;
    m_ReflectionModel = EReflectionModel::Unknown;
}

void UEProber::ExecutePhase(int phase) {
    if (phase < 1 || phase > 6) return;
    if (ProbeCancelled()) { m_PhaseStatus[phase] = EPhaseStatus::Cancelled; return; }
    m_PhaseStatus[phase] = EPhaseStatus::InProgress;
    m_CoreSnapshot.result = {};
    static constexpr void (UEProber::*work[])() = {nullptr, &UEProber::Phase1_AutoProbe,
        &UEProber::Phase2_AutoProbe, &UEProber::Phase3_AutoProbe, &UEProber::Phase4_AutoProbe,
        &UEProber::Phase5_AutoProbe, &UEProber::Phase6_AutoProbe};
    (this->*work[phase])();
    static const std::vector<std::string> required[] = {{},
        {"UObject::InternalIndex", "UObject::NamePrivate", "UObject::ClassPrivate", "UObject::OuterPrivate", "UObject::ObjectFlags"},
        {"UField::Next", "UStruct::SuperStruct", "UStruct::Children", "UStruct::PropertiesSize"},
        {"UClass::CastFlags", "UClass::ClassDefaultObject"},
        {"UFunction::FunctionFlags", "UFunction::NumParms", "UFunction::ParmsSize", "UFunction::Func"},
        {"FField::NamePrivate", "FField::Owner", "FField::Next", "FField::ClassPrivate", "FField::FlagsPrivate",
            "FProperty::ArrayDim", "FProperty::ElementSize", "FProperty::PropertyFlags", "FProperty::Offset_Internal",
            "UFunction::ReturnValueOffset",
            "sizeof(FProperty)", "FProperty::SubPropertyBase", "FEnumProperty::UnderlyingType", "FEnumProperty::Enum",
            "FArrayProperty::Inner", "FSetProperty::ElementProp", "FMapProperty::KeyProp", "FMapProperty::ValueProp",
            "FBoolProperty::FieldSize", "FBoolProperty::ByteOffset", "FBoolProperty::ByteMask", "FBoolProperty::FieldMask"},
        {"ProcessEvent::VTableIdx", "UEnum::Names"}};
    const bool complete = m_GameDetected && static_cast<bool>(m_CoreSnapshot.result) && std::all_of(required[phase].begin(), required[phase].end(), [&](const auto& name) { return HasConfirmed(name); });
    m_PhaseStatus[phase] = ProbeCancelled() ? EPhaseStatus::Cancelled : complete ? EPhaseStatus::Completed : EPhaseStatus::Failed;
}

void UEProber::Phase1_AutoProbe() {
    m_CoreSnapshot.result = RunAutomaticProfilePhase(1, m_CoreSnapshot);
    if (!m_CoreSnapshot.result) LogError(m_CoreSnapshot.result.message);
}

// ============================================================
// Phase 2: UField and UStruct.
// ============================================================

andueprober::Status UEProber::RunConfiguredStructPhase(andueprober::MemoryReader& reader,
    const andueprober::StructProbeProfile& profile, std::span<const andueprober::StructSample> structures,
    std::span<const andueprober::StructFieldSample> fields, andueprober::ReadBudget& budget,
    andueprober::Snapshot& observation) {
    std::lock_guard lock(m_OwnerMutex);
    if (m_StructOperation || m_ClassOperation || m_FunctionOperation || (m_ExecutionThread != std::thread::id{} && m_ExecutionThread != std::this_thread::get_id()))
        return {andueprober::Error::Busy, "Struct probing requires the current operation owner"};
    m_ExecutionThread = std::this_thread::get_id();
    m_CoreSnapshot = observation;
    m_GameDetected = true;
    m_ReflectionModel = profile.layout == andueprober::Layout::UProperty ? EReflectionModel::UProperty :
        profile.layout == andueprober::Layout::FField ? EReflectionModel::FField : EReflectionModel::Unknown;
    StructOperation operation{reader, profile, structures, fields, budget};
    m_StructOperation = &operation;
    struct Release { StructOperation*& value; ~Release() { value = nullptr; } } release{m_StructOperation};
    andueprober::Command command;
    command.kind = andueprober::CommandKind::ProbePhase; command.phase = 2; command.generation = observation.generation;
    return ExecuteCommand(command, observation);
}
void UEProber::Phase2_AutoProbe() {
    if (!m_StructOperation) {
        m_CoreSnapshot.result = RunAutomaticProfilePhase(2, m_CoreSnapshot);
        if (!m_CoreSnapshot.result) LogError(m_CoreSnapshot.result.message);
        return;
    }
    auto& operation = *m_StructOperation;
    m_CoreSnapshot.result = andueprober::probeStructFields(operation.reader, operation.profile,
        operation.structures, operation.fields, operation.budget, m_CoreSnapshot);
    if (!m_CoreSnapshot.result) LogError(m_CoreSnapshot.result.message);
}

// ============================================================
// Phase 3: UClass fields.
// ============================================================

andueprober::Status UEProber::RunConfiguredClassPhase(andueprober::MemoryReader& reader,
    const andueprober::ClassProbeProfile& profile, std::span<const andueprober::ClassSample> samples,
    andueprober::ReadBudget& budget, andueprober::Snapshot& observation) {
    std::lock_guard lock(m_OwnerMutex);
    if (m_StructOperation || m_ClassOperation || m_FunctionOperation ||
        (m_ExecutionThread != std::thread::id{} && m_ExecutionThread != std::this_thread::get_id()))
        return {andueprober::Error::Busy, "Class probing requires the current operation owner"};
    m_ExecutionThread = std::this_thread::get_id();
    m_CoreSnapshot = observation;
    ClassOperation operation{reader, profile, samples, budget};
    m_ClassOperation = &operation;
    struct Release { ClassOperation*& value; ~Release() { value = nullptr; } } release{m_ClassOperation};
    andueprober::Command command;
    command.kind = andueprober::CommandKind::ProbePhase; command.phase = 3; command.generation = observation.generation;
    return ExecuteCommand(command, observation);
}
void UEProber::Phase3_AutoProbe() {
    if (!m_ClassOperation) {
        m_CoreSnapshot.result = RunAutomaticProfilePhase(3, m_CoreSnapshot);
        if (!m_CoreSnapshot.result) LogError(m_CoreSnapshot.result.message);
        return;
    }
    auto& operation = *m_ClassOperation;
    m_CoreSnapshot.result = andueprober::probeClassFields(operation.reader, operation.profile,
        operation.samples, operation.budget, m_CoreSnapshot);
    if (!m_CoreSnapshot.result) LogError(m_CoreSnapshot.result.message);
}

// ============================================================
// Phase 4: UFunction fields.
// ============================================================

andueprober::Status UEProber::RunConfiguredFunctionPhase(andueprober::MemoryReader& reader,
    const andueprober::FunctionProbeProfile& profile, std::span<const andueprober::FunctionSample> samples,
    andueprober::ReadBudget& budget, andueprober::Snapshot& observation) {
    std::lock_guard lock(m_OwnerMutex);
    if (m_StructOperation || m_ClassOperation || m_FunctionOperation ||
        (m_ExecutionThread != std::thread::id{} && m_ExecutionThread != std::this_thread::get_id()))
        return {andueprober::Error::Busy, "Function probing requires the current operation owner"};
    m_ExecutionThread = std::this_thread::get_id();
    m_CoreSnapshot = observation;
    FunctionOperation operation{reader, profile, samples, budget};
    m_FunctionOperation = &operation;
    struct Release { FunctionOperation*& value; ~Release() { value = nullptr; } } release{m_FunctionOperation};
    andueprober::Command command;
    command.kind = andueprober::CommandKind::ProbePhase; command.phase = 4; command.generation = observation.generation;
    return ExecuteCommand(command, observation);
}
void UEProber::Phase4_AutoProbe() {
    if (!m_FunctionOperation) {
        m_CoreSnapshot.result = RunAutomaticProfilePhase(4, m_CoreSnapshot);
        if (!m_CoreSnapshot.result) LogError(m_CoreSnapshot.result.message);
        return;
    }
    auto& operation = *m_FunctionOperation;
    m_CoreSnapshot.result = andueprober::probeFunctionFields(operation.reader, operation.profile,
        operation.samples, operation.budget, m_CoreSnapshot);
    if (!m_CoreSnapshot.result) LogError(m_CoreSnapshot.result.message);
}

// ============================================================
// Phase 5: FField and FProperty.
// ============================================================

andueprober::Status UEProber::RunConfiguredFieldBasePhase(andueprober::MemoryReader& reader,
    const andueprober::FieldProbeProfile& profile, std::span<const andueprober::FieldBaseSample> samples,
    const andueprober::NameLayout& names, std::uintptr_t pool, const andueprober::NamePoolProfile& poolProfile,
    andueprober::ReadBudget& budget, andueprober::Snapshot& observation) {
    std::lock_guard lock(m_OwnerMutex);
    if (m_StructOperation || m_ClassOperation || m_FunctionOperation || m_ExecutionThread != std::this_thread::get_id())
        return {andueprober::Error::Busy, "FField base probing requires the current configured operation owner"};
    m_CoreSnapshot = observation;
    m_PhaseStatus[5] = EPhaseStatus::InProgress;
    const auto status = andueprober::probeFieldFields(reader, profile, samples, names, pool, poolProfile, budget, m_CoreSnapshot);
    m_CoreSnapshot.result = status;
    m_PhaseStatus[5] = status ? EPhaseStatus::Partial : EPhaseStatus::Failed;
    if (status) Log("FField base observations are complete; FProperty and container metadata require an independent operation");
    else LogError(status.message);
    observation = GetSnapshot();
    return status;
}

andueprober::Status UEProber::RunConfiguredPropertyPhase(andueprober::MemoryReader& reader,
    const andueprober::PropertyProbeProfile& profile, std::span<const andueprober::PropertySample> samples,
    const andueprober::NameLayout& names, std::uintptr_t pool, const andueprober::NamePoolProfile& poolProfile,
    andueprober::ReadBudget& budget, andueprober::Snapshot& observation) {
    std::lock_guard lock(m_OwnerMutex);
    if (m_StructOperation || m_ClassOperation || m_FunctionOperation || m_ExecutionThread != std::this_thread::get_id())
        return {andueprober::Error::Busy, "Property probing requires the current configured operation owner"};
    m_CoreSnapshot = observation;
    m_PhaseStatus[5] = EPhaseStatus::InProgress;
    const auto status = andueprober::probePropertyFields(reader, profile, samples, names, pool, poolProfile, budget, m_CoreSnapshot);
    m_CoreSnapshot.result = status;
    m_PhaseStatus[5] = status ? EPhaseStatus::Partial : EPhaseStatus::Failed;
    if (status) Log("FProperty scalar observations are complete; subclass and container metadata require an independent operation");
    else LogError(status.message);
    observation = GetSnapshot();
    return status;
}

void UEProber::Phase5_AutoProbe() {
    m_CoreSnapshot.result = RunAutomaticProfilePhase(5, m_CoreSnapshot);
    if (m_CoreSnapshot.result) m_ReflectionModel = EReflectionModel::FField;
    else LogError(m_CoreSnapshot.result.message);
}

andueprober::Status UEProber::RunConfiguredPropertyTailPhase(andueprober::MemoryReader& reader,
    const andueprober::PropertyTailProfile& profile, std::span<const andueprober::PropertyTailSample> samples,
    const andueprober::NameLayout& names, std::uintptr_t pool, const andueprober::NamePoolProfile& poolProfile,
    andueprober::ReadBudget& budget, andueprober::Snapshot& observation) {
    std::lock_guard lock(m_OwnerMutex);
    if (m_StructOperation || m_ClassOperation || m_FunctionOperation || m_ExecutionThread != std::this_thread::get_id())
        return {andueprober::Error::Busy, "Property-tail probing requires the current configured operation owner"};
    m_CoreSnapshot = observation;
    m_PhaseStatus[5] = EPhaseStatus::InProgress;
    const auto status = andueprober::probePropertyTails(reader, profile, samples, names, pool, poolProfile, budget, m_CoreSnapshot);
    m_CoreSnapshot.result = status;
    m_PhaseStatus[5] = status ? EPhaseStatus::Partial : EPhaseStatus::Failed;
    if (status) Log("Declared property pointers are observed; complete container layouts require independent metadata");
    else LogError(status.message);
    observation = GetSnapshot();
    return status;
}

andueprober::Status UEProber::RunConfiguredBoolPropertyPhase(andueprober::MemoryReader& reader,
    const andueprober::BoolPropertyProfile& profile, std::span<const andueprober::BoolPropertySample> samples,
    const andueprober::NameLayout& names, std::uintptr_t pool, const andueprober::NamePoolProfile& poolProfile,
    andueprober::ReadBudget& budget, andueprober::Snapshot& observation) {
    std::lock_guard lock(m_OwnerMutex);
    if (m_StructOperation || m_ClassOperation || m_FunctionOperation || m_ExecutionThread != std::this_thread::get_id())
        return {andueprober::Error::Busy, "Property metadata probing requires the current configured operation owner"};
    m_CoreSnapshot = observation;
    m_PhaseStatus[5] = EPhaseStatus::InProgress;
    const auto status = andueprober::probeBoolProperty(reader, profile, samples, names, pool, poolProfile, budget, m_CoreSnapshot);
    m_CoreSnapshot.result = status;
    m_PhaseStatus[5] = status ? EPhaseStatus::Partial : EPhaseStatus::Failed;
    if (status) Log("Declared property metadata is observed; complete Phase 5 requires independent subclass contracts");
    else LogError(status.message);
    observation = GetSnapshot();
    return status;
}

andueprober::Status UEProber::RunConfiguredFieldPathPropertyPhase(andueprober::MemoryReader& reader,
    const andueprober::FieldPathPropertyProfile& profile, std::span<const andueprober::FieldPathPropertySample> samples,
    const andueprober::NameLayout& names, std::uintptr_t pool, const andueprober::NamePoolProfile& poolProfile,
    andueprober::ReadBudget& budget, andueprober::Snapshot& observation) {
    std::lock_guard lock(m_OwnerMutex);
    if (m_StructOperation || m_ClassOperation || m_FunctionOperation || m_ExecutionThread != std::this_thread::get_id())
        return {andueprober::Error::Busy, "Property metadata probing requires the current configured operation owner"};
    m_CoreSnapshot = observation;
    m_PhaseStatus[5] = EPhaseStatus::InProgress;
    const auto status = andueprober::probeFieldPathProperty(reader, profile, samples, names, pool, poolProfile, budget, m_CoreSnapshot);
    m_CoreSnapshot.result = status;
    m_PhaseStatus[5] = status ? EPhaseStatus::Partial : EPhaseStatus::Failed;
    if (status) Log("Declared property metadata is observed; complete Phase 5 requires independent subclass contracts");
    else LogError(status.message);
    observation = GetSnapshot();
    return status;
}

andueprober::Status UEProber::RunConfiguredEnumPhase(andueprober::MemoryReader& reader,
    const andueprober::EnumProbeProfile& profile, std::span<const andueprober::EnumSample> samples,
    const andueprober::NameLayout& names, std::uintptr_t pool, const andueprober::NamePoolProfile& poolProfile,
    andueprober::ReadBudget& budget, andueprober::Snapshot& observation) {
    std::lock_guard lock(m_OwnerMutex);
    if (m_StructOperation || m_ClassOperation || m_FunctionOperation || m_ExecutionThread != std::this_thread::get_id())
        return {andueprober::Error::Busy, "Enum probing requires the current configured operation owner"};
    m_CoreSnapshot = observation;
    m_PhaseStatus[6] = EPhaseStatus::InProgress;
    const auto status = andueprober::probeEnumNames(reader, profile, samples, names, pool, poolProfile, budget, m_CoreSnapshot);
    m_CoreSnapshot.result = status;
    m_PhaseStatus[6] = status ? EPhaseStatus::Partial : EPhaseStatus::Failed;
    if (status) Log("UEnum names are observed; ProcessEvent requires a separately verified engine contract");
    else LogError(status.message);
    observation = GetSnapshot();
    return status;
}

void UEProber::Phase6_AutoProbe() {
    m_CoreSnapshot.result = RunAutomaticProfilePhase(6, m_CoreSnapshot);
    if (!m_CoreSnapshot.result) LogError(m_CoreSnapshot.result.message);
}

andueprober::Status UEProber::ExecuteCommand(const andueprober::Command& command, andueprober::Snapshot& observation) {
    std::lock_guard lock(m_OwnerMutex);
    using namespace andueprober;
    if (m_ExecutionThread != std::thread::id{} && m_ExecutionThread != std::this_thread::get_id())
        return {Error::Busy, "The inspector belongs to another command worker"};
    m_ExecutionThread = std::this_thread::get_id();
    Status result;
    if (command.kind != CommandKind::Detect && (!m_GameDetected || command.generation != m_CoreSnapshot.generation))
        return {Error::StaleIdentity, "The command requires the current initialized profile generation"};
    switch (command.kind) {
    case CommandKind::Detect:
        DetectGame();
        if (!m_GameDetected) result = m_CoreSnapshot.result;
        break;
    case CommandKind::ProbePhase:
    case CommandKind::ProbeAll:
        for (int phase = command.kind == CommandKind::ProbePhase ? command.phase : 1;
            phase <= (command.kind == CommandKind::ProbePhase ? command.phase : 6); ++phase) {
            ExecutePhase(phase);
            if (m_PhaseStatus[phase] != EPhaseStatus::Completed) {
                result = !m_CoreSnapshot.result ? m_CoreSnapshot.result : Status{
                    ProbeCancelled() ? Error::Cancelled : Error::InvalidEvidence,
                    "Phase " + std::to_string(phase) + " lacks complete offset evidence"};
                break;
            }
        }
        break;
    case CommandKind::SetOverride: {
        if (!command.value || *command.value > INT32_MAX || command.field.empty())
            return {Error::InvalidArgument, "An override requires a field and bounded value"};
        if (!m_CoreSnapshot.offsets.contains(command.field) && m_CoreSnapshot.offsets.size() >= 256)
            return {Error::Busy, "The inspector offset capacity is exhausted"};
        if (auto status = SetUserOverride(command.field, *command.value); !status) return status;
        break;
    }
    case CommandKind::ClearOverride: {
        const auto existing = m_CoreSnapshot.offsets.find(command.field);
        if (existing == m_CoreSnapshot.offsets.end() || existing->second.origin != Origin::User)
            return {Error::InvalidArgument, "The field has no explicit user override"};
        auto stale = existing->second; stale.validation = Validation::Stale;
        if (auto status = publishOffset(m_CoreSnapshot, command.field, std::move(stale)); !status) return status;
        m_CoreSnapshot.offsets.at(command.field).origin = Origin::Probe;
        m_UserOverrides.erase(command.field);
        InvalidateDerivedState();
        break;
    }
    case CommandKind::ClearResults:
        std::erase_if(m_CoreSnapshot.offsets, [](const auto& item) { return item.second.origin != Origin::User; });
        m_CoreSnapshot.fieldReports.clear();
        m_CoreSnapshot.memoryInspection.reset();
        InvalidateDerivedState();
        m_Log.clear();
        break;
    case CommandKind::Export:
        StartDump();
        if (m_DumpStatus.load() != EDumpStatus::Success)
            result = !m_CoreSnapshot.result ? m_CoreSnapshot.result : Status{ProbeCancelled() ? Error::Cancelled : Error::InvalidEvidence, m_DumpError};
        break;
    case CommandKind::InspectMemory:
        result = InspectTargetMemory(command.address, command.size, m_CoreSnapshot);
        break;
    default:
        return {Error::InvalidArgument, "Unsupported inspector command"};
    }
    observation = GetSnapshot();
    return result;
}

void UEProber::RunAutoDumpFlow() {
    std::lock_guard lock(m_OwnerMutex);
    if (m_ExecutionThread != std::thread::id{} && m_ExecutionThread != std::this_thread::get_id()) {
        m_DumpError = "The inspector belongs to another worker";
        m_DumpStatus.store(EDumpStatus::Failed);
        return;
    }
    m_ExecutionThread = std::this_thread::get_id();

    DetectGame();
    if (!m_GameDetected) {
        m_DumpStatus.store(EDumpStatus::Failed);
        return;
    }

    for (int phase = 1; phase <= 6; ++phase) {
        ExecutePhase(phase);
        if (m_PhaseStatus[phase] != EPhaseStatus::Completed) {
            m_DumpError = "Probe phase " + std::to_string(phase) + " did not complete";
            m_DumpStatus.store(ProbeCancelled() ? EDumpStatus::Cancelled : EDumpStatus::Failed);
            return;
        }
    }
    if (ProbeCancelled()) { m_DumpStatus.store(EDumpStatus::Cancelled); return; }

    StartDump();
}

void UEProber::DetectGame() {
    if (m_GameDetected) return;

    GameDetectionResult result;
    const auto detection = DetectAndPrepareGame(result);
    if (!detection) {
        m_CoreSnapshot.result = detection;
        m_DumpError = detection.message;
        return;
    }

    m_GameDetected = true;
    m_CoreSnapshot = {};
    m_CoreSnapshot.sessionId = "profile-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    CaptureProbeIdentity(m_CoreSnapshot);
    for (const auto& [name, override] : m_UserOverrides) {
        andueprober::Offset value;
        value.value = override;
        value.origin = andueprober::Origin::User;
        andueprober::publishOffset(m_CoreSnapshot, name, std::move(value));
    }
}

void UEProber::StartDump() {
    m_DumpOutputDir.clear();
    m_DumpError.clear();
    if (m_OutputRoot.empty()) {
        m_CoreSnapshot.result = {andueprober::Error::InvalidArgument, "The SDK publication root is not configured"};
    } else {
        static const std::atomic<bool> neverCancelled{false};
        m_CoreSnapshot.result = RunFullSdkDump(m_CoreSnapshot, m_OutputRoot, neverCancelled, m_DumpOutputDir);
    }
    if (m_CoreSnapshot.result) {
        m_DumpStatus.store(EDumpStatus::Success);
        Log("Full SDK published to " + m_DumpOutputDir);
    } else {
        m_DumpError = m_CoreSnapshot.result.message;
        m_DumpStatus.store(ProbeCancelled() ? EDumpStatus::Cancelled : EDumpStatus::Failed);
        LogError(m_DumpError);
    }
}

andueprober::Snapshot UEProber::GetSnapshot() const {
    std::lock_guard lock(m_OwnerMutex);
    auto snapshot = m_CoreSnapshot;
    snapshot.messages = m_Log;
    snapshot.layout = m_ReflectionModel == EReflectionModel::UProperty ? andueprober::Layout::UProperty :
        m_ReflectionModel == EReflectionModel::FField ? andueprober::Layout::FField : andueprober::Layout::Unknown;
    return snapshot;
}

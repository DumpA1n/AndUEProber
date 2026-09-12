#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "andueprober/Commands.hpp"
#include "andueprober/Structs.hpp"
#include "andueprober/Classes.hpp"
#include "andueprober/Functions.hpp"
#include "andueprober/Fields.hpp"
#include "andueprober/Properties.hpp"
#include "andueprober/Enums.hpp"
#include "andueprober/PropertyTails.hpp"
#include "andueprober/PropertyValues.hpp"
#include "DumperBridge.h"

// ============================================================
// UEProber: UE reflection-layout analysis.
// ============================================================

class UEProber {
public:
    explicit UEProber(std::string outputRoot = {});
    ~UEProber() = default;
    UEProber(const UEProber&) = delete;
    UEProber& operator=(const UEProber&) = delete;

    andueprober::Status ExecuteCommand(const andueprober::Command&, andueprober::Snapshot&);
    // Synchronous owner operation. Reader, budget and metadata remain borrowed only during this call.
    andueprober::Status RunConfiguredStructPhase(andueprober::MemoryReader&,
        const andueprober::StructProbeProfile&, std::span<const andueprober::StructSample>,
        std::span<const andueprober::StructFieldSample>, andueprober::ReadBudget&, andueprober::Snapshot&);
    andueprober::Status RunConfiguredClassPhase(andueprober::MemoryReader&,
        const andueprober::ClassProbeProfile&, std::span<const andueprober::ClassSample>,
        andueprober::ReadBudget&, andueprober::Snapshot&);
    andueprober::Status RunConfiguredFunctionPhase(andueprober::MemoryReader&,
        const andueprober::FunctionProbeProfile&, std::span<const andueprober::FunctionSample>,
        andueprober::ReadBudget&, andueprober::Snapshot&);
    andueprober::Status RunConfiguredFieldBasePhase(andueprober::MemoryReader&,
        const andueprober::FieldProbeProfile&, std::span<const andueprober::FieldBaseSample>,
        const andueprober::NameLayout&, std::uintptr_t pool, const andueprober::NamePoolProfile&,
        andueprober::ReadBudget&, andueprober::Snapshot&);
    andueprober::Status RunConfiguredPropertyPhase(andueprober::MemoryReader&,
        const andueprober::PropertyProbeProfile&, std::span<const andueprober::PropertySample>,
        const andueprober::NameLayout&, std::uintptr_t pool, const andueprober::NamePoolProfile&,
        andueprober::ReadBudget&, andueprober::Snapshot&);
    andueprober::Status RunConfiguredPropertyTailPhase(andueprober::MemoryReader&,
        const andueprober::PropertyTailProfile&, std::span<const andueprober::PropertyTailSample>,
        const andueprober::NameLayout&, std::uintptr_t, const andueprober::NamePoolProfile&,
        andueprober::ReadBudget&, andueprober::Snapshot&);
    andueprober::Status RunConfiguredBoolPropertyPhase(andueprober::MemoryReader&,
        const andueprober::BoolPropertyProfile&, std::span<const andueprober::BoolPropertySample>,
        const andueprober::NameLayout&, std::uintptr_t, const andueprober::NamePoolProfile&,
        andueprober::ReadBudget&, andueprober::Snapshot&);
    andueprober::Status RunConfiguredFieldPathPropertyPhase(andueprober::MemoryReader&,
        const andueprober::FieldPathPropertyProfile&, std::span<const andueprober::FieldPathPropertySample>,
        const andueprober::NameLayout&, std::uintptr_t, const andueprober::NamePoolProfile&,
        andueprober::ReadBudget&, andueprober::Snapshot&);
    andueprober::Status RunConfiguredEnumPhase(andueprober::MemoryReader&,
        const andueprober::EnumProbeProfile&, std::span<const andueprober::EnumSample>,
        const andueprober::NameLayout&, std::uintptr_t, const andueprober::NamePoolProfile&,
        andueprober::ReadBudget&, andueprober::Snapshot&);

    // Explicit synchronous workflow; the composition root owns its worker and cancellation.
    void RunAutoDumpFlow();

    andueprober::Snapshot GetSnapshot() const;
    EDumpStatus GetDumpStatus() const { return m_DumpStatus.load(); }
    std::string GetDumpError() const { std::lock_guard lock(m_OwnerMutex); return m_DumpError; }
    std::string GetDumpOutputDir() const { std::lock_guard lock(m_OwnerMutex); return m_DumpOutputDir; }

    // Phase state.
    enum class EPhaseStatus {
        NotStarted,
        InProgress,
        Completed,
        Partial,
        Failed,
        Cancelled,
    };

private:
    struct StructOperation {
        andueprober::MemoryReader& reader;
        const andueprober::StructProbeProfile& profile;
        std::span<const andueprober::StructSample> structures;
        std::span<const andueprober::StructFieldSample> fields;
        andueprober::ReadBudget& budget;
    };
    StructOperation* m_StructOperation = nullptr;
    struct ClassOperation {
        andueprober::MemoryReader& reader;
        const andueprober::ClassProbeProfile& profile;
        std::span<const andueprober::ClassSample> samples;
        andueprober::ReadBudget& budget;
    };
    ClassOperation* m_ClassOperation = nullptr;
    struct FunctionOperation {
        andueprober::MemoryReader& reader;
        const andueprober::FunctionProbeProfile& profile;
        std::span<const andueprober::FunctionSample> samples;
        andueprober::ReadBudget& budget;
    };
    FunctionOperation* m_FunctionOperation = nullptr;
    mutable std::recursive_mutex m_OwnerMutex;
    std::thread::id m_ExecutionThread;

    // Phase 1: UObject fields.

    void ExecutePhase(int phase);
    andueprober::Status SetUserOverride(const std::string&, std::uint32_t);
    void InvalidateDerivedState();
    void Phase1_AutoProbe();
    // Phase 2: UField and UStruct.

    void Phase2_AutoProbe();
    // Phase 3: UClass.

    void Phase3_AutoProbe();

    // Phase 4: UFunction.

    void Phase4_AutoProbe();
    // Phase 5: FField and FProperty.

    void Phase5_AutoProbe();
    // Phase 6: UEnum and ProcessEvent.

    void Phase6_AutoProbe();

    void DetectGame();
    void StartDump();


    // Completion requires a validated Core observation.
    bool HasConfirmed(const std::string& name);

    // State.

    // Phase state.
    EPhaseStatus m_PhaseStatus[7] = {};

    // Values explicitly supplied by the user survive profile discovery.
    std::map<std::string, std::uint32_t> m_UserOverrides;

    // Logs.
    std::vector<std::string> m_Log;
    void Log(const std::string& text);
    void LogError(const std::string& text);

    // Dump state.
    std::atomic<EDumpStatus> m_DumpStatus{EDumpStatus::Idle};
    std::string m_DumpError;
    std::string m_DumpOutputDir;
    std::string m_OutputRoot;

    // Profile-detection state.
    andueprober::Snapshot m_CoreSnapshot;
    bool m_GameDetected = false;

    // Reflection model: UObject-derived properties on Children, or FField properties
    // on ChildProperties. The configured profile supplies this layout explicitly.
    enum class EReflectionModel { Unknown, UProperty, FField };
    EReflectionModel m_ReflectionModel = EReflectionModel::Unknown;
};

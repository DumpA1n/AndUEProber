#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "DumperBridge.h"

// ============================================================
// UEProber: UE reflection-layout analysis.
// ============================================================

class UEProber {
public:
    static UEProber& GetInstance() {
        static UEProber instance;
        return instance;
    }

    UEProber(const UEProber&) = delete;
    UEProber& operator=(const UEProber&) = delete;

    void Draw(bool* p_open = nullptr);

    // Headless one-shot orchestration: DetectGame → Phase1~6 AutoProbe → StartDump.
    // For automated validation paths that bypass the ImGui UI (e.g. DFM filters
    // injected touch input). Returns immediately; StartDump runs on a detached
    // worker. Poll status via the getters below.
    void RunAutoDumpFlow();

    EDumpStatus GetDumpStatus() const { return m_DumpStatus.load(); }
    const std::string& GetDumpError() const { return m_DumpError; }
    const std::string& GetDumpOutputDir() const { return m_DumpOutputDir; }

    // Probe result data.

    // Result for one offset.
    struct OffsetResult {
        std::string name;          // Field name, such as UObject::Index.
        int32_t     offset = -1;   // Candidate offset; -1 means unresolved.
        int32_t     size = 0;      // Field width.
        std::string typeName;      // Type name.
        std::string evidence;      // Evidence description.
        bool        confirmed = false; // Confirmation flag; some probe paths set it automatically.
        bool        autoDetected = false; // Automatic detection flag.
    };

    // Phase state.
    enum class EPhaseStatus {
        NotStarted,
        InProgress,
        Completed,
        Failed,
    };

    // Scan candidate.
    struct ScanCandidate {
        int32_t     offset;
        uint64_t    rawValue;
        std::string description;
        float       confidence;    // 0.0 ~ 1.0
    };

private:
    UEProber();
    ~UEProber() = default;

    // Memory access.

    bool TryReadFName(uintptr_t address, std::string& outName);
    bool TryGetFullName(uintptr_t objAddr, std::string& outFullName);
    bool IsValidPtr(uintptr_t ptr);

    // Find an object by name, optionally filtering Class->Name first.
    uintptr_t FindObjectInGObjects(const std::string& targetName, const std::string& className = "");

    // Find a named UFunction along Children->Next.
    uintptr_t WalkChildrenChain(uintptr_t classAddr, const std::string& funcName,
                                int32_t childrenOff, int32_t nextOff, int32_t namePrivateOff);

    // Get structure size by reflected type name or address.
    int32_t GetStructSize(const std::string& structName);
    int32_t GetStructSize(uintptr_t structAddr);

    // Phase 1: UObject fields.

    void Phase1_AutoProbe();
    void Phase1_ProbeInternalIndex(uintptr_t objAddr, int32_t expectedIndex);
    void Phase1_ProbeNamePrivate(uintptr_t objAddr, const std::string& expectedName);
    void Phase1_ProbeClassPrivate(uintptr_t objAddr, const std::string& expectedClassName);
    void Phase1_ProbeOuterPrivate(uintptr_t obj2Addr, uintptr_t obj1Addr);
    void Phase1_ProbeObjectFlags(uintptr_t objAddr);

    // Phase 2: UField and UStruct.

    void Phase2_AutoProbe();
    // Discriminate the reflection model: ≤UE4.24 keeps properties as UObject-derived
    // UProperty on UStruct::Children; UE4.25+ moved them to FField/FProperty on
    // UStruct::ChildProperties. Detected via the presence of a non-CDO *Property
    // *instance* in GObjects — only ≤4.24 makes property instances UObjects (4.25+
    // keeps the property UClass + its CDO in both, so the class alone can't
    // discriminate). Gates the Phase2 ChildProperties probe + Phase5.
    void DetectReflectionModel();
    void Phase2_ProbeSuperStruct(uintptr_t classAddr);
    int32_t Phase2_ProbeUObjectSize();  // sizeof(UObject) via UField::Next offset (bootstraps size)
    void Phase2_ProbePropertiesSize(uintptr_t objectUClass, int32_t sizeofUObject);
    void Phase2_ProbeChildren(uintptr_t classAddr);
    void Phase2_ProbeChildProperties(uintptr_t classAddr);
    void Phase2_ProbeUFieldNext(uintptr_t functionAddr);

    // Phase 3: UClass.

    void Phase3_AutoProbe();
    void Phase3_ProbeCastFlags();
    void Phase3_ProbeClassDefaultObject(uintptr_t classAddr);

    // Phase 4: UFunction.

    void Phase4_AutoProbe();
    void Phase4_CollectAnchors();      // Collect five function anchors.
    void Phase4_ProbeFunctionFlags();
    void Phase4_ProbeNumParmsAndParmsSize();
    void Phase4_ProbeReturnValueOffset();
    void Phase4_ProbeFunc();

    // Phase 5: FField and FProperty.

    void Phase5_AutoProbe();
    void Phase5_CollectAnchors();            // Collect FField anchors.
    void Phase5_ProbeFFieldNamePrivate();
    void Phase5_ProbeFFieldOwner();
    void Phase5_ProbeFFieldNext();
    void Phase5_ProbeFFieldClassPrivate();
    void Phase5_ProbeFFieldFlagsPrivate();
    void Phase5_ProbeFPropertyArrayDimAndElementSize();
    void Phase5_ProbeFPropertyFlags();
    void Phase5_ProbeFPropertyOffsetInternal();
    // Probes both sizeof(FProperty) (true 8-aligned size, after leading-metadata
    // correction) and FProperty::SubPropertyBase (first-known-pointer offset =
    // where derived-class tail data actually lives). They're the same value on
    // standard UE layouts, and differ by the leading-metadata pad on games like
    // DeltaForce. Both results are written into the result map in one pass.
    void Phase5_ProbeFPropertySize();
    // FEnumProperty.UnderlyingType / .Enum positions inside the subclass tail.
    // Walks GObjects for any FEnumProperty instance, then probes both possible
    // orderings (UnderlyingType-first standard vs Enum-first variant) by reading
    // each candidate slot's FField::ClassPrivate name. Writes both result keys
    // when one ordering uniquely matches.
    void Phase5_ProbeFEnumPropertyLayout();
    // FArrayProperty.Inner / FSetProperty.ElementProp / FMapProperty.{Key,Value}Prop
    // tail-pointer offsets. Walks GObjects for one instance of each subclass and
    // scans candidate tail slots (SubPropertyBase, FProperty.Size, Size+8…) until
    // one reads as a valid inner FProperty (validated by FField.ClassPrivate name
    // matching a known property-class FName). Recovers DFM-style alt layouts where
    // individual container subclasses have leading-metadata pads that differ from
    // the global SubPropertyBase value probed via FStructProperty/FObjectPropertyBase.
    void Phase5_ProbeFContainerPropertyTails();

    // Phase 6: ProcessEvent vtable.

    void Phase6_AutoProbe();
    void Phase6_ScanProcessEvent();
    void Phase6_ProbeUEnumNames();  // locate UEnum::Names TArray (layout varies per game)

    // Rendering.

    void DrawPhaseSelector();
    void DrawPhase1();
    void DrawPhase2();
    void DrawPhase3();
    void DrawPhase4();
    void DrawPhase5();
    void DrawPhase6();
    void DrawResultsSummary();
    void DrawOffsetTable(const std::string& category);
    void DrawCandidateTable(const std::string& label, std::vector<ScanCandidate>& candidates,
                            OffsetResult& target);
    void DrawMemoryDump(uintptr_t address, int32_t size, const std::string& label);
    void DrawExportPanel();

    // Dumper integration.

    void DrawDumpPanel();
    void DetectGame();             // Detect the target profile and initialize object-array and name access.
    void StartDump();              // Start a dump worker using available probe results.

    // Validation helpers.

    void CallGetEngineVersion();   // Invoke UKismetSystemLibrary::GetEngineVersion.

    // Helpers.

    uintptr_t GetTextSegStart();
    uintptr_t GetTextSegEnd();
    std::string FormatHex(uint64_t value);
    std::string FormatPtr(uintptr_t ptr);

    // Offset-result helpers.
    OffsetResult& GetResult(const std::string& name);
    int32_t GetConfirmedOffset(const std::string& name);
    bool HasResult(const std::string& name);
    bool HasConfirmed(const std::string& name);

    // State.

    // Current phase.
    int m_CurrentPhase = 0; // Zero selects the overview; one through six select phases.

    // Phase state.
    EPhaseStatus m_PhaseStatus[7] = {};

    // Results indexed by name.
    std::map<std::string, OffsetResult> m_Results;

    // Per-phase scan candidates.
    std::vector<ScanCandidate> m_Phase1InternalIndexCandidates;
    std::vector<ScanCandidate> m_Phase1NamePrivateCandidates;
    std::vector<ScanCandidate> m_Phase1ClassPrivateCandidates;
    std::vector<ScanCandidate> m_Phase1OuterPrivateCandidates;
    std::vector<ScanCandidate> m_Phase1ObjectFlagsCandidates;

    std::vector<ScanCandidate> m_Phase2SuperStructCandidates;
    std::vector<ScanCandidate> m_Phase2SizeCandidates;
    std::vector<ScanCandidate> m_Phase2ChildrenCandidates;
    std::vector<ScanCandidate> m_Phase2ChildPropsCandidates;
    std::vector<ScanCandidate> m_Phase2NextCandidates;
    std::vector<ScanCandidate> m_Phase2MinAlignCandidates;
    std::vector<ScanCandidate> m_Phase2ClassDefaultObjCandidates;

    std::vector<ScanCandidate> m_Phase3CastFlagsCandidates;

    std::vector<ScanCandidate> m_Phase4FuncFlagsCandidates;
    std::vector<ScanCandidate> m_Phase4NumParmsCandidates;
    std::vector<ScanCandidate> m_Phase4ParmsSizeCandidates;
    std::vector<ScanCandidate> m_Phase4ReturnValueOffCandidates;
    std::vector<ScanCandidate> m_Phase4ExecFuncCandidates;

    std::vector<ScanCandidate> m_Phase5FFieldNamePrivateCandidates;
    std::vector<ScanCandidate> m_Phase5FFieldNextCandidates;
    std::vector<ScanCandidate> m_Phase5FFieldOwnerCandidates;
    std::vector<ScanCandidate> m_Phase5FFieldClassCandidates;
    std::vector<ScanCandidate> m_Phase5FFieldFlagsPrivateCandidates;
    std::vector<ScanCandidate> m_Phase5FPropOffsetCandidates;
    std::vector<ScanCandidate> m_Phase5FPropArrayDimCandidates;
    std::vector<ScanCandidate> m_Phase5FPropElemSizeCandidates;
    std::vector<ScanCandidate> m_Phase5FPropFlagsCandidates;
    std::vector<ScanCandidate> m_Phase5FPropSizeCandidates;

    std::vector<ScanCandidate> m_Phase6ProcessEventCandidates;

    // Memory-dump buffer.
    std::vector<uint8_t> m_DumpBuffer;
    uintptr_t m_DumpAddress = 0;
    int32_t m_DumpSize = 0x100;
    char m_DumpAddrInput[32] = {};

    // Cached UClass addresses.
    uintptr_t m_ClassObject = 0;   // Object UClass.
    uintptr_t m_ClassClass = 0;    // Class UClass.
    uintptr_t m_ClassStruct = 0;   // Struct UClass.
    uintptr_t m_ClassField = 0;    // Field UClass.
    uintptr_t m_ClassFunction = 0; // Function UClass.

    // Cached UFunction anchors for Phase 4.
    uintptr_t m_FuncReceiveBeginPlay = 0;
    uintptr_t m_FuncReceiveTick = 0;
    uintptr_t m_FuncIsValid = 0;
    uintptr_t m_FuncPrintString = 0;
    uintptr_t m_FuncK2_GetActorLocation = 0;

    // Cached FField anchors for Phase 5.
    uintptr_t m_FFEntryPoint = 0;      // ExecuteUbergraph->ChildProperties (IntProperty, NumParms=1)
    uintptr_t m_FFDeltaSeconds = 0;    // ReceiveTick->ChildProperties (FloatProperty, NumParms=1)
    uintptr_t m_FFIsValidParam0 = 0;   // IsValid first ChildProperties entry, expected ObjectProperty.
    uintptr_t m_FFIsValidReturn = 0;   // IsValid Next-chain return entry, expected BoolProperty.
    uintptr_t m_FFK2LocReturn = 0;     // K2_GetActorLocation->ChildProperties (StructProperty)
    uintptr_t m_FFEnumProp = 0;        // FEnumProperty instance found through object scanning.

    // Logs.
    struct LogEntry {
        std::string text;
        ImVec4 color;
    };
    std::vector<LogEntry> m_Log;
    void Log(const std::string& text, ImVec4 color = ImVec4(1, 1, 1, 1));
    void LogInfo(const std::string& text);
    void LogSuccess(const std::string& text);
    void LogWarning(const std::string& text);
    void LogError(const std::string& text);

    // Probe bounds.
    int m_ProbeRange = 0x80;

    // Cached engine version.
    std::string m_EngineVersion;

    // Dump state.
    std::atomic<EDumpStatus> m_DumpStatus{EDumpStatus::Idle};
    std::string m_DumpError;
    std::string m_DumpOutputDir;

    // Profile-detection state.
    GameDetectionResult m_GameDetection;
    bool m_GameDetected = false;
    bool m_GObjectsInitialized = false;  // Object-array access initialized from the profile.

    // Reflection model: UObject-derived properties on Children, or FField properties
    // on ChildProperties. DetectReflectionModel caches its inference.
    // Unknown currently follows the FField path; it is not a confirmed model.
    enum class EReflectionModel { Unknown, UProperty, FField };
    EReflectionModel m_ReflectionModel = EReflectionModel::Unknown;
};

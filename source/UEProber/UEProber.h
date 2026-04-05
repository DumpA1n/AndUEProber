#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "imgui/imgui.h"

// ============================================================
//  UEProber — UE 结构逆向探测工具
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

    // ======================== 探测结果数据 ========================

    // 单个偏移量探测结果
    struct OffsetResult {
        std::string name;          // 字段名 (如 "UObject::Index")
        int32_t     offset = -1;   // 探测到的偏移 (-1 = 未确定)
        int32_t     size = 0;      // 字段大小
        std::string typeName;      // 类型名
        std::string evidence;      // 证据描述
        bool        confirmed = false; // 用户确认
        bool        autoDetected = false; // 自动探测成功
    };

    // 阶段状态
    enum class EPhaseStatus {
        NotStarted,
        InProgress,
        Completed,
        Failed,
    };

    // 扫描候选项
    struct ScanCandidate {
        int32_t     offset;
        uint64_t    rawValue;
        std::string description;
        float       confidence;    // 0.0 ~ 1.0
    };

private:
    UEProber();
    ~UEProber() = default;

    // ======================== 内存操作 ========================

    bool ReadMem(uintptr_t address, void* buffer, size_t size);
    bool ReadMemUnsafe(uintptr_t address, void* buffer, size_t size);
    bool TryReadFName(uintptr_t address, std::string& outName);
    bool IsValidPtr(uintptr_t ptr);

    // 在 GObjects 中按名称查找对象, className 非空时先按 Class->Name 过滤
    uintptr_t FindObjectInGObjects(const std::string& targetName, const std::string& className = "");

    // 沿 Children→Next 链查找指定名称的 UFunction
    uintptr_t WalkChildrenChain(uintptr_t classAddr, const std::string& funcName,
                                int32_t childrenOff, int32_t nextOff, int32_t nameOff);

    // 获取结构 Size: 按名称 ("UObject"/"UStruct"/"UClass"/"UFunction"/"UField") 或按地址
    int32_t GetStructSize(const std::string& structName);
    int32_t GetStructSize(uintptr_t structAddr);

    // ======================== 阶段 1: UObject 基础成员 ========================

    void Phase1_AutoProbe();
    void Phase1_ProbeIndex(uintptr_t objAddr, int32_t expectedIndex);
    void Phase1_ProbeName(uintptr_t objAddr, const std::string& expectedName);
    void Phase1_ProbeClass(uintptr_t objAddr, const std::string& expectedClassName);
    void Phase1_ProbeOuter(uintptr_t obj2Addr, uintptr_t obj1Addr);
    void Phase1_ProbeFlags(uintptr_t objAddr);

    // ======================== 阶段 2: UField / UStruct ========================

    void Phase2_AutoProbe();
    void Phase2_ProbeSuper(uintptr_t classAddr);
    void Phase2_ProbePropertiesSize(uintptr_t execUbergraph, uintptr_t objectUClass);
    void Phase2_ProbeMinAlignment(uintptr_t execUbergraph, uintptr_t objectUClass);
    void Phase2_ProbeChildren(uintptr_t classAddr);
    void Phase2_ProbeChildProperties(uintptr_t classAddr);
    void Phase2_ProbeUFieldNext(uintptr_t functionAddr);

    // ======================== 阶段 3: UClass ========================

    void Phase3_AutoProbe();
    void Phase3_ProbeCastFlags();
    void Phase3_ProbeDefaultObject(uintptr_t classAddr);

    // ======================== 阶段 4: UFunction ========================

    void Phase4_AutoProbe();
    void Phase4_CollectAnchors();      // 收集 5 个锚点函数
    void Phase4_ProbeFunctionFlags();
    void Phase4_ProbeNumParmsAndParmsSize();
    void Phase4_ProbeReturnValueOffset();
    void Phase4_ProbeFunc();

    // ======================== 阶段 5: FField / FProperty ========================

    void Phase5_AutoProbe();
    void Phase5_CollectAnchors();            // 收集锚点 FField
    void Phase5_ProbeFFieldNamePrivate();
    void Phase5_ProbeFFieldOwner();
    void Phase5_ProbeFFieldNext();
    void Phase5_ProbeFFieldClassPrivate();
    void Phase5_ProbeFFieldFlagsPrivate();
    void Phase5_ProbeFPropertyArrayDimAndElementSize();
    void Phase5_ProbeFPropertyFlags();
    void Phase5_ProbeFPropertyOffsetInternal();
    void Phase5_ProbeFPropertySize();

    // ======================== 阶段 6: ProcessEvent VTable ========================

    void Phase6_AutoProbe();
    void Phase6_ScanProcessEvent();

    // ======================== 绘制 ========================

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

    // ======================== 验证工具 ========================

    void CallGetEngineVersion();   // 调用 UKismetSystemLibrary::GetEngineVersion 打印版本

    // ======================== 工具函数 ========================

    uintptr_t GetTextSegStart();
    uintptr_t GetTextSegEnd();
    std::string FormatHex(uint64_t value);
    std::string FormatPtr(uintptr_t ptr);

    // 偏移结果辅助
    OffsetResult& GetResult(const std::string& name);
    int32_t GetConfirmedOffset(const std::string& name);
    bool HasResult(const std::string& name);
    bool HasConfirmed(const std::string& name);

    // ======================== 状态 ========================

    // 当前阶段
    int m_CurrentPhase = 0; // 0=总览, 1~6=各阶段

    // 阶段状态
    EPhaseStatus m_PhaseStatus[7] = {};

    // .text 段范围
    uintptr_t m_TextStart = 0;
    uintptr_t m_TextEnd = 0;

    // 所有探测结果 (按名称索引)
    std::map<std::string, OffsetResult> m_Results;

    // 各阶段扫描候选项
    std::vector<ScanCandidate> m_Phase1IndexCandidates;
    std::vector<ScanCandidate> m_Phase1NameCandidates;
    std::vector<ScanCandidate> m_Phase1ClassCandidates;
    std::vector<ScanCandidate> m_Phase1OuterCandidates;
    std::vector<ScanCandidate> m_Phase1FlagsCandidates;

    std::vector<ScanCandidate> m_Phase2SuperCandidates;
    std::vector<ScanCandidate> m_Phase2SizeCandidates;
    std::vector<ScanCandidate> m_Phase2ChildrenCandidates;
    std::vector<ScanCandidate> m_Phase2ChildPropsCandidates;
    std::vector<ScanCandidate> m_Phase2NextCandidates;
    std::vector<ScanCandidate> m_Phase2MinAlignCandidates;
    std::vector<ScanCandidate> m_Phase2DefaultObjCandidates;

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

    // 内存 dump 缓冲
    std::vector<uint8_t> m_DumpBuffer;
    uintptr_t m_DumpAddress = 0;
    int32_t m_DumpSize = 0x100;
    char m_DumpAddrInput[32] = {};

    // 缓存的关键 UClass 地址
    uintptr_t m_ClassObject = 0;   // "Object" 的 UClass
    uintptr_t m_ClassClass = 0;    // "Class" 的 UClass
    uintptr_t m_ClassStruct = 0;   // "Struct" 的 UClass
    uintptr_t m_ClassField = 0;    // "Field" 的 UClass
    uintptr_t m_ClassFunction = 0; // "Function" 的 UClass

    // 缓存的 UFunction 锚点地址 (用于阶段4)
    uintptr_t m_FuncReceiveBeginPlay = 0;
    uintptr_t m_FuncReceiveTick = 0;
    uintptr_t m_FuncIsValid = 0;
    uintptr_t m_FuncPrintString = 0;
    uintptr_t m_FuncK2_GetActorLocation = 0;

    // 缓存的 FField 锚点地址 (用于阶段5)
    uintptr_t m_FFEntryPoint = 0;      // ExecuteUbergraph->ChildProperties (IntProperty, NumParms=1)
    uintptr_t m_FFDeltaSeconds = 0;    // ReceiveTick->ChildProperties (FloatProperty, NumParms=1)
    uintptr_t m_FFIsValidParam0 = 0;   // IsValid->ChildProperties 首参 (ObjectProperty)
    uintptr_t m_FFIsValidReturn = 0;   // IsValid->ChildProperties Next链 (BoolProperty)
    uintptr_t m_FFK2LocReturn = 0;     // K2_GetActorLocation->ChildProperties (StructProperty)

    // 日志
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

    // 探测范围配置
    int m_ProbeRange = 0x80;

    // 缓存的引擎版本
    std::string m_EngineVersion;
};

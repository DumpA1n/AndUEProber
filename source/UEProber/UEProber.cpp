#include "UEProber.h"

#include "Core/ElfScannerManager.h"
#include "UECore/CoreUObject_classes.h"
#include "Utils/FileLogger.h"
#include "Utils/KittyEx.h"
#include "Utils/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <set>
#include <thread>
#include <unistd.h>

// 探测器调试日志开关
#define PROBER_DEBUG 1

#if PROBER_DEBUG
#define PDBG(fmt, ...) FLOGF("[PB] " fmt, ##__VA_ARGS__)
#else
#define PDBG(fmt, ...) do {} while(0)
#endif

// UE4 FName 是 case-insensitive 的, 所有从 FName 读取的字符串比较都应忽略大小写
static bool FNameEq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

using namespace SDK;

// ============================================================
//  EFunctionFlags 位定义 (UE 4.24)
// ============================================================
namespace EFuncFlags {
    constexpr uint32_t FUNC_Final           = 0x00000001;
    constexpr uint32_t FUNC_Net             = 0x00000040;
    constexpr uint32_t FUNC_Native          = 0x00000400;
    constexpr uint32_t FUNC_Event           = 0x00000800;
    constexpr uint32_t FUNC_Static          = 0x00002000;
    constexpr uint32_t FUNC_NetServer       = 0x00200000;
    constexpr uint32_t FUNC_HasOutParms     = 0x00400000;
    constexpr uint32_t FUNC_HasDefaults     = 0x00800000;
    constexpr uint32_t FUNC_BlueprintCallable = 0x04000000;
    constexpr uint32_t FUNC_BlueprintEvent  = 0x08000000;
    constexpr uint32_t FUNC_BlueprintPure   = 0x10000000;
}

// EClassCastFlags 常用值 (参照 Basic.h EClassCastFlags 枚举)
namespace ECastFlags {
    constexpr uint64_t CASTCLASS_UField          = 0x0000000000000001;
    constexpr uint64_t CASTCLASS_UEnum           = 0x0000000000000004;
    constexpr uint64_t CASTCLASS_UStruct         = 0x0000000000000008;
    constexpr uint64_t CASTCLASS_UScriptStruct   = 0x0000000000000010;
    constexpr uint64_t CASTCLASS_UClass          = 0x0000000000000020;
    constexpr uint64_t CASTCLASS_UProperty       = 0x0000000000008000;
    constexpr uint64_t CASTCLASS_UFunction       = 0x0000000000080000;
    constexpr uint64_t CASTCLASS_UPackage        = 0x0000000400000000;

    // "Class" 元类的 CastFlags = UField|UStruct|UClass = 0x29
    constexpr uint64_t KNOWN_CLASS_FLAGS    = CASTCLASS_UField | CASTCLASS_UStruct | CASTCLASS_UClass; // 0x29
    // "Package" UClass 的 CastFlags = 0x0000000400000000
    constexpr uint64_t KNOWN_PACKAGE_FLAGS  = CASTCLASS_UPackage;
}

// CPF 属性标志
namespace ECPFFlags {
    constexpr uint64_t CPF_Parm        = 0x0000000000000080;
    constexpr uint64_t CPF_OutParm     = 0x0000000000000100;
    constexpr uint64_t CPF_ReturnParm  = 0x0000000000000400;
    constexpr uint64_t CPF_Net         = 0x0000000000000020;
}

// ============================================================
//  构造
// ============================================================

UEProber::UEProber() {
    memset(m_DumpAddrInput, 0, sizeof(m_DumpAddrInput));

    for (int i = 0; i < 7; ++i)
        m_PhaseStatus[i] = EPhaseStatus::NotStarted;
}

// ============================================================
//  内存操作
// ============================================================

bool UEProber::ReadMem(uintptr_t address, void* buffer, size_t size) {
    if (!KT::IsValid(address))
        return false;
    return KT::Read(address, buffer, size);
}

bool UEProber::ReadMemUnsafe(uintptr_t address, void* buffer, size_t size) {
    if (!address) return false;
    return KT::ReadRaw(address, buffer, size);
}

bool UEProber::TryReadFName(uintptr_t address, std::string& outName) {
    struct RawFName {
        int32_t ComparisonIndex;
        uint32_t Number;
    };

    RawFName fname{};
    if (!ReadMem(address, &fname, sizeof(fname)))
        return false;

    if (fname.ComparisonIndex < 0 || fname.ComparisonIndex > 0x2000000 || fname.Number > 0xFFFF)
        return false;

    FName tempName;
    memcpy(&tempName, &fname, sizeof(fname));

    try {
        std::string result = tempName.GetRawString();
        if (result.empty() || result.size() > 1024)
            return false;

        bool hasAlpha = false;
        for (char c : result) {
            if (c < 0x20 || c > 0x7E) return false;
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                hasAlpha = true;
        }
        if (!hasAlpha) return false;

        outName = result;
        return true;
    } catch (...) {
        return false;
    }
}

bool UEProber::IsValidPtr(uintptr_t ptr) {
    // 高位只允许 0 或 PAC 标记 (0xB4)，其余视为垃圾
    uint8_t top = (ptr >> 56) & 0xFF;
    if (top != 0x00 && top != 0xB4)
        return false;
    return KT::IsValidStrong(ptr);
}

uintptr_t UEProber::FindObjectInGObjects(const std::string& targetName, const std::string& className) {
    int32_t namePrivateOffset = GetConfirmedOffset("UObject::NamePrivate");
    int32_t classPrivateOffset = GetConfirmedOffset("UObject::ClassPrivate");
    if (namePrivateOffset < 0) return 0;

    bool filterByClass = !className.empty() && classPrivateOffset >= 0;
    int32_t count = UObject::GObjects->Num();

    for (int32_t i = 1; i < count; ++i) {
        uintptr_t obj = reinterpret_cast<uintptr_t>(UObject::GObjects->GetByIndex(i));
        if (!obj || !IsValidPtr(obj)) continue;

        if (filterByClass) {
            uintptr_t objClass = 0;
            if (!ReadMem(obj + classPrivateOffset, &objClass, 8) || !IsValidPtr(objClass)) continue;
            std::string clsName;
            if (!TryReadFName(objClass + namePrivateOffset, clsName) || !FNameEq(clsName, className)) continue;
        }

        std::string name;
        if (!TryReadFName(obj + namePrivateOffset, name)) continue;
        if (FNameEq(name, targetName)) return obj;
    }
    return 0;
}

int32_t UEProber::GetStructSize(const std::string& structName) {
    int32_t sizeOff = GetConfirmedOffset("UStruct::PropertiesSize");
    if (sizeOff < 0) return 0;

    // 优先使用缓存地址
    uintptr_t addr = 0;
    if (structName == "UObject") {
        addr = m_ClassObject;
        if (!addr) addr = reinterpret_cast<uintptr_t>(UObject::GObjects->GetByIndex(1));
    } else if (structName == "UStruct")   addr = m_ClassStruct;
    else if (structName == "UClass")      addr = m_ClassClass;
    else if (structName == "UField")      addr = m_ClassField;
    else if (structName == "UFunction")   addr = m_ClassFunction;

    // 未缓存: 在 GObjects 中查找对应 UClass/UScriptStruct
    if (!addr) {
        // 去掉 "U"/"A"/"F" 前缀, GObjects 中 Name 不带前缀
        std::string plainName = structName;
        if (!plainName.empty() && (plainName[0] == 'U' || plainName[0] == 'A' || plainName[0] == 'F'))
            plainName = plainName.substr(1);
        addr = FindObjectInGObjects(plainName, "Class");
        if (!addr) addr = FindObjectInGObjects(plainName, "ScriptStruct");
    }

    if (!addr) return 0;
    return GetStructSize(addr);
}

int32_t UEProber::GetStructSize(uintptr_t structAddr) {
    int32_t sizeOff = GetConfirmedOffset("UStruct::PropertiesSize");
    if (sizeOff < 0 || !structAddr) return 0;
    int32_t size = 0;
    ReadMem(structAddr + sizeOff, &size, 4);
    return size;
}

// ============================================================
//  工具函数
// ============================================================

uintptr_t UEProber::GetTextSegStart() {
    if (m_TextStart) return m_TextStart;
    // 从 ElfScanner 获取
    m_TextStart = Elf.UE4().base();
    return m_TextStart;
}

uintptr_t UEProber::GetTextSegEnd() {
    if (m_TextEnd) return m_TextEnd;
    m_TextEnd = Elf.UE4().end();
    return m_TextEnd;
}

std::string UEProber::FormatHex(uint64_t value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)value);
    return buf;
}

std::string UEProber::FormatPtr(uintptr_t ptr) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)ptr);
    return buf;
}

UEProber::OffsetResult& UEProber::GetResult(const std::string& name) {
    auto it = m_Results.find(name);
    if (it == m_Results.end()) {
        m_Results[name] = OffsetResult{};
        m_Results[name].name = name;
    }
    return m_Results[name];
}

int32_t UEProber::GetConfirmedOffset(const std::string& name) {
    auto it = m_Results.find(name);
    if (it != m_Results.end() && it->second.confirmed)
        return it->second.offset;
    return -1;
}

bool UEProber::HasResult(const std::string& name) {
    auto it = m_Results.find(name);
    return it != m_Results.end() && it->second.autoDetected;
}

bool UEProber::HasConfirmed(const std::string& name) {
    auto it = m_Results.find(name);
    return it != m_Results.end() && it->second.confirmed;
}

// ============================================================
//  日志
// ============================================================

void UEProber::Log(const std::string& text, ImVec4 color) {
    m_Log.push_back({text, color});
    if (m_Log.size() > 500)
        m_Log.erase(m_Log.begin(), m_Log.begin() + 100);
}

void UEProber::LogInfo(const std::string& text) {
    Log("[INFO] " + text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
}

void UEProber::LogSuccess(const std::string& text) {
    Log("[OK] " + text, ImVec4(0.2f, 1.0f, 0.4f, 1.0f));
}

void UEProber::LogWarning(const std::string& text) {
    Log("[WARN] " + text, ImVec4(1.0f, 0.8f, 0.2f, 1.0f));
}

void UEProber::LogError(const std::string& text) {
    Log("[ERR] " + text, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
}

// ============================================================
//  阶段 1: UObject 基础成员探测
// ============================================================

void UEProber::Phase1_ProbeInternalIndex(uintptr_t objAddr, int32_t expectedIndex) {
    m_Phase1InternalIndexCandidates.clear();
    LogInfo(std::format("探测 UObject::Index @ {}, 期望值={}", FormatPtr(objAddr), expectedIndex));

    for (int32_t off = 0; off < m_ProbeRange; off += 4) {
        uint32_t val = 0;
        if (!ReadMem(objAddr + off, &val, 4)) continue;
        if ((int32_t)val == expectedIndex) {
            float confidence = 0.5f;
            // 交叉验证: 检查其他对象
            int crossMatch = 0;
            for (int idx = 0; idx <= 3; ++idx) {
                if (idx == expectedIndex) { crossMatch++; continue; }
                uintptr_t otherObj = reinterpret_cast<uintptr_t>(UObject::GObjects->GetByIndex(idx));
                if (!otherObj) continue;
                uint32_t otherVal = 0;
                if (ReadMem(otherObj + off, &otherVal, 4) && (int32_t)otherVal == idx)
                    crossMatch++;
            }
            confidence = (float)crossMatch / 4.0f;

            m_Phase1InternalIndexCandidates.push_back({
                off, val,
                std::format("在 obj[{}] 偏移 0x{:X} 找到值 {}, 交叉验证 {}/4",
                    expectedIndex, off, val, crossMatch),
                confidence
            });

            // 全部交叉验证通过则无需继续
            if (confidence >= 1.0f)
                break;
        }
    }

    std::sort(m_Phase1InternalIndexCandidates.begin(), m_Phase1InternalIndexCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase1InternalIndexCandidates.empty()) {
        auto& best = m_Phase1InternalIndexCandidates[0];
        if (best.confidence >= 0.75f) {
            auto& r = GetResult("UObject::InternalIndex");
            r.offset = best.offset;
            r.size = 4;
            r.typeName = "int32";
            r.evidence = best.description;
            r.autoDetected = true;
            if (best.confidence >= 1.0f) r.confirmed = true;
            LogSuccess(std::format("UObject::InternalIndex 自动探测: 偏移 0x{:X} (置信度 {:.0f}%){}",
                best.offset, best.confidence * 100, r.confirmed ? " [已自动确认]" : ""));
        }
    }
}

void UEProber::Phase1_ProbeNamePrivate(uintptr_t objAddr, const std::string& expectedName) {
    m_Phase1NamePrivateCandidates.clear();
    LogInfo(std::format("探测 UObject::Name @ {}, 期望 ToString()=\"{}\"", FormatPtr(objAddr), expectedName));

    const int32_t maxRange = std::min(m_ProbeRange, 0x40);
    uint8_t* base = reinterpret_cast<uint8_t*>(objAddr);

    for (int32_t off = 0; off < maxRange; off += 4) {
        try {
            FName* namePtr = reinterpret_cast<FName*>(base + off);
            int32_t cmpIdx = namePtr->ComparisonIndex;
            if (cmpIdx < 0 || cmpIdx > 0x200000) continue;
            std::string toString = namePtr->ToString();
            if (toString.empty()) continue;
            bool validStr = true;
            for (char c : toString) {
                if (c < 0x20 || c > 0x7E) { validStr = false; break; }
            }
            if (!validStr) continue;
            std::string rawString = namePtr->GetRawString();

            float confidence = 0.0f;
            if (toString == expectedName)
                confidence = 1.0f;
            else if (!toString.empty() && toString.size() < 256)
                confidence = 0.1f;
            else
                continue;

            if (confidence > 0.05f) {
                m_Phase1NamePrivateCandidates.push_back({
                    off, 0,
                    std::format("偏移 0x{:X} -> ToString()=\"{}\", GetRawString()=\"{}\"",
                        off, toString, rawString),
                    confidence
                });
            }

            // 精确匹配则无需继续探测
            if (confidence >= 1.0f)
                break;
        } catch (...) {
            continue;
        }
    }

    std::sort(m_Phase1NamePrivateCandidates.begin(), m_Phase1NamePrivateCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase1NamePrivateCandidates.empty() && m_Phase1NamePrivateCandidates[0].confidence >= 0.9f) {
        auto& best = m_Phase1NamePrivateCandidates[0];
        auto& r = GetResult("UObject::NamePrivate");
        r.offset = best.offset;
        r.size = 8;
        r.typeName = "FName";
        r.evidence = best.description;
        r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        LogSuccess(std::format("UObject::NamePrivate 自动探测: 偏移 0x{:X}{}", best.offset, r.confirmed ? " [已自动确认]" : ""));
    }
}

void UEProber::Phase1_ProbeClassPrivate(uintptr_t objAddr, const std::string& expectedClassName) {
    m_Phase1ClassPrivateCandidates.clear();
    int32_t namePrivateOffset = GetConfirmedOffset("UObject::NamePrivate");
    if (namePrivateOffset < 0) {
        LogWarning("需要先确定 UObject::Name 偏移才能探测 Class");
        return;
    }
    LogInfo(std::format("探测 UObject::Class @ {}, 期望 Class Name = \"{}\"",
        FormatPtr(objAddr), expectedClassName));

    for (int32_t off = 0; off < m_ProbeRange; off += 8) {
        uintptr_t ptr = 0;
        if (!ReadMem(objAddr + off, &ptr, 8)) continue;
        if (!IsValidPtr(ptr)) continue;

        std::string name;
        if (TryReadFName(ptr + namePrivateOffset, name)) {
            float confidence = 0.0f;
            if (name == expectedClassName)
                confidence = 1.0f;
            else if (!name.empty())
                confidence = 0.1f;

            if (confidence > 0.05f) {
                m_Phase1ClassPrivateCandidates.push_back({
                    off, ptr,
                    std::format("偏移 0x{:X} -> ptr {} -> Name = \"{}\"",
                        off, FormatPtr(ptr), name),
                    confidence
                });
            }

            // 精确匹配则无需继续探测
            if (confidence >= 1.0f)
                break;
        }
    }

    std::sort(m_Phase1ClassPrivateCandidates.begin(), m_Phase1ClassPrivateCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase1ClassPrivateCandidates.empty() && m_Phase1ClassPrivateCandidates[0].confidence >= 0.9f) {
        auto& best = m_Phase1ClassPrivateCandidates[0];
        auto& r = GetResult("UObject::ClassPrivate");
        r.offset = best.offset;
        r.size = 8;
        r.typeName = "UClass*";
        r.evidence = best.description;
        r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        LogSuccess(std::format("UObject::ClassPrivate 自动探测: 偏移 0x{:X}{}", best.offset, r.confirmed ? " [已自动确认]" : ""));
    }
}

void UEProber::Phase1_ProbeOuterPrivate(uintptr_t obj2Addr, uintptr_t obj1Addr) {
    m_Phase1OuterPrivateCandidates.clear();
    LogInfo(std::format("探测 UObject::Outer @ {}, 期望指向 {}",
        FormatPtr(obj2Addr), FormatPtr(obj1Addr)));

    for (int32_t off = 0; off < m_ProbeRange; off += 8) {
        uintptr_t ptr = 0;
        if (!ReadMem(obj2Addr + off, &ptr, 8)) continue;

        if (ptr == obj1Addr) {
            m_Phase1OuterPrivateCandidates.push_back({
                off, ptr,
                std::format("偏移 0x{:X} -> ptr {} == obj[1] 地址", off, FormatPtr(ptr)),
                1.0f
            });
            // 精确匹配则无需继续探测
            break;
        } else if (IsValidPtr(ptr)) {
            int32_t namePrivateOffset = GetConfirmedOffset("UObject::NamePrivate");
            std::string name;
            if (namePrivateOffset >= 0 && TryReadFName(ptr + namePrivateOffset, name)) {
                m_Phase1OuterPrivateCandidates.push_back({
                    off, ptr,
                    std::format("偏移 0x{:X} -> ptr {} -> Name = \"{}\"",
                        off, FormatPtr(ptr), name),
                    0.1f
                });
            }
        }
    }

    std::sort(m_Phase1OuterPrivateCandidates.begin(), m_Phase1OuterPrivateCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase1OuterPrivateCandidates.empty() && m_Phase1OuterPrivateCandidates[0].confidence >= 0.9f) {
        auto& best = m_Phase1OuterPrivateCandidates[0];
        auto& r = GetResult("UObject::OuterPrivate");
        r.offset = best.offset;
        r.size = 8;
        r.typeName = "UObject*";
        r.evidence = best.description;
        r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        LogSuccess(std::format("UObject::OuterPrivate 自动探测: 偏移 0x{:X}{}", best.offset, r.confirmed ? " [已自动确认]" : ""));
    }
}

void UEProber::Phase1_ProbeObjectFlags(uintptr_t objAddr) {
    m_Phase1ObjectFlagsCandidates.clear();
    LogInfo("探测 UObject::Flags (RF_xxx 位域)");

    // 排除已确定的偏移
    std::set<int32_t> usedOffsets;
    for (auto& [name, result] : m_Results) {
        if (result.confirmed && result.offset >= 0) {
            for (int32_t b = 0; b < result.size; ++b)
                usedOffsets.insert(result.offset + b);
        }
    }

    for (int32_t off = 0; off < m_ProbeRange; off += 4) {
        if (usedOffsets.count(off)) continue;

        uint32_t val = 0;
        if (!ReadMem(objAddr + off, &val, 4)) continue;

        // obj[0] 的 Flags 已知为 1 (RF_Public)
        if (val != 1) continue;

        // 交叉验证多个对象: 其它对象的 Flags 也应非零且在合理范围内
        int validCount = 0;
        for (int idx = 0; idx <= 3; ++idx) {
            uintptr_t otherObj = reinterpret_cast<uintptr_t>(UObject::GObjects->GetByIndex(idx));
            if (!otherObj) continue;
            uint32_t otherVal = 0;
            if (ReadMem(otherObj + off, &otherVal, 4) && otherVal > 0 && otherVal <= 0x03FFFFFF)
                validCount++;
        }

        float confidence = (float)validCount / 4.0f;
        m_Phase1ObjectFlagsCandidates.push_back({
            off, val,
            std::format("偏移 0x{:X} -> 0x{:08X}, {}/4 对象有合理 Flag 值", off, val, validCount),
            confidence
        });

        // 全部交叉验证通过则无需继续
        if (confidence >= 1.0f)
            break;
    }

    std::sort(m_Phase1ObjectFlagsCandidates.begin(), m_Phase1ObjectFlagsCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase1ObjectFlagsCandidates.empty() && m_Phase1ObjectFlagsCandidates[0].confidence >= 0.75f) {
        auto& best = m_Phase1ObjectFlagsCandidates[0];
        auto& r = GetResult("UObject::ObjectFlags");
        r.offset = best.offset;
        r.size = 4;
        r.typeName = "int32";
        r.evidence = best.description;
        r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        LogSuccess(std::format("UObject::ObjectFlags 自动探测: 偏移 0x{:X} (置信度 {:.0f}%){}",
            best.offset, best.confidence * 100, r.confirmed ? " [已自动确认]" : ""));
    }
}

void UEProber::Phase1_AutoProbe() {
    m_PhaseStatus[1] = EPhaseStatus::InProgress;
    LogInfo("===== 阶段 1: UObject 基础成员自动探测 =====");

    // VTable 始终在偏移 0
    {
        auto& r = GetResult("UObject::VTable");
        r.offset = 0; r.size = 8; r.typeName = "void**";
        r.evidence = "VTable 始终在偏移 0";
        r.autoDetected = true; r.confirmed = true;
        LogSuccess("UObject::VTable = 0x00 (固定)");
    }

    // 获取 obj[0] 和 obj[1]
    // GetByIndex(0) -> Name="CoreUObject", Class="Package"
    // GetByIndex(1) -> Name="Object", Outer 指向 obj[0]
    uintptr_t obj0 = reinterpret_cast<uintptr_t>(UObject::GObjects->GetByIndex(0));
    uintptr_t obj1 = reinterpret_cast<uintptr_t>(UObject::GObjects->GetByIndex(1));

    if (!obj0 || !IsValidPtr(obj0)) {
        LogError("无法获取 GetByIndex(0), 请检查 GObjects 地址");
        m_PhaseStatus[1] = EPhaseStatus::Failed;
        return;
    }

    LogInfo(std::format("obj[0] = {}", FormatPtr(obj0)));
    if (obj1) LogInfo(std::format("obj[1] = {}", FormatPtr(obj1)));

    // 探测 Index (用 obj[1] 的 Index 应为 1, 避免 0 误匹配)
    Phase1_ProbeInternalIndex(obj1, 1);

    // 探测 Name (obj[0] 的 Name 应为 "CoreUObject")
    Phase1_ProbeNamePrivate(obj0, "CoreUObject");

    // 如果 Name 已确定，探测 Class
    if (HasConfirmed("UObject::NamePrivate")) {
        Phase1_ProbeClassPrivate(obj0, "Package");
    }

    // 如果有 obj[1]，探测 Outer (obj[1].Outer 应指向 obj[0])
    if (obj1 && IsValidPtr(obj1)) {
        Phase1_ProbeOuterPrivate(obj1, obj0);
    }

    // 探测 Flags
    Phase1_ProbeObjectFlags(obj0);

    LogInfo("阶段 1 自动探测完成, 请查看候选结果并手动确认");
}

// ============================================================
//  阶段 2: UField / UStruct 探测
// ============================================================

void UEProber::Phase2_ProbeSuperStruct(uintptr_t classAddr) {
    PDBG(">>>>>>>>>> [ProbeSuper] BEGIN >>>>>>>>>>");
    m_Phase2SuperStructCandidates.clear();
    int32_t namePrivateOffset = GetConfirmedOffset("UObject::NamePrivate");
    PDBG("ProbeSuper: classAddr={}, namePrivateOffset=0x{:X}", FormatPtr(classAddr), namePrivateOffset);
    if (namePrivateOffset < 0) { PDBG("ProbeSuper: namePrivateOffset < 0, 中止"); PDBG("<<<<<<<<<< [ProbeSuper] END <<<<<<<<<<"); return; }

    // classAddr 是 GetByIndex(0) 的 Class ("Package" UClass)
    // 其 Super 应指向 obj[1] (UObject 基类, Name="Object")
    uintptr_t obj1 = reinterpret_cast<uintptr_t>(UObject::GObjects->GetByIndex(1));
    PDBG("ProbeSuper: obj[1]={}", FormatPtr(obj1));

    // Super 是 UStruct 成员, 搜索范围: sizeof(UObject) ~ sizeof(UStruct)
    // MinAlignment/Size 已优先探测, Size 必然已确认
    int32_t sizeofUObject = GetStructSize("UObject");
    int32_t sizeofUStruct = GetStructSize("UStruct");
    int32_t searchStart = sizeofUObject;
    int32_t searchEnd = sizeofUStruct > 0 ? sizeofUStruct : sizeofUObject * 3;
    PDBG("ProbeSuper: 搜索范围 [0x{:X}, 0x{:X}), sizeofUObject=0x{:X}, sizeofUStruct=0x{:X}",
         searchStart, searchEnd, sizeofUObject, sizeofUStruct);

    int scannedPtrs = 0;
    for (int32_t off = searchStart; off < searchEnd; off += 8) {
        uintptr_t ptr = 0;
        if (!ReadMem(classAddr + off, &ptr, 8)) continue;
        if (!IsValidPtr(ptr)) continue;
        scannedPtrs++;

        std::string name;
        if (!TryReadFName(ptr + namePrivateOffset, name)) continue;

        float confidence = 0.0f;
        if (ptr == obj1 || FNameEq(name, "Object")) {
            PDBG("ProbeSuper: off=0x{:X} 匹配 obj1/Object, ptr={} name=\"{}\", 开始验证链",
                 off, FormatPtr(ptr), name);
            // 直接匹配 obj[1] 地址或名称
            // 验证链: Object 的 Class -> "Class" UClass, 读 "Class".Super -> 应为 "Struct"
            int32_t classPrivateOffset = GetConfirmedOffset("UObject::ClassPrivate");
            if (classPrivateOffset >= 0) {
                uintptr_t objClass = 0;
                if (ReadMem(ptr + classPrivateOffset, &objClass, 8) && IsValidPtr(objClass)) {
                    // objClass 应是 "Object" UClass, 其 Class 应是 "Class" UClass
                    uintptr_t classClassAddr = 0;
                    if (ReadMem(objClass + classPrivateOffset, &classClassAddr, 8) && IsValidPtr(classClassAddr)) {
                        // 验证 Class->Super == Struct
                        uintptr_t classSuperPtr = 0;
                        if (ReadMem(classClassAddr + off, &classSuperPtr, 8) && IsValidPtr(classSuperPtr)) {
                            std::string superName;
                            if (TryReadFName(classSuperPtr + namePrivateOffset, superName) &&
                                FNameEq(superName, "Struct")) {
                                confidence = 1.0f;
                                PDBG("ProbeSuper: 验证链成功 Class->Super->Name=\"Struct\", conf=1.0");
                            } else {
                                confidence = 0.7f;
                                PDBG("ProbeSuper: Class->Super->Name=\"{}\" (期望Struct), conf=0.7", superName);
                            }
                        } else {
                            confidence = 0.6f;
                            PDBG("ProbeSuper: Class->Super 读取失败/无效 (classSuperPtr={}), conf=0.6",
                                 FormatPtr(classSuperPtr));
                        }
                    } else {
                        confidence = 0.5f;
                        PDBG("ProbeSuper: classClassAddr 读取失败/无效, conf=0.5");
                    }
                } else {
                    confidence = 0.5f;
                }
            } else {
                confidence = (ptr == obj1) ? 0.8f : 0.5f;
            }
        } else if (!name.empty()) {
            confidence = 0.1f;
        }

        PDBG("ProbeSuper: off=0x{:X} ptr={} name=\"{}\" confidence={:.2f}",
             off, FormatPtr(ptr), name, confidence);

        if (confidence > 0.05f) {
            m_Phase2SuperStructCandidates.push_back({
                off, ptr,
                std::format("偏移 0x{:X} -> {} -> Name = \"{}\"", off, FormatPtr(ptr), name),
                confidence
            });
        }

        if (confidence >= 1.0f) break;
    }

    PDBG("ProbeSuper: 扫描完毕, 有效指针={}, 候选数={}", scannedPtrs, m_Phase2SuperStructCandidates.size());
    std::sort(m_Phase2SuperStructCandidates.begin(), m_Phase2SuperStructCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase2SuperStructCandidates.empty() && m_Phase2SuperStructCandidates[0].confidence >= 0.9f) {
        auto& best = m_Phase2SuperStructCandidates[0];
        auto& r = GetResult("UStruct::SuperStruct");
        r.offset = best.offset; r.size = 8; r.typeName = "UStruct*";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeSuper: 选定 offset=0x{:X}, confidence={:.2f}, confirmed={}",
             best.offset, best.confidence, r.confirmed);

        // 通过继承链缓存各 UClass 地址
        // obj[1] 本身就是 UObject 基类 (Name="Object", Class.Name="Class")
        m_ClassObject = obj1;
        int32_t classPrivateOffset = GetConfirmedOffset("UObject::ClassPrivate");
        if (classPrivateOffset >= 0) {
            // obj[1].Class = "Class" UClass (元类)
            uintptr_t classUClass = 0;
            if (ReadMem(obj1 + classPrivateOffset, &classUClass, 8) && IsValidPtr(classUClass)) {
                m_ClassClass = classUClass;
                // Class->Super 应为 Struct
                uintptr_t structAddr = 0;
                if (ReadMem(classUClass + best.offset, &structAddr, 8) && IsValidPtr(structAddr))
                    m_ClassStruct = structAddr;
                // Struct->Super 应为 Field
                if (m_ClassStruct) {
                    uintptr_t fieldAddr = 0;
                    if (ReadMem(m_ClassStruct + best.offset, &fieldAddr, 8) && IsValidPtr(fieldAddr))
                        m_ClassField = fieldAddr;
                }
            }
        }
    } else {
        PDBG("ProbeSuper: 无满足阈值的候选 (最佳 confidence={:.2f})",
             m_Phase2SuperStructCandidates.empty() ? 0.0f : m_Phase2SuperStructCandidates[0].confidence);
    }
    PDBG("<<<<<<<<<< [ProbeSuper] END <<<<<<<<<<");
}

void UEProber::Phase2_ProbeMinAlignment(uintptr_t execUbergraph, uintptr_t objectUClass) {
    PDBG(">>>>>>>>>> [ProbeMinAlignment] BEGIN >>>>>>>>>>");
    m_Phase2MinAlignCandidates.clear();
    PDBG("ProbeMinAlignment: execUbergraph={}, objectUClass={}",
         FormatPtr(execUbergraph), FormatPtr(objectUClass));

    // MinAlignment 是 UStruct 成员, Size 此时尚未确认
    // confidence >= 1.0 时 early break, 无需设定上界
    int scannedCount = 0;
    for (int32_t off = 0x28; ; off += 4) {
        int32_t valExec = 0, valObject = 0;
        bool r1 = ReadMem(execUbergraph + off, &valExec, 4);
        if (!r1) {
            PDBG("ProbeMinAlignment: ReadMem 失败 off=0x{:X}, 停止扫描 (已扫描 {} 个偏移)", off, scannedCount);
            break;
        }
        bool r2 = objectUClass && ReadMem(objectUClass + off, &valObject, 4);
        scannedCount++;

        float confidence = 0.0f;
        if (valExec == 0x1 && r2 && valObject == 0x8)
            confidence = 1.0f;
        else if ((valExec == 0x1 || valExec == 0x4 || valExec == 0x8) &&
                 r2 && (valObject == 0x4 || valObject == 0x8 || valObject == 0x10))
            confidence = 0.4f;

        if (confidence > 0.05f) {
            PDBG("ProbeMinAlignment: off=0x{:X} valExec=0x{:X} valObject=0x{:X} conf={:.2f}",
                 off, valExec, valObject, confidence);
            m_Phase2MinAlignCandidates.push_back({
                off, (uint64_t)valExec,
                std::format("偏移 0x{:X}: ExecUbergraph={}, Object={}", off, valExec, valObject),
                confidence
            });
        }

        if (confidence >= 1.0f) break;
    }

    PDBG("ProbeMinAlignment: 扫描完毕, 候选数={}, 扫描偏移数={}",
         m_Phase2MinAlignCandidates.size(), scannedCount);

    std::sort(m_Phase2MinAlignCandidates.begin(), m_Phase2MinAlignCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase2MinAlignCandidates.empty() && m_Phase2MinAlignCandidates[0].confidence >= 0.8f) {
        auto& best = m_Phase2MinAlignCandidates[0];
        auto& r = GetResult("UStruct::MinAlignment");
        r.offset = best.offset; r.size = 4; r.typeName = "int32";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeMinAlignment: 选定 offset=0x{:X}, confidence={:.2f}, confirmed={}",
             best.offset, best.confidence, r.confirmed);
    } else {
        PDBG("ProbeMinAlignment: 无满足阈值的候选 (最佳 confidence={:.2f})",
             m_Phase2MinAlignCandidates.empty() ? 0.0f : m_Phase2MinAlignCandidates[0].confidence);
    }
    PDBG("<<<<<<<<<< [ProbeMinAlignment] END <<<<<<<<<<");
}

void UEProber::Phase2_ProbePropertiesSize(uintptr_t execUbergraph, uintptr_t objectUClass) {
    PDBG(">>>>>>>>>> [ProbePropertiesSize] BEGIN >>>>>>>>>>");
    m_Phase2SizeCandidates.clear();
    int32_t alignOffset = GetConfirmedOffset("UStruct::MinAlignment");
    PDBG("ProbePropertiesSize: execUbergraph={}, objectUClass={}, alignOffset=0x{:X}",
         FormatPtr(execUbergraph), FormatPtr(objectUClass), alignOffset);
    if (alignOffset < 0) { PDBG("ProbePropertiesSize: alignOffset < 0, 中止"); PDBG("<<<<<<<<<< [ProbePropertiesSize] END <<<<<<<<<<"); return; }

    int scannedCount = 0;
    // Size 通常紧邻 MinAlignment (±32 字节)
    for (int32_t off = alignOffset - 32; off <= alignOffset + 32; off += 4) {
        if (off == alignOffset) continue;
        if (off < 0) continue;

        int32_t valExec = 0;
        if (!ReadMem(execUbergraph + off, &valExec, 4)) continue;
        scannedCount++;
        if (valExec != 0x4) continue;

        // 交叉验证: "Object" UClass 在同一偏移应为合理的 UObject 大小
        float confidence = 0.8f;
        std::string desc = std::format("偏移 0x{:X}: ExecUbergraph=0x{:X}", off, valExec);

        int32_t valObject = 0;
        if (objectUClass && ReadMem(objectUClass + off, &valObject, 4)) {
            if (valObject > 0x20 && valObject < 0x200 && (valObject % 8 == 0)) {
                confidence = 1.0f;
                desc += std::format(", Object=0x{:X}(合理)", valObject);
            } else {
                confidence = 0.5f;
                desc += std::format(", Object=0x{:X}(不合理)", valObject);
            }
        }

        PDBG("ProbePropertiesSize: off=0x{:X} valExec=0x{:X} valObject=0x{:X} conf={:.2f}",
             off, valExec, valObject, confidence);
        m_Phase2SizeCandidates.push_back({off, (uint64_t)valExec, desc, confidence});

        if (confidence >= 1.0f) break;
    }

    PDBG("ProbePropertiesSize: 扫描完毕, 候选数={}, 扫描偏移数={}",
         m_Phase2SizeCandidates.size(), scannedCount);

    std::sort(m_Phase2SizeCandidates.begin(), m_Phase2SizeCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase2SizeCandidates.empty() && m_Phase2SizeCandidates[0].confidence >= 0.8f) {
        auto& best = m_Phase2SizeCandidates[0];
        auto& r = GetResult("UStruct::PropertiesSize");
        r.offset = best.offset; r.size = 4; r.typeName = "int32";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbePropertiesSize: 选定 offset=0x{:X}, confidence={:.2f}, confirmed={}",
             best.offset, best.confidence, r.confirmed);
    } else {
        PDBG("ProbePropertiesSize: 无满足阈值的候选 (最佳 confidence={:.2f})",
             m_Phase2SizeCandidates.empty() ? 0.0f : m_Phase2SizeCandidates[0].confidence);
    }
    PDBG("<<<<<<<<<< [ProbePropertiesSize] END <<<<<<<<<<");
}

void UEProber::Phase2_ProbeChildren(uintptr_t classAddr) {
    PDBG(">>>>>>>>>> [ProbeChildren] BEGIN >>>>>>>>>>");
    m_Phase2ChildrenCandidates.clear();
    int32_t namePrivateOffset = GetConfirmedOffset("UObject::NamePrivate");
    PDBG("ProbeChildren: classAddr={}, namePrivateOffset=0x{:X}", FormatPtr(classAddr), namePrivateOffset);
    if (namePrivateOffset < 0) { PDBG("ProbeChildren: namePrivateOffset < 0, 中止"); PDBG("<<<<<<<<<< [ProbeChildren] END <<<<<<<<<<"); return; }

    // Children 是 UStruct 成员, 搜索范围: sizeof(UObject) ~ sizeof(UStruct)
    // MinAlignment/Size 已优先探测, Size 必然已确认
    int32_t searchStart = GetStructSize("UObject");
    int32_t searchEnd = GetStructSize("UStruct");
    if (searchEnd <= searchStart) searchEnd = searchStart * 3;
    PDBG("ProbeChildren: 搜索范围 [0x{:X}, 0x{:X})", searchStart, searchEnd);
    if (searchStart <= 0) {
        PDBG("ProbeChildren: 搜索范围无效 (UObject size=0x{:X})", searchStart);
    }

    int scannedCount = 0, validPtrCount = 0;

    for (int32_t off = searchStart; off < searchEnd; off += 8) {
        uintptr_t ptr = 0;
        if (!ReadMem(classAddr + off, &ptr, 8)) continue;
        if (!IsValidPtr(ptr)) continue;
        scannedCount++;
        validPtrCount++;

        std::string name;
        if (TryReadFName(ptr + namePrivateOffset, name) && !name.empty()) {
            float confidence = 0.0f;
            if (FNameEq(name, "ExecuteUbergraph"))
                confidence = 1.0f;
            else {
                // 检查 Class->Name 是否为 "Function"
                int32_t classPrivateOffset = GetConfirmedOffset("UObject::ClassPrivate");
                if (classPrivateOffset >= 0) {
                    uintptr_t childClass = 0;
                    if (ReadMem(ptr + classPrivateOffset, &childClass, 8) && IsValidPtr(childClass)) {
                        std::string className;
                        if (TryReadFName(childClass + namePrivateOffset, className) && FNameEq(className, "Function"))
                            confidence = 0.5f;
                    }
                }
                if (confidence == 0.0f)
                    confidence = 0.1f;
            }

            PDBG("ProbeChildren: off=0x{:X} ptr={} name=\"{}\" conf={:.2f}",
                 off, FormatPtr(ptr), name, confidence);

            m_Phase2ChildrenCandidates.push_back({
                off, ptr,
                std::format("偏移 0x{:X} -> {} -> Name = \"{}\"", off, FormatPtr(ptr), name),
                confidence
            });

            if (confidence >= 1.0f) break;
        }
    }

    PDBG("ProbeChildren: 扫描完毕, 有效指针={}, 候选数={}", validPtrCount, m_Phase2ChildrenCandidates.size());

    std::sort(m_Phase2ChildrenCandidates.begin(), m_Phase2ChildrenCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase2ChildrenCandidates.empty() && m_Phase2ChildrenCandidates[0].confidence >= 0.8f) {
        auto& best = m_Phase2ChildrenCandidates[0];
        auto& r = GetResult("UStruct::Children");
        r.offset = best.offset; r.size = 8; r.typeName = "UField*";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeChildren: 选定 offset=0x{:X}, confidence={:.2f}, confirmed={}",
             best.offset, best.confidence, r.confirmed);
    } else {
        PDBG("ProbeChildren: 无满足阈值的候选 (最佳 confidence={:.2f})",
             m_Phase2ChildrenCandidates.empty() ? 0.0f : m_Phase2ChildrenCandidates[0].confidence);
    }
    PDBG("<<<<<<<<<< [ProbeChildren] END <<<<<<<<<<");
}

void UEProber::Phase2_ProbeChildProperties(uintptr_t classAddr) {
    PDBG(">>>>>>>>>> [ProbeChildProperties] BEGIN >>>>>>>>>>");
    m_Phase2ChildPropsCandidates.clear();
    int32_t childrenOffset = GetConfirmedOffset("UStruct::Children");
    PDBG("ProbeChildProperties: classAddr={}, childrenOffset=0x{:X}",
         FormatPtr(classAddr), childrenOffset);
    if (childrenOffset < 0) { PDBG("ProbeChildProperties: childrenOffset < 0, 中止"); PDBG("<<<<<<<<<< [ProbeChildProperties] END <<<<<<<<<<"); return; }

    // 从 Children ("ExecuteUbergraph") 函数中探测 ChildProperties,
    // 其 ChildProperties 的第一个 FField Name 应为 "EntryPoint"
    uintptr_t childrenAddr = 0;
    if (!ReadMem(classAddr + childrenOffset, &childrenAddr, 8) || !IsValidPtr(childrenAddr)) {
        PDBG("ProbeChildProperties: 无法读取 Children 地址, ReadMem(classAddr+0x{:X}) 失败或 childrenAddr={} 无效",
             childrenOffset, FormatPtr(childrenAddr));
        PDBG("<<<<<<<<<< [ProbeChildProperties] END <<<<<<<<<<");
        return;
    }

    PDBG("ProbeChildProperties: childrenAddr(ExecuteUbergraph)={}", FormatPtr(childrenAddr));

    // ChildProperties 是 UStruct 成员, 搜索范围: sizeof(UObject) ~ sizeof(UStruct)
    int32_t searchStart = GetStructSize("UObject");
    int32_t searchEnd = GetStructSize("UStruct");
    if (searchEnd <= searchStart) searchEnd = searchStart * 3;
    PDBG("ProbeChildProperties: 搜索范围 [0x{:X}, 0x{:X})", searchStart, searchEnd);

    for (int32_t off = searchStart; off < searchEnd; off += 8) {
        // 跳过已知的 Children 偏移
        if (off == childrenOffset) continue;

        uintptr_t ptr = 0;
        if (!ReadMem(childrenAddr + off, &ptr, 8)) continue;
        if (!IsValidPtr(ptr)) continue;

        // ChildProperties 指向 FField, 不是 UObject
        // 在 FField 的不同偏移处尝试读 FName
        float bestConf = 0.0f;
        std::string bestDesc;
        for (int namePrivateOff = 0x18; namePrivateOff <= 0x30; namePrivateOff += 4) {
            std::string name;
            if (TryReadFName(ptr + namePrivateOff, name) && !name.empty()) {
                float conf = 0.0f;
                if (FNameEq(name, "EntryPoint"))
                    conf = 1.0f;
                else
                    conf = 0.3f;
                if (conf > bestConf) {
                    bestConf = conf;
                    bestDesc = std::format("偏移 0x{:X} -> {} -> FField Name@0x{:X} = \"{}\"",
                        off, FormatPtr(ptr), namePrivateOff, name);
                }
                if (conf >= 1.0f) break;
            }
        }

        if (bestConf > 0.05f) {
            PDBG("ProbeChildProperties: off=0x{:X} ptr={} bestName=\"{}\" conf={:.2f}",
                 off, FormatPtr(ptr), bestDesc, bestConf);
            m_Phase2ChildPropsCandidates.push_back({off, ptr, bestDesc, bestConf});
        }

        if (bestConf >= 1.0f) break;
    }

    PDBG("ProbeChildProperties: 候选数={}", m_Phase2ChildPropsCandidates.size());

    std::sort(m_Phase2ChildPropsCandidates.begin(), m_Phase2ChildPropsCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase2ChildPropsCandidates.empty() && m_Phase2ChildPropsCandidates[0].confidence >= 0.8f) {
        auto& best = m_Phase2ChildPropsCandidates[0];
        auto& r = GetResult("UStruct::ChildProperties");
        r.offset = best.offset; r.size = 8; r.typeName = "FField*";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeChildProperties: 选定 offset=0x{:X}, confidence={:.2f}, confirmed={}",
             best.offset, best.confidence, r.confirmed);
    } else {
        PDBG("ProbeChildProperties: 无满足阈值的候选 (最佳 confidence={:.2f})",
             m_Phase2ChildPropsCandidates.empty() ? 0.0f : m_Phase2ChildPropsCandidates[0].confidence);
    }
    PDBG("<<<<<<<<<< [ProbeChildProperties] END <<<<<<<<<<");
}

void UEProber::Phase2_ProbeUFieldNext(uintptr_t /*unused*/) {
    PDBG(">>>>>>>>>> [ProbeUFieldNext] BEGIN >>>>>>>>>>");
    m_Phase2NextCandidates.clear();
    int32_t namePrivateOffset = GetConfirmedOffset("UObject::NamePrivate");
    int32_t classPrivateOffset = GetConfirmedOffset("UObject::ClassPrivate");
    int32_t childrenOffset = GetConfirmedOffset("UStruct::Children");
    PDBG("ProbeUFieldNext: namePrivateOffset=0x{:X}, classPrivateOffset=0x{:X}, childrenOffset=0x{:X}",
         namePrivateOffset, classPrivateOffset, childrenOffset);
    if (namePrivateOffset < 0 || classPrivateOffset < 0 || childrenOffset < 0) {
        PDBG("ProbeUFieldNext: 前置偏移不全, 中止");
        PDBG("<<<<<<<<<< [ProbeUFieldNext] END <<<<<<<<<<");
        return;
    }

    PDBG("ProbeUFieldNext: 在 GObjects 中搜索 KismetSystemLibrary ...");
    uintptr_t kismetClass = FindObjectInGObjects("KismetSystemLibrary", "Class");
    if (!kismetClass) {
        PDBG("ProbeUFieldNext: FindObjectInGObjects(KismetSystemLibrary) 返回 0, 中止");
        PDBG("<<<<<<<<<< [ProbeUFieldNext] END <<<<<<<<<<");
        return;
    }
    PDBG("ProbeUFieldNext: KismetSystemLibrary @ {}", FormatPtr(kismetClass));

    // 读取 UClass 的 Children -> 第一个 UFunction
    uintptr_t firstFunc = 0;
    if (!ReadMem(kismetClass + childrenOffset, &firstFunc, 8) || !IsValidPtr(firstFunc)) {
        PDBG("ProbeUFieldNext: ReadMem(kismetClass+0x{:X}) 失败或 firstFunc={} 无效",
             childrenOffset, FormatPtr(firstFunc));
        PDBG("<<<<<<<<<< [ProbeUFieldNext] END <<<<<<<<<<");
        return;
    }
    std::string firstFuncName;
    if (!TryReadFName(firstFunc + namePrivateOffset, firstFuncName) || firstFuncName.empty()) {
        PDBG("ProbeUFieldNext: TryReadFName(firstFunc+0x{:X}) 失败, firstFunc={}",
             namePrivateOffset, FormatPtr(firstFunc));
        PDBG("<<<<<<<<<< [ProbeUFieldNext] END <<<<<<<<<<");
        return;
    }
    PDBG("ProbeUFieldNext: Children 第一个函数 \"{}\" @ {}",
         firstFuncName, FormatPtr(firstFunc));

    // UField::Next 是 UField 成员, 搜索范围: sizeof(UObject) ~ sizeof(UField)
    int32_t nextSearchStart = GetStructSize("UObject");
    int32_t nextSearchEnd = GetStructSize("UField");
    PDBG("ProbeUFieldNext: 搜索范围 [0x{:X}, 0x{:X})", nextSearchStart, nextSearchEnd);

    for (int32_t off = nextSearchStart; off < nextSearchEnd; off += 8) {
        uintptr_t nextPtr = 0;
        if (!ReadMem(firstFunc + off, &nextPtr, 8)) continue;
        if (!IsValidPtr(nextPtr)) continue;

        std::string nextName;
        if (!TryReadFName(nextPtr + namePrivateOffset, nextName) || nextName.empty()) continue;

        // 找到了! 沿链遍历计算链长
        int chainLen = 2;
        uintptr_t cur = nextPtr;
        std::string lastFuncName = nextName;
        for (int j = 0; j < 200; ++j) {
            uintptr_t nn = 0;
            if (!ReadMem(cur + off, &nn, 8) || !IsValidPtr(nn)) break;
            std::string nnName;
            if (!TryReadFName(nn + namePrivateOffset, nnName) || nnName.empty()) break;
            chainLen++;
            lastFuncName = nnName;
            cur = nn;
        }

        float confidence = std::min(1.0f, chainLen / 3.0f);
        PDBG("ProbeUFieldNext: off=0x{:X} firstNext=\"{}\" lastFunc=\"{}\" chainLen={} conf={:.2f}",
             off, nextName, lastFuncName, chainLen, confidence);
        m_Phase2NextCandidates.push_back({
            off, (uint64_t)firstFunc,
            std::format("KismetSystemLibrary Children \"{}\" -> \"{}\" -> ... -> \"{}\" (链长={}, 偏移 0x{:X})",
                firstFuncName, nextName, lastFuncName, chainLen, off),
            confidence
        });

        if (confidence >= 1.0f) {
            break;
        }
    }

    PDBG("ProbeUFieldNext: 候选数={}", m_Phase2NextCandidates.size());

    std::sort(m_Phase2NextCandidates.begin(), m_Phase2NextCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase2NextCandidates.empty() && m_Phase2NextCandidates[0].confidence >= 0.8f) {
        auto& best = m_Phase2NextCandidates[0];
        auto& r = GetResult("UField::Next");
        r.offset = best.offset; r.size = 8; r.typeName = "UField*";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeUFieldNext: 选定 offset=0x{:X}, confidence={:.2f}, confirmed={}",
             best.offset, best.confidence, r.confirmed);
    } else {
        PDBG("ProbeUFieldNext: 无满足阈值的候选 (最佳 confidence={:.2f})",
             m_Phase2NextCandidates.empty() ? 0.0f : m_Phase2NextCandidates[0].confidence);
    }
    PDBG("<<<<<<<<<< [ProbeUFieldNext] END <<<<<<<<<<");
}

void UEProber::Phase2_AutoProbe() {
    m_PhaseStatus[2] = EPhaseStatus::InProgress;

    PDBG("========== [Phase2_AutoProbe] BEGIN ==========");
    PDBG("Phase2 开始, 检查前置条件: Name={}, Class={}",
         HasConfirmed("UObject::NamePrivate"), HasConfirmed("UObject::ClassPrivate"));

    if (!HasConfirmed("UObject::NamePrivate") || !HasConfirmed("UObject::ClassPrivate")) {
        PDBG("Phase2 中止: 需要先完成阶段1 (Name={}, Class={})",
             HasConfirmed("UObject::NamePrivate"), HasConfirmed("UObject::ClassPrivate"));
        return;
    }

    int32_t namePrivateOffset = GetConfirmedOffset("UObject::NamePrivate");
    int32_t classPrivateOffset = GetConfirmedOffset("UObject::ClassPrivate");
    PDBG("namePrivateOffset=0x{:X}, classPrivateOffset=0x{:X}", namePrivateOffset, classPrivateOffset);

    uintptr_t obj0 = reinterpret_cast<uintptr_t>(UObject::GObjects->GetByIndex(0));
    PDBG("obj[0] = {}", FormatPtr(obj0));
    if (!obj0) { PDBG("Phase2 中止: obj[0] 为空"); return; }

    uintptr_t obj1 = reinterpret_cast<uintptr_t>(UObject::GObjects->GetByIndex(1));
    PDBG("obj[1] = {}", FormatPtr(obj1));
    if (!obj1) { PDBG("Phase2 中止: obj[1] 为空"); return; }

    // === MinAlignment & Size (最优先探测, 为所有后续探测提供精确 size 边界) ===
    PDBG("---------- [Step 1/5] MinAlignment & Size ----------");
    PDBG("正在 GObjects 中搜索 ExecuteUbergraph...");
    uintptr_t execUbergraph = FindObjectInGObjects("ExecuteUbergraph");
    if (!execUbergraph) {
        PDBG("FindObjectInGObjects(ExecuteUbergraph) 返回 0, MinAlignment/Size 探测将跳过");
    } else {
        PDBG("ExecuteUbergraph = {}, 开始探测 MinAlignment", FormatPtr(execUbergraph));
        Phase2_ProbeMinAlignment(execUbergraph, obj1);
        bool minAlignOk = HasConfirmed("UStruct::MinAlignment");
        PDBG("MinAlignment 探测结果: confirmed={}", minAlignOk);
        if (minAlignOk) {
            PDBG("开始探测 Size");
            Phase2_ProbePropertiesSize(execUbergraph, obj1);
            PDBG("Size 探测结果: confirmed={}", HasConfirmed("UStruct::PropertiesSize"));
        } else {
            PDBG("MinAlignment 未确认, 跳过 Size 探测");
        }
    }

    // 读取 sizeof(UObject) — 用于后续探测的搜索范围
    int32_t sizeofUObject = GetStructSize("UObject");
    PDBG("GetStructSize(UObject) = 0x{:X}", sizeofUObject);
    if (sizeofUObject <= 0) {
        PDBG("sizeof(UObject) 返回 0, 可能 UStruct::PropertiesSize 未确认或 UObject 未找到");
    }

    // === Super ===
    PDBG("---------- [Step 2/5] Super ----------");
    uintptr_t packageClass = 0;
    PDBG("读取 obj[0]+0x{:X} (Class 偏移) ...", classPrivateOffset);
    if (!ReadMem(obj0 + classPrivateOffset, &packageClass, 8) || !IsValidPtr(packageClass)) {
        PDBG("Phase2 中止: ReadMem(obj0+classPrivateOffset) 失败或 packageClass={} 无效", FormatPtr(packageClass));
        return;
    }
    std::string pkgClassName;
    TryReadFName(packageClass + namePrivateOffset, pkgClassName);
    PDBG("obj[0].Class = {}, Name=\"{}\"", FormatPtr(packageClass), pkgClassName);
    Phase2_ProbeSuperStruct(packageClass);
    PDBG("Super 探测结果: confirmed={}", HasConfirmed("UStruct::SuperStruct"));

    // === Children ===
    PDBG("---------- [Step 3/5] Children ----------");
    uintptr_t obj1Class = 0;
    PDBG("读取 obj[1]+0x{:X} (Class 偏移) ...", classPrivateOffset);
    if (!ReadMem(obj1 + classPrivateOffset, &obj1Class, 8) || !IsValidPtr(obj1Class)) {
        PDBG("Phase2 中止: ReadMem(obj1+classPrivateOffset) 失败或 obj1Class={} 无效", FormatPtr(obj1Class));
        return;
    }
    std::string obj1ClassName;
    TryReadFName(obj1Class + namePrivateOffset, obj1ClassName);
    if (!FNameEq(obj1ClassName, "Class")) {
        PDBG("obj[1].Class.Name 不是 Class, 实际=\"{}\"", obj1ClassName);
    }
    PDBG("obj[1] = {}, Class = {}, Class.Name=\"{}\"",
         FormatPtr(obj1), FormatPtr(obj1Class), obj1ClassName);
    Phase2_ProbeChildren(obj1);
    PDBG("Children 探测结果: confirmed={}", HasConfirmed("UStruct::Children"));

    // === ChildProperties ===
    PDBG("---------- [Step 4/5] ChildProperties ----------");
    if (HasConfirmed("UStruct::Children")) {
        PDBG("开始探测 ChildProperties");
        Phase2_ProbeChildProperties(obj1);
        PDBG("ChildProperties 探测结果: confirmed={}", HasConfirmed("UStruct::ChildProperties"));
    } else {
        PDBG("Children 未确认, 跳过 ChildProperties 探测");
    }

    // === UField::Next ===
    PDBG("---------- [Step 5/5] UField::Next ----------");
    if (HasConfirmed("UStruct::Children")) {
        PDBG("开始探测 UField::Next");
        Phase2_ProbeUFieldNext(0);
        PDBG("UField::Next 探测结果: confirmed={}", HasConfirmed("UField::Next"));
    } else {
        PDBG("Children 未确认, 跳过 UField::Next 探测");
    }

    PDBG("Phase2 完成, 汇总: MinAlign={} Super={} Children={} ChildProps={} Next={}",
         HasConfirmed("UStruct::MinAlignment"), HasConfirmed("UStruct::SuperStruct"),
         HasConfirmed("UStruct::Children"), HasConfirmed("UStruct::ChildProperties"),
         HasConfirmed("UField::Next"));
    PDBG("========== [Phase2_AutoProbe] END ==========");
}

// ============================================================
//  阶段 3: UClass 成员
// ============================================================

void UEProber::Phase3_ProbeCastFlags() {
    m_Phase3CastFlagsCandidates.clear();
    LogInfo("探测 UClass::CastFlags");

    int32_t namePrivateOffset = GetConfirmedOffset("UObject::NamePrivate");
    int32_t classPrivateOffset = GetConfirmedOffset("UObject::ClassPrivate");
    int32_t superStructOffset = GetConfirmedOffset("UStruct::SuperStruct");
    if (namePrivateOffset < 0 || classPrivateOffset < 0) return;

    // 确保 m_ClassClass 已初始化
    if (!m_ClassClass) {
        // obj[1] (UObject 基类) -> Class = "Class" 元类
        uintptr_t obj1 = reinterpret_cast<uintptr_t>(UObject::GObjects->GetByIndex(1));
        if (!obj1) return;
        uintptr_t obj1Class = 0;
        ReadMem(obj1 + classPrivateOffset, &obj1Class, 8);
        if (!IsValidPtr(obj1Class)) return;
        m_ClassClass = obj1Class;
    }
    // 确保 m_ClassStruct 已初始化
    if (!m_ClassStruct && superStructOffset >= 0) {
        uintptr_t structAddr = 0;
        ReadMem(m_ClassClass + superStructOffset, &structAddr, 8);
        if (IsValidPtr(structAddr)) m_ClassStruct = structAddr;
    }

    // 获取 "Package" UClass: obj[0] 的 Class
    uintptr_t packageClass = 0;
    {
        uintptr_t obj0 = reinterpret_cast<uintptr_t>(UObject::GObjects->GetByIndex(0));
        if (obj0 && IsValidPtr(obj0)) {
            ReadMem(obj0 + classPrivateOffset, &packageClass, 8);
            if (!IsValidPtr(packageClass)) packageClass = 0;
        }
    }

    // 已知精确值: "Class" 元类 CastFlags = 0x29, "Package" UClass CastFlags = 0x0000000400000000
    // CastFlags 是 UClass 成员, 搜索范围: sizeof(UStruct) ~ sizeof(UClass)
    int32_t sizeofUStruct = GetStructSize("UStruct");
    int32_t sizeofUClass = GetStructSize("UClass");
    int32_t searchStart = sizeofUStruct;
    int32_t searchEnd = sizeofUClass;

    for (int32_t off = searchStart; off < searchEnd; off += 8) {
        uint64_t valClass = 0, valStruct = 0, valPackage = 0;
        bool readClass = m_ClassClass && ReadMem(m_ClassClass + off, &valClass, 8);
        bool readStruct = m_ClassStruct && ReadMem(m_ClassStruct + off, &valStruct, 8);
        bool readPackage = packageClass && ReadMem(packageClass + off, &valPackage, 8);

        if (!readClass) continue;
        if (valClass == 0) continue;

        float confidence = 0.0f;
        std::string desc;

        // "Class" 元类 CastFlags 应精确等于 0x29
        if (valClass == ECastFlags::KNOWN_CLASS_FLAGS) {
            confidence += 0.5f;
            desc += std::format("Class=0x{:X} (精确匹配 0x29); ", valClass);
        } else if (valClass & ECastFlags::CASTCLASS_UClass) {
            confidence += 0.2f;
            desc += std::format("Class=0x{:X} (含 CASTCLASS_UClass); ", valClass);
        }

        // "Struct" 应包含 CASTCLASS_UStruct 但不含 CASTCLASS_UClass
        if (readStruct && (valStruct & ECastFlags::CASTCLASS_UStruct) && !(valStruct & ECastFlags::CASTCLASS_UClass)) {
            confidence += 0.2f;
            desc += std::format("Struct=0x{:X}; ", valStruct);
        }

        // "Package" UClass CastFlags 应精确等于 0x0000000400000000
        if (readPackage && valPackage == ECastFlags::KNOWN_PACKAGE_FLAGS) {
            confidence += 0.3f;
            desc += std::format("Package=0x{:X} (精确匹配); ", valPackage);
        } else if (readPackage && valPackage != 0 && valPackage != valClass) {
            confidence += 0.1f;
            desc += std::format("Package=0x{:X}; ", valPackage);
        }

        if (confidence > 0.1f) {
            m_Phase3CastFlagsCandidates.push_back({
                off, valClass,
                std::format("偏移 0x{:X}: Class=0x{:X}, Struct=0x{:X} ({})",
                    off, valClass, valStruct, desc),
                confidence
            });
        }
    }

    std::sort(m_Phase3CastFlagsCandidates.begin(), m_Phase3CastFlagsCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase3CastFlagsCandidates.empty() && m_Phase3CastFlagsCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase3CastFlagsCandidates[0];
        auto& r = GetResult("UClass::CastFlags");
        r.offset = best.offset; r.size = 8; r.typeName = "EClassCastFlags";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        LogSuccess(std::format("UClass::CastFlags 自动探测: 偏移 0x{:X}{}",
            best.offset, r.confirmed ? " [已自动确认]" : ""));
    }
}

void UEProber::Phase3_ProbeClassDefaultObject(uintptr_t classAddr) {
    m_Phase2ClassDefaultObjCandidates.clear();

    LogInfo(std::format("探测 UClass::DefaultObject @ {}", FormatPtr(classAddr)));

    // 先在 GObjects 中找到 "Default__Object" 的地址, 然后直接做指针值比较
    uintptr_t defaultObj = FindObjectInGObjects("Default__Object");
    if (!defaultObj) {
        LogWarning("未在 GObjects 中找到 Default__Object");
        return;
    }

    LogInfo(std::format("Default__Object 地址: {}", FormatPtr(defaultObj)));

    // DefaultObject 是 UClass 成员, 搜索范围: sizeof(UStruct) ~ sizeof(UClass)
    int32_t searchStart = GetStructSize("UStruct");
    int32_t searchEnd = GetStructSize("UClass");

    for (int32_t off = searchStart; off < searchEnd; off += 8) {
        uintptr_t ptr = 0;
        if (!ReadMem(classAddr + off, &ptr, 8)) continue;

        float confidence = 0.0f;
        if (ptr == defaultObj)
            confidence = 1.0f;
        else if (IsValidPtr(ptr))
            confidence = 0.05f;

        if (confidence > 0.04f) {
            m_Phase2ClassDefaultObjCandidates.push_back({
                off, ptr,
                std::format("偏移 0x{:X} -> {} {}", off, FormatPtr(ptr),
                    ptr == defaultObj ? "== Default__Object [精确匹配]" : "(其他指针)"),
                confidence
            });
        }

        if (confidence >= 1.0f) break;
    }

    std::sort(m_Phase2ClassDefaultObjCandidates.begin(), m_Phase2ClassDefaultObjCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase2ClassDefaultObjCandidates.empty() && m_Phase2ClassDefaultObjCandidates[0].confidence >= 0.9f) {
        auto& best = m_Phase2ClassDefaultObjCandidates[0];
        auto& r = GetResult("UClass::ClassDefaultObject");
        r.offset = best.offset; r.size = 8; r.typeName = "UObject*";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        LogSuccess(std::format("UClass::ClassDefaultObject 自动探测: 偏移 0x{:X}{}",
            best.offset, r.confirmed ? " [已自动确认]" : ""));
    }
}

void UEProber::Phase3_AutoProbe() {
    m_PhaseStatus[3] = EPhaseStatus::InProgress;
    LogInfo("===== 阶段 3: UClass 自动探测 =====");

    Phase3_ProbeCastFlags();

    // === DefaultObject ===
    uintptr_t obj1 = reinterpret_cast<uintptr_t>(UObject::GObjects->GetByIndex(1));
    if (obj1 && IsValidPtr(obj1)) {
        Phase3_ProbeClassDefaultObject(obj1);
    }

    LogInfo("阶段 3 自动探测完成");
}

// ============================================================
//  阶段 4: UFunction 探测
//  锚点: ReceiveBeginPlay, ReceiveTick, IsValid, PrintString, K2_GetActorLocation
// ============================================================

// ---- 辅助: 沿 Children→Next 链查找指定名称的 UFunction ----
uintptr_t UEProber::WalkChildrenChain(
    uintptr_t classAddr, const std::string& funcName,
    int32_t childrenOff, int32_t nextOff, int32_t namePrivateOff)
{
    uintptr_t cur = 0;
    if (!ReadMem(classAddr + childrenOff, &cur, 8) || !IsValidPtr(cur))
        return 0;

    for (int i = 0; i < 500 && cur; ++i) {
        std::string n;
        if (TryReadFName(cur + namePrivateOff, n) && n == funcName)
            return cur;
        uintptr_t next = 0;
        if (!ReadMem(cur + nextOff, &next, 8)) break;
        cur = (next && IsValidPtr(next)) ? next : 0;
    }
    return 0;
}

void UEProber::Phase4_CollectAnchors() {
    int32_t namePrivateOff     = GetConfirmedOffset("UObject::NamePrivate");
    int32_t classPrivateOff    = GetConfirmedOffset("UObject::ClassPrivate");
    int32_t childrenOff = GetConfirmedOffset("UStruct::Children");
    int32_t nextOff     = GetConfirmedOffset("UField::Next");
    if (namePrivateOff < 0 || classPrivateOff < 0 || childrenOff < 0 || nextOff < 0) {
        LogError("缺少前置偏移 (Name/Class/Children/Next), 无法收集锚点");
        return;
    }

    // --- Actor Children 链: ReceiveBeginPlay, ReceiveTick, K2_GetActorLocation ---
    uintptr_t actorClass = FindObjectInGObjects("Actor", "Class");
    if (!actorClass) {
        LogWarning("未找到 Actor UClass");
    } else {
        LogInfo(std::format("Actor UClass @ {}", FormatPtr(actorClass)));
        if (!m_FuncReceiveBeginPlay)
            m_FuncReceiveBeginPlay = WalkChildrenChain(actorClass, "ReceiveBeginPlay", childrenOff, nextOff, namePrivateOff);
        if (!m_FuncReceiveTick)
            m_FuncReceiveTick = WalkChildrenChain(actorClass, "ReceiveTick", childrenOff, nextOff, namePrivateOff);
        if (!m_FuncK2_GetActorLocation)
            m_FuncK2_GetActorLocation = WalkChildrenChain(actorClass, "K2_GetActorLocation", childrenOff, nextOff, namePrivateOff);
    }

    // --- KismetSystemLibrary Children 链: IsValid, PrintString ---
    uintptr_t kismetClass = FindObjectInGObjects("KismetSystemLibrary", "Class");
    if (!kismetClass) {
        LogWarning("未找到 KismetSystemLibrary UClass");
    } else {
        LogInfo(std::format("KismetSystemLibrary UClass @ {}", FormatPtr(kismetClass)));
        if (!m_FuncIsValid)
            m_FuncIsValid = WalkChildrenChain(kismetClass, "IsValid", childrenOff, nextOff, namePrivateOff);
        if (!m_FuncPrintString)
            m_FuncPrintString = WalkChildrenChain(kismetClass, "PrintString", childrenOff, nextOff, namePrivateOff);
    }

    // 打印收集结果
    auto logAnchor = [&](const char* name, uintptr_t addr) {
        if (addr) LogSuccess(std::format("锚点 \"{}\" @ {}", name, FormatPtr(addr)));
        else      LogWarning(std::format("锚点 \"{}\" 未找到", name));
    };
    logAnchor("ReceiveBeginPlay",    m_FuncReceiveBeginPlay);
    logAnchor("ReceiveTick",         m_FuncReceiveTick);
    logAnchor("IsValid",             m_FuncIsValid);
    logAnchor("PrintString",         m_FuncPrintString);
    logAnchor("K2_GetActorLocation", m_FuncK2_GetActorLocation);
}

void UEProber::Phase4_ProbeFunctionFlags() {
    m_Phase4FuncFlagsCandidates.clear();
    LogInfo("探测 UFunction::FunctionFlags");

    if (!m_FuncIsValid || !m_FuncPrintString || !m_FuncReceiveBeginPlay) {
        LogWarning("需要 IsValid, PrintString, ReceiveBeginPlay 锚点");
        return;
    }

    int32_t sizeofUStruct = GetStructSize("UStruct");
    int32_t sizeofUFunction = GetStructSize("UFunction");
    int32_t searchStart = sizeofUStruct;
    int32_t searchEnd = sizeofUFunction;

    if (searchStart <= 0 || searchEnd <= 0 || searchStart >= searchEnd) {
        LogError(std::format("搜索范围无效: UStruct Size=0x{:X}, UFunction Size=0x{:X}", sizeofUStruct, sizeofUFunction));
        return;
    }

    // 期望标志位
    const uint32_t isValidExpected  = EFuncFlags::FUNC_Native | EFuncFlags::FUNC_Final | EFuncFlags::FUNC_BlueprintPure;
    const uint32_t printStrExpected = EFuncFlags::FUNC_Native | EFuncFlags::FUNC_Final | EFuncFlags::FUNC_HasDefaults;
    const uint32_t bpEventExpected  = EFuncFlags::FUNC_Event  | EFuncFlags::FUNC_BlueprintEvent;

    for (int32_t off = searchStart; off < searchEnd; off += 4) {
        uint32_t valIsValid = 0, valPrintStr = 0, valBeginPlay = 0;
        if (!ReadMemUnsafe(m_FuncIsValid + off, &valIsValid, 4)) continue;
        if (!ReadMemUnsafe(m_FuncPrintString + off, &valPrintStr, 4)) continue;
        if (!ReadMemUnsafe(m_FuncReceiveBeginPlay + off, &valBeginPlay, 4)) continue;

        // IsValid: 必含 Native|Final|BlueprintPure
        if ((valIsValid & isValidExpected) != isValidExpected) continue;
        // PrintString: 必含 Native|Final|HasDefaults, 不含 BlueprintPure
        if ((valPrintStr & printStrExpected) != printStrExpected) continue;
        if (valPrintStr & EFuncFlags::FUNC_BlueprintPure) continue;
        // ReceiveBeginPlay: 必含 Event|BlueprintEvent, 不含 Native
        if ((valBeginPlay & bpEventExpected) != bpEventExpected) continue;
        if (valBeginPlay & EFuncFlags::FUNC_Native) continue;

        float confidence = 0.8f;
        std::string desc = std::format("IsValid=0x{:X}, PrintString=0x{:X}, BeginPlay=0x{:X}", valIsValid, valPrintStr, valBeginPlay);

        // ReceiveTick 交叉验证: 应与 ReceiveBeginPlay 相同的事件标志
        if (m_FuncReceiveTick) {
            uint32_t valTick = 0;
            if (ReadMemUnsafe(m_FuncReceiveTick + off, &valTick, 4) &&
                (valTick & bpEventExpected) == bpEventExpected &&
                !(valTick & EFuncFlags::FUNC_Native)) {
                confidence += 0.1f;
                desc += std::format(", Tick=0x{:X}", valTick);
            }
        }
        // K2_GetActorLocation 交叉验证: 应含 Native|Final|BlueprintPure
        if (m_FuncK2_GetActorLocation) {
            uint32_t valLoc = 0;
            if (ReadMemUnsafe(m_FuncK2_GetActorLocation + off, &valLoc, 4) &&
                (valLoc & isValidExpected) == isValidExpected) {
                confidence += 0.1f;
                desc += std::format(", GetLoc=0x{:X}", valLoc);
            }
        }

        m_Phase4FuncFlagsCandidates.push_back({off, valIsValid, desc, confidence});
        if (confidence >= 1.0f) break;
    }

    std::sort(m_Phase4FuncFlagsCandidates.begin(), m_Phase4FuncFlagsCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase4FuncFlagsCandidates.empty() && m_Phase4FuncFlagsCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase4FuncFlagsCandidates[0];
        auto& r = GetResult("UFunction::FunctionFlags");
        r.offset = best.offset; r.size = 4; r.typeName = "EFunctionFlags";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        LogSuccess(std::format("UFunction::FunctionFlags 自动探测: 偏移 0x{:X}{}",
            best.offset, r.confirmed ? " [已自动确认]" : ""));
    } else {
        LogWarning("UFunction::FunctionFlags 探测失败, 未找到满足所有条件的偏移");
    }
}

void UEProber::Phase4_ProbeNumParmsAndParmsSize() {
    m_Phase4NumParmsCandidates.clear();
    m_Phase4ParmsSizeCandidates.clear();
    LogInfo("探测 UFunction::NumParms 和 ParmsSize");

    // 至少需要 3 个不同 NumParms 值的锚点
    if (!m_FuncReceiveBeginPlay || !m_FuncIsValid) {
        LogWarning("需要 ReceiveBeginPlay 和 IsValid 锚点");
        return;
    }

    int32_t funcFlagsOff = GetConfirmedOffset("UFunction::FunctionFlags");
    if (funcFlagsOff < 0) { LogWarning("需要先确认 FunctionFlags"); return; }

    int32_t searchStart = GetStructSize("UStruct");
    int32_t searchEnd = GetStructSize("UFunction");

    // === NumParms (uint8) ===
    // PrintString=6, IsValid=2, ReceiveTick=1, ReceiveBeginPlay=0
    for (int32_t off = searchStart; off < searchEnd; off += 1) {
        if (off >= funcFlagsOff && off < funcFlagsOff + 4) continue; // 跳过 FunctionFlags 本身
        uint8_t valBP = 0, valIsValid = 0;
        if (!ReadMemUnsafe(m_FuncReceiveBeginPlay + off, &valBP, 1)) continue;
        if (!ReadMemUnsafe(m_FuncIsValid + off, &valIsValid, 1)) continue;

        if (valBP != 0 || valIsValid != 2) continue;

        float confidence = 0.6f;
        std::string desc = std::format("BeginPlay=0, IsValid=2");

        if (m_FuncReceiveTick) {
            uint8_t valTick = 0;
            if (ReadMemUnsafe(m_FuncReceiveTick + off, &valTick, 1) && valTick == 1) {
                confidence += 0.15f;
                desc += ", Tick=1";
            } else {
                continue;
            }
        }
        if (m_FuncPrintString) {
            uint8_t valPS = 0;
            if (ReadMemUnsafe(m_FuncPrintString + off, &valPS, 1) && valPS == 6) {
                confidence += 0.25f;
                desc += ", PrintString=6";
            } else {
                continue;
            }
        }

        m_Phase4NumParmsCandidates.push_back({off, valIsValid, desc, confidence});
        if (confidence >= 1.0f) break;
    }

    // === ParmsSize (uint16) ===
    // IsValid=9, ReceiveTick=4, ReceiveBeginPlay=0
    for (int32_t off = searchStart; off < searchEnd; off += 2) {
        if (off >= funcFlagsOff && off < funcFlagsOff + 4) continue; // 跳过 FunctionFlags 本身
        uint16_t valBP = 0, valIsValid = 0;
        if (!ReadMemUnsafe(m_FuncReceiveBeginPlay + off, &valBP, 2)) continue;
        if (!ReadMemUnsafe(m_FuncIsValid + off, &valIsValid, 2)) continue;

        if (valBP != 0 || valIsValid != 9) continue;

        float confidence = 0.7f;
        std::string desc = std::format("BeginPlay=0, IsValid=9");

        if (m_FuncReceiveTick) {
            uint16_t valTick = 0;
            if (ReadMemUnsafe(m_FuncReceiveTick + off, &valTick, 2) && valTick == 4) {
                confidence += 0.15f;
                desc += ", Tick=4";
            } else {
            }
        }
        if (m_FuncPrintString) {
            uint16_t valPS = 0;
            if (ReadMemUnsafe(m_FuncPrintString + off, &valPS, 2) && valPS > 20) {
                confidence += 0.15f;
                desc += std::format(", PrintString={}", valPS);
            }
        }

        m_Phase4ParmsSizeCandidates.push_back({off, valIsValid, desc, confidence});
        if (confidence >= 1.0f) break;
    }

    std::sort(m_Phase4NumParmsCandidates.begin(), m_Phase4NumParmsCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });
    std::sort(m_Phase4ParmsSizeCandidates.begin(), m_Phase4ParmsSizeCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase4NumParmsCandidates.empty() && m_Phase4NumParmsCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase4NumParmsCandidates[0];
        auto& r = GetResult("UFunction::NumParms");
        r.offset = best.offset; r.size = 1; r.typeName = "uint8";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        LogSuccess(std::format("UFunction::NumParms 自动探测: 偏移 0x{:X}{}",
            best.offset, r.confirmed ? " [已自动确认]" : ""));
    }
    if (!m_Phase4ParmsSizeCandidates.empty() && m_Phase4ParmsSizeCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase4ParmsSizeCandidates[0];
        auto& r = GetResult("UFunction::ParmsSize");
        r.offset = best.offset; r.size = 2; r.typeName = "uint16";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        LogSuccess(std::format("UFunction::ParmsSize 自动探测: 偏移 0x{:X}{}",
            best.offset, r.confirmed ? " [已自动确认]" : ""));
    }
}

void UEProber::Phase4_ProbeReturnValueOffset() {
    m_Phase4ReturnValueOffCandidates.clear();
    LogInfo("探测 UFunction::ReturnValueOffset");

    // 需要三种不同值: K2_GetActorLocation=0, IsValid=8, PrintString=0xFFFF
    if (!m_FuncK2_GetActorLocation || !m_FuncIsValid || !m_FuncPrintString) {
        LogWarning("需要 K2_GetActorLocation, IsValid, PrintString 锚点");
        return;
    }

    int32_t funcFlagsOff = GetConfirmedOffset("UFunction::FunctionFlags");
    if (funcFlagsOff < 0) { LogWarning("需要先确认 FunctionFlags"); return; }
    int32_t numParmsOff = GetConfirmedOffset("UFunction::NumParms");
    int32_t parmsSizeOff = GetConfirmedOffset("UFunction::ParmsSize");

    int32_t searchStart = GetStructSize("UStruct");
    int32_t searchEnd = GetStructSize("UFunction");

    for (int32_t off = searchStart; off < searchEnd; off += 2) {
        if (off == numParmsOff || off == parmsSizeOff) continue;
        if (off >= funcFlagsOff && off < funcFlagsOff + 4) continue;

        uint16_t valLoc = 0, valIsValid = 0, valPS = 0;
        if (!ReadMemUnsafe(m_FuncK2_GetActorLocation + off, &valLoc, 2)) continue;
        if (!ReadMemUnsafe(m_FuncIsValid + off, &valIsValid, 2)) continue;
        if (!ReadMemUnsafe(m_FuncPrintString + off, &valPS, 2)) continue;

        // K2_GetActorLocation=0, IsValid=8, PrintString=0xFFFF
        if (valLoc != 0 || valIsValid != 8 || valPS != 0xFFFF) continue;

        float confidence = 0.9f;
        std::string desc = std::format("GetLoc=0, IsValid=8, PrintString=0xFFFF");

        // 交叉验证: ReceiveBeginPlay 和 ReceiveTick 应为 0xFFFF
        if (m_FuncReceiveBeginPlay) {
            uint16_t valBP = 0;
            if (ReadMemUnsafe(m_FuncReceiveBeginPlay + off, &valBP, 2) && valBP == 0xFFFF) {
                confidence += 0.05f;
                desc += ", BeginPlay=0xFFFF";
            }
        }
        if (m_FuncReceiveTick) {
            uint16_t valTick = 0;
            if (ReadMemUnsafe(m_FuncReceiveTick + off, &valTick, 2) && valTick == 0xFFFF) {
                confidence += 0.05f;
                desc += ", Tick=0xFFFF";
            }
        }

        m_Phase4ReturnValueOffCandidates.push_back({off, valLoc, desc, confidence});
        if (confidence >= 1.0f) break;
    }

    std::sort(m_Phase4ReturnValueOffCandidates.begin(), m_Phase4ReturnValueOffCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase4ReturnValueOffCandidates.empty() && m_Phase4ReturnValueOffCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase4ReturnValueOffCandidates[0];
        auto& r = GetResult("UFunction::ReturnValueOffset");
        r.offset = best.offset; r.size = 2; r.typeName = "uint16";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        LogSuccess(std::format("UFunction::ReturnValueOffset 自动探测: 偏移 0x{:X}{}",
            best.offset, r.confirmed ? " [已自动确认]" : ""));
    }
}

void UEProber::Phase4_ProbeFunc() {
    m_Phase4ExecFuncCandidates.clear();
    LogInfo("探测 UFunction::Func");

    if (!m_FuncIsValid || !m_FuncPrintString || !m_FuncReceiveBeginPlay) {
        LogWarning("需要 IsValid, PrintString, ReceiveBeginPlay 锚点");
        return;
    }

    uintptr_t textStart = GetTextSegStart();
    uintptr_t textEnd = GetTextSegEnd();

    int32_t sizeofUStruct = GetStructSize("UStruct");
    int32_t sizeofUFunction = GetStructSize("UFunction");

    // Native 锚点: IsValid, PrintString, K2_GetActorLocation
    std::vector<uintptr_t> nativeFuncs = {m_FuncIsValid, m_FuncPrintString};
    if (m_FuncK2_GetActorLocation) nativeFuncs.push_back(m_FuncK2_GetActorLocation);

    // 蓝图锚点: ReceiveBeginPlay, ReceiveTick
    std::vector<uintptr_t> bpFuncs = {m_FuncReceiveBeginPlay};
    if (m_FuncReceiveTick) bpFuncs.push_back(m_FuncReceiveTick);

    for (int32_t off = sizeofUStruct; off < sizeofUFunction; off += 8) {
        // 检查 Native 函数: 值应落在 .text 段且互不相同
        int nativeInText = 0;
        std::set<uintptr_t> nativeValues;
        for (auto fnAddr : nativeFuncs) {
            uintptr_t ptr = 0;
            if (ReadMemUnsafe(fnAddr + off, &ptr, 8) && ptr >= textStart && ptr < textEnd) {
                nativeInText++;
                nativeValues.insert(ptr);
            }
        }
        bool nativeAllDiff = (nativeValues.size() == nativeFuncs.size());

        // 检查 BP 函数: 值应完全相同
        std::set<uintptr_t> bpValues;
        for (auto fnAddr : bpFuncs) {
            uintptr_t ptr = 0;
            if (ReadMemUnsafe(fnAddr + off, &ptr, 8) && ptr != 0)
                bpValues.insert(ptr);
        }
        bool bpAllSame = (bpValues.size() == 1 && bpFuncs.size() >= 2);

        if (nativeInText < 2) continue;

        float confidence = 0.0f;
        if (nativeInText >= 3 && nativeAllDiff) confidence += 0.5f;
        else if (nativeInText >= 2 && nativeAllDiff) confidence += 0.4f;
        else if (nativeInText >= 2) confidence += 0.3f;

        if (bpAllSame) confidence += 0.5f;
        else if (bpValues.size() == 1) confidence += 0.3f; // 只有 1 个 BP 样本

        if (confidence < 0.5f) continue;

        m_Phase4ExecFuncCandidates.push_back({
            off, 0,
            std::format("Native在.text={}/{} 互不同={}, BP值相同={}({}个)",
                nativeInText, (int)nativeFuncs.size(), nativeAllDiff, bpAllSame, (int)bpFuncs.size()),
            confidence
        });
        if (confidence >= 1.0f) break;
    }

    std::sort(m_Phase4ExecFuncCandidates.begin(), m_Phase4ExecFuncCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase4ExecFuncCandidates.empty() && m_Phase4ExecFuncCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase4ExecFuncCandidates[0];
        auto& r = GetResult("UFunction::Func");
        r.offset = best.offset; r.size = 8; r.typeName = "FNativeFuncPtr";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        LogSuccess(std::format("UFunction::Func 自动探测: 偏移 0x{:X}{}",
            best.offset, r.confirmed ? " [已自动确认]" : ""));
    }
}

void UEProber::Phase4_AutoProbe() {
    m_PhaseStatus[4] = EPhaseStatus::InProgress;
    LogInfo("===== 阶段 4: UFunction 自动探测 =====");

    if (!HasConfirmed("UObject::NamePrivate") || !HasConfirmed("UObject::ClassPrivate") ||
        !HasConfirmed("UStruct::Children") || !HasConfirmed("UField::Next")) {
        LogError("需要先完成阶段 1~2 (确认 Name, Class, Children, Next 偏移)");
        return;
    }

    // === 收集 5 个锚点函数 ===
    Phase4_CollectAnchors();

    int anchorCount = 0;
    if (m_FuncReceiveBeginPlay)    anchorCount++;
    if (m_FuncReceiveTick)         anchorCount++;
    if (m_FuncIsValid)             anchorCount++;
    if (m_FuncPrintString)         anchorCount++;
    if (m_FuncK2_GetActorLocation) anchorCount++;

    if (anchorCount < 3) {
        LogError(std::format("只找到 {} 个锚点, 至少需要 3 个", anchorCount));
        return;
    }

    // === 按依赖顺序探测 ===
    Phase4_ProbeFunctionFlags();

    if (HasConfirmed("UFunction::FunctionFlags")) {
        Phase4_ProbeNumParmsAndParmsSize();
        Phase4_ProbeReturnValueOffset();
        Phase4_ProbeFunc();
    }

    LogInfo("阶段 4 自动探测完成");
}

// ============================================================
//  阶段 5: FField / FProperty
// ============================================================

void UEProber::Phase5_CollectAnchors() {
    PDBG(">>>>>>>>>> [CollectAnchors] BEGIN >>>>>>>>>>");

    int32_t childPropsOff = GetConfirmedOffset("UStruct::ChildProperties");
    if (childPropsOff < 0) {
        PDBG("CollectAnchors: ChildProperties 未确认, 中止");
        PDBG("<<<<<<<<<< [CollectAnchors] END <<<<<<<<<<");
        return;
    }
    PDBG("CollectAnchors: childPropsOff=0x{:X}", childPropsOff);

    // ExecuteUbergraph -> "EntryPoint" (IntProperty)
    uintptr_t execUbergraph = FindObjectInGObjects("ExecuteUbergraph");
    PDBG("CollectAnchors: FindObjectInGObjects(ExecuteUbergraph) = {}", FormatPtr(execUbergraph));
    if (execUbergraph) {
        ReadMem(execUbergraph + childPropsOff, &m_FFEntryPoint, 8);
        if (!IsValidPtr(m_FFEntryPoint)) m_FFEntryPoint = 0;
    }
    PDBG("CollectAnchors: m_FFEntryPoint={}", FormatPtr(m_FFEntryPoint));

    // ReceiveTick -> "DeltaSeconds" (FloatProperty)
    PDBG("CollectAnchors: m_FuncReceiveTick={}", FormatPtr(m_FuncReceiveTick));
    if (m_FuncReceiveTick) {
        ReadMem(m_FuncReceiveTick + childPropsOff, &m_FFDeltaSeconds, 8);
        if (!IsValidPtr(m_FFDeltaSeconds)) m_FFDeltaSeconds = 0;
    }
    PDBG("CollectAnchors: m_FFDeltaSeconds={}", FormatPtr(m_FFDeltaSeconds));

    // IsValid -> 首参 (ObjectProperty) -> Next -> ReturnValue (BoolProperty)
    PDBG("CollectAnchors: m_FuncIsValid={}", FormatPtr(m_FuncIsValid));
    if (m_FuncIsValid) {
        ReadMem(m_FuncIsValid + childPropsOff, &m_FFIsValidParam0, 8);
        if (!IsValidPtr(m_FFIsValidParam0)) m_FFIsValidParam0 = 0;
        // IsValid ReturnValue 在 Next 探测后获取
    }
    PDBG("CollectAnchors: m_FFIsValidParam0={}", FormatPtr(m_FFIsValidParam0));

    // K2_GetActorLocation -> "ReturnValue" (StructProperty)
    PDBG("CollectAnchors: m_FuncK2_GetActorLocation={}", FormatPtr(m_FuncK2_GetActorLocation));
    if (m_FuncK2_GetActorLocation) {
        ReadMem(m_FuncK2_GetActorLocation + childPropsOff, &m_FFK2LocReturn, 8);
        if (!IsValidPtr(m_FFK2LocReturn)) m_FFK2LocReturn = 0;
    }
    PDBG("CollectAnchors: m_FFK2LocReturn={}", FormatPtr(m_FFK2LocReturn));

    // 预验证: ReceiveBeginPlay 的 ChildProperties 应为 nullptr (NumParms=0)
    PDBG("CollectAnchors: m_FuncReceiveBeginPlay={}", FormatPtr(m_FuncReceiveBeginPlay));
    if (m_FuncReceiveBeginPlay) {
        uintptr_t bpChildProps = 0;
        ReadMem(m_FuncReceiveBeginPlay + childPropsOff, &bpChildProps, 8);
        if (bpChildProps != 0) {
            PDBG("CollectAnchors: WARNING ReceiveBeginPlay ChildProperties 不为 nullptr: {}, 可能 ChildProperties 偏移有误",
                FormatPtr(bpChildProps));
        } else {
            PDBG("CollectAnchors: ReceiveBeginPlay ChildProperties = nullptr (预验证通过)");
        }
    }

    PDBG("CollectAnchors: 汇总: EntryPoint={}, DeltaSeconds={}, IsValidP0={}, K2LocRV={}",
        FormatPtr(m_FFEntryPoint), FormatPtr(m_FFDeltaSeconds),
        FormatPtr(m_FFIsValidParam0), FormatPtr(m_FFK2LocReturn));
    PDBG("<<<<<<<<<< [CollectAnchors] END <<<<<<<<<<");
}

void UEProber::Phase5_ProbeFFieldNamePrivate() {
    PDBG(">>>>>>>>>> [ProbeFFieldNamePrivate] BEGIN >>>>>>>>>>");
    m_Phase5FFieldNamePrivateCandidates.clear();

    if (!m_FFEntryPoint) {
        PDBG("ProbeFFieldNamePrivate: 缺少 EntryPoint 锚点, 中止");
        PDBG("<<<<<<<<<< [ProbeFFieldNamePrivate] END <<<<<<<<<<");
        return;
    }
    PDBG("ProbeFFieldNamePrivate: EntryPoint={}, DeltaSeconds={}",
         FormatPtr(m_FFEntryPoint), FormatPtr(m_FFDeltaSeconds));

    for (int32_t off = 0x08; off < 0x40; off += 4) {
        std::string name1;
        if (!TryReadFName(m_FFEntryPoint + off, name1) || !FNameEq(name1, "EntryPoint")) {
            PDBG("ProbeFFieldNamePrivate: off=0x{:X} name1=\"{}\" 不匹配", off, name1);
            continue;
        }
        PDBG("ProbeFFieldNamePrivate: off=0x{:X} 命中 EntryPoint! name1=\"{}\"", off, name1);

        float confidence = 0.7f;
        std::string desc = std::format("EntryPoint Name=\"{}\"", name1);

        // 交叉验证: DeltaSeconds
        if (m_FFDeltaSeconds) {
            std::string name2;
            if (TryReadFName(m_FFDeltaSeconds + off, name2) && FNameEq(name2, "DeltaSeconds")) {
                confidence += 0.3f;
                desc += ", DeltaSeconds OK";
                PDBG("ProbeFFieldNamePrivate: off=0x{:X} DeltaSeconds 交叉验证通过, conf={:.2f}", off, confidence);
            } else {
                PDBG("ProbeFFieldNamePrivate: off=0x{:X} DeltaSeconds 交叉验证失败 name2=\"{}\", 跳过", off, name2);
                continue; // 两个锚点必须同时命中
            }
        }

        m_Phase5FFieldNamePrivateCandidates.push_back({off, 0, desc, confidence});
        PDBG("ProbeFFieldNamePrivate: 添加候选 off=0x{:X} conf={:.2f}", off, confidence);
        if (confidence >= 1.0f) break;
    }

    std::sort(m_Phase5FFieldNamePrivateCandidates.begin(), m_Phase5FFieldNamePrivateCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    PDBG("ProbeFFieldNamePrivate: 候选数={}", m_Phase5FFieldNamePrivateCandidates.size());
    if (!m_Phase5FFieldNamePrivateCandidates.empty() && m_Phase5FFieldNamePrivateCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase5FFieldNamePrivateCandidates[0];
        auto& r = GetResult("FField::NamePrivate");
        r.offset = best.offset; r.size = 8; r.typeName = "FName";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeFFieldNamePrivate: 选定 offset=0x{:X}, conf={:.2f}, confirmed={}",
             best.offset, best.confidence, r.confirmed);
    } else {
        PDBG("ProbeFFieldNamePrivate: 无满足阈值的候选 (最佳 conf={:.2f})",
             m_Phase5FFieldNamePrivateCandidates.empty() ? 0.0f : m_Phase5FFieldNamePrivateCandidates[0].confidence);
    }
    PDBG("<<<<<<<<<< [ProbeFFieldNamePrivate] END <<<<<<<<<<");
}

void UEProber::Phase5_ProbeFFieldOwner() {
    PDBG(">>>>>>>>>> [ProbeFFieldOwner] BEGIN >>>>>>>>>>");
    m_Phase5FFieldOwnerCandidates.clear();

    if (!m_FFEntryPoint) {
        PDBG("ProbeFFieldOwner: 缺少 EntryPoint 锚点, 中止");
        PDBG("<<<<<<<<<< [ProbeFFieldOwner] END <<<<<<<<<<");
        return;
    }

    uintptr_t execUbergraph = FindObjectInGObjects("ExecuteUbergraph");
    if (!execUbergraph) {
        PDBG("ProbeFFieldOwner: 未找到 ExecuteUbergraph, 中止");
        PDBG("<<<<<<<<<< [ProbeFFieldOwner] END <<<<<<<<<<");
        return;
    }
    PDBG("ProbeFFieldOwner: EntryPoint={}, execUbergraph={}",
         FormatPtr(m_FFEntryPoint), FormatPtr(execUbergraph));

    int32_t namePrivateOff = GetConfirmedOffset("FField::NamePrivate");
    PDBG("ProbeFFieldOwner: namePrivateOff=0x{:X}", namePrivateOff);

    for (int32_t off = 0x00; off < 0x30; off += 8) {
        if (off == namePrivateOff) continue; // 排除已确认的 Name 偏移

        uintptr_t ptr1 = 0;
        if (!ReadMem(m_FFEntryPoint + off, &ptr1, 8)) continue;
        if (ptr1 != execUbergraph) {
            PDBG("ProbeFFieldOwner: off=0x{:X} ptr1={} != execUbergraph, 跳过", off, FormatPtr(ptr1));
            continue;
        }
        PDBG("ProbeFFieldOwner: off=0x{:X} 命中! ptr1 == execUbergraph", off);

        float confidence = 0.7f;
        std::string desc = std::format("EntryPoint Owner -> ExecuteUbergraph");

        // 交叉验证: DeltaSeconds 的 Owner 应指向 ReceiveTick
        if (m_FFDeltaSeconds && m_FuncReceiveTick) {
            uintptr_t ptr2 = 0;
            if (ReadMem(m_FFDeltaSeconds + off, &ptr2, 8) && ptr2 == m_FuncReceiveTick) {
                confidence += 0.2f;
                desc += ", DeltaSeconds->ReceiveTick OK";
            }
        }

        // 检查 bIsUObject 标志 (紧随指针之后的 8 字节最低位)
        uint64_t bIsUObj = 0;
        if (ReadMem(m_FFEntryPoint + off + 8, &bIsUObj, 8) && (bIsUObj & 1)) {
            confidence += 0.1f;
            desc += ", bIsUObject=1";
        }

        m_Phase5FFieldOwnerCandidates.push_back({off, ptr1, desc, confidence});
        PDBG("ProbeFFieldOwner: 添加候选 off=0x{:X} conf={:.2f}", off, confidence);
        if (confidence >= 1.0f) break;
    }

    std::sort(m_Phase5FFieldOwnerCandidates.begin(), m_Phase5FFieldOwnerCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    PDBG("ProbeFFieldOwner: 候选数={}", m_Phase5FFieldOwnerCandidates.size());
    if (!m_Phase5FFieldOwnerCandidates.empty() && m_Phase5FFieldOwnerCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase5FFieldOwnerCandidates[0];
        auto& r = GetResult("FField::Owner");
        r.offset = best.offset; r.size = 16; r.typeName = "FFieldVariant";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeFFieldOwner: 选定 offset=0x{:X}, conf={:.2f}, confirmed={}",
             best.offset, best.confidence, r.confirmed);
    } else {
        PDBG("ProbeFFieldOwner: 无满足阈值的候选 (最佳 conf={:.2f})",
             m_Phase5FFieldOwnerCandidates.empty() ? 0.0f : m_Phase5FFieldOwnerCandidates[0].confidence);
    }
    PDBG("<<<<<<<<<< [ProbeFFieldOwner] END <<<<<<<<<<");
}

void UEProber::Phase5_ProbeFFieldNext() {
    PDBG(">>>>>>>>>> [ProbeFFieldNext] BEGIN >>>>>>>>>>");
    m_Phase5FFieldNextCandidates.clear();

    int32_t namePrivateOff = GetConfirmedOffset("FField::NamePrivate");
    if (namePrivateOff < 0) {
        PDBG("ProbeFFieldNext: FField::NamePrivate 未确认, 中止");
        PDBG("<<<<<<<<<< [ProbeFFieldNext] END <<<<<<<<<<");
        return;
    }
    int32_t ownerOff = GetConfirmedOffset("FField::Owner");

    if (!m_FFEntryPoint || !m_FFIsValidParam0) {
        PDBG("ProbeFFieldNext: 缺少锚点 EntryPoint={} IsValidP0={}, 中止",
             FormatPtr(m_FFEntryPoint), FormatPtr(m_FFIsValidParam0));
        PDBG("<<<<<<<<<< [ProbeFFieldNext] END <<<<<<<<<<");
        return;
    }
    PDBG("ProbeFFieldNext: namePrivateOff=0x{:X}, ownerOff=0x{:X}, EntryPoint={}, IsValidP0={}",
         namePrivateOff, ownerOff, FormatPtr(m_FFEntryPoint), FormatPtr(m_FFIsValidParam0));

    for (int32_t off = 0x08; off < 0x30; off += 8) {
        if (off == namePrivateOff || off == ownerOff || (ownerOff >= 0 && off == ownerOff + 8)) continue;

        // EntryPoint (NumParms=1): Next 应为 nullptr
        uintptr_t ptr1 = 0;
        ReadMem(m_FFEntryPoint + off, &ptr1, 8);
        if (ptr1 != 0) {
            PDBG("ProbeFFieldNext: off=0x{:X} EntryPoint.Next={} != nullptr, 跳过", off, FormatPtr(ptr1));
            continue;
        }

        // IsValid 首参 (NumParms=2): Next 应为有效指针 -> "ReturnValue"
        uintptr_t ptr2 = 0;
        if (!ReadMem(m_FFIsValidParam0 + off, &ptr2, 8) || !IsValidPtr(ptr2)) {
            PDBG("ProbeFFieldNext: off=0x{:X} IsValidP0.Next={} 无效, 跳过", off, FormatPtr(ptr2));
            continue;
        }

        std::string nextName;
        if (!TryReadFName(ptr2 + namePrivateOff, nextName) || !FNameEq(nextName, "ReturnValue")) {
            PDBG("ProbeFFieldNext: off=0x{:X} Next->Name=\"{}\" 不是ReturnValue, 跳过", off, nextName);
            continue;
        }
        PDBG("ProbeFFieldNext: off=0x{:X} 命中! EntryPoint.Next=nullptr, IsValidP0.Next->\"ReturnValue\"", off);

        // 验证链长: IsValid 应恰好 2 个 (首参 + ReturnValue)
        uintptr_t nextNext = 0;
        ReadMem(ptr2 + off, &nextNext, 8);
        bool chainLen2 = (nextNext == 0); // ReturnValue 的 Next 应为 nullptr

        float confidence = 0.8f;
        std::string desc = std::format("EntryPoint.Next=nullptr, IsValid.Next->\"ReturnValue\"");

        if (chainLen2) {
            confidence += 0.2f;
            desc += ", 链长=2";
            // 保存 IsValid ReturnValue 地址
            m_FFIsValidReturn = ptr2;
        }

        m_Phase5FFieldNextCandidates.push_back({off, 0, desc, confidence});
        PDBG("ProbeFFieldNext: 添加候选 off=0x{:X} conf={:.2f}", off, confidence);
        if (confidence >= 1.0f) break;
    }

    std::sort(m_Phase5FFieldNextCandidates.begin(), m_Phase5FFieldNextCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    PDBG("ProbeFFieldNext: 候选数={}", m_Phase5FFieldNextCandidates.size());
    if (!m_Phase5FFieldNextCandidates.empty() && m_Phase5FFieldNextCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase5FFieldNextCandidates[0];
        auto& r = GetResult("FField::Next");
        r.offset = best.offset; r.size = 8; r.typeName = "FField*";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeFFieldNext: 选定 offset=0x{:X}, conf={:.2f}, confirmed={}",
             best.offset, best.confidence, r.confirmed);
    } else {
        PDBG("ProbeFFieldNext: 无满足阈值的候选 (最佳 conf={:.2f})",
             m_Phase5FFieldNextCandidates.empty() ? 0.0f : m_Phase5FFieldNextCandidates[0].confidence);
    }
    PDBG("<<<<<<<<<< [ProbeFFieldNext] END <<<<<<<<<<");
}

void UEProber::Phase5_ProbeFFieldClassPrivate() {
    PDBG(">>>>>>>>>> [ProbeFFieldClassPrivate] BEGIN >>>>>>>>>>");
    m_Phase5FFieldClassCandidates.clear();

    int32_t namePrivateOff = GetConfirmedOffset("FField::NamePrivate");
    int32_t ownerOff = GetConfirmedOffset("FField::Owner");
    int32_t nextOff = GetConfirmedOffset("FField::Next");
    if (namePrivateOff < 0) {
        PDBG("ProbeFFieldClassPrivate: FField::NamePrivate 未确认, 中止");
        PDBG("<<<<<<<<<< [ProbeFFieldClassPrivate] END <<<<<<<<<<");
        return;
    }

    if (!m_FFEntryPoint) {
        PDBG("ProbeFFieldClassPrivate: 缺少 EntryPoint 锚点, 中止");
        PDBG("<<<<<<<<<< [ProbeFFieldClassPrivate] END <<<<<<<<<<");
        return;
    }
    PDBG("ProbeFFieldClassPrivate: namePrivateOff=0x{:X}, ownerOff=0x{:X}, nextOff=0x{:X}",
         namePrivateOff, ownerOff, nextOff);

    for (int32_t off = 0x08; off < 0x30; off += 8) {
        if (off == namePrivateOff || off == ownerOff || off == nextOff) continue;
        if (ownerOff >= 0 && off == ownerOff + 8) continue; // 跳过 bIsUObject 所在位置

        // EntryPoint 的 ClassPrivate -> FFieldClass, FFieldClass->Name(偏移0) 应为 "IntProperty"
        uintptr_t ptr1 = 0;
        if (!ReadMem(m_FFEntryPoint + off, &ptr1, 8) || !IsValidPtr(ptr1)) continue;

        std::string typeName1;
        if (!TryReadFName(ptr1, typeName1) || !FNameEq(typeName1, "IntProperty")) {
            PDBG("ProbeFFieldClassPrivate: off=0x{:X} ptr1={} typeName=\"{}\" 不是IntProperty",
                 off, FormatPtr(ptr1), typeName1);
            continue;
        }
        PDBG("ProbeFFieldClassPrivate: off=0x{:X} 命中! ptr1={} -> \"IntProperty\"", off, FormatPtr(ptr1));

        float confidence = 0.7f;
        std::string desc = std::format("EntryPoint ClassPrivate -> \"IntProperty\"");

        // 交叉验证: DeltaSeconds 应为 "FloatProperty"
        if (m_FFDeltaSeconds) {
            uintptr_t ptr2 = 0;
            if (ReadMem(m_FFDeltaSeconds + off, &ptr2, 8) && IsValidPtr(ptr2)) {
                std::string typeName2;
                if (TryReadFName(ptr2, typeName2) && FNameEq(typeName2, "FloatProperty")) {
                    confidence += 0.2f;
                    desc += ", DeltaSeconds -> \"FloatProperty\"";
                }
            }
        }

        // 补充验证: IsValid ReturnValue 应为 "BoolProperty"
        if (m_FFIsValidReturn) {
            uintptr_t ptr3 = 0;
            if (ReadMem(m_FFIsValidReturn + off, &ptr3, 8) && IsValidPtr(ptr3)) {
                std::string typeName3;
                if (TryReadFName(ptr3, typeName3) && FNameEq(typeName3, "BoolProperty")) {
                    confidence += 0.1f;
                    desc += ", IsValid RV -> \"BoolProperty\"";
                }
            }
        }

        m_Phase5FFieldClassCandidates.push_back({off, ptr1, desc, confidence});
        PDBG("ProbeFFieldClassPrivate: 添加候选 off=0x{:X} conf={:.2f}", off, confidence);
        if (confidence >= 1.0f) break;
    }

    std::sort(m_Phase5FFieldClassCandidates.begin(), m_Phase5FFieldClassCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    PDBG("ProbeFFieldClassPrivate: 候选数={}", m_Phase5FFieldClassCandidates.size());
    if (!m_Phase5FFieldClassCandidates.empty() && m_Phase5FFieldClassCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase5FFieldClassCandidates[0];
        auto& r = GetResult("FField::ClassPrivate");
        r.offset = best.offset; r.size = 8; r.typeName = "FFieldClass*";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeFFieldClassPrivate: 选定 offset=0x{:X}, conf={:.2f}, confirmed={}",
             best.offset, best.confidence, r.confirmed);
    } else {
        PDBG("ProbeFFieldClassPrivate: 无满足阈值的候选 (最佳 conf={:.2f})",
             m_Phase5FFieldClassCandidates.empty() ? 0.0f : m_Phase5FFieldClassCandidates[0].confidence);
    }
    PDBG("<<<<<<<<<< [ProbeFFieldClassPrivate] END <<<<<<<<<<");
}

void UEProber::Phase5_ProbeFFieldFlagsPrivate() {
    PDBG(">>>>>>>>>> [ProbeFFieldFlagsPrivate] BEGIN >>>>>>>>>>");
    m_Phase5FFieldFlagsPrivateCandidates.clear();

    int32_t namePrivateOff = GetConfirmedOffset("FField::NamePrivate");
    int32_t ownerOff = GetConfirmedOffset("FField::Owner");
    int32_t nextOff = GetConfirmedOffset("FField::Next");
    int32_t classPrivateOff = GetConfirmedOffset("FField::ClassPrivate");

    if (!m_FFEntryPoint) {
        PDBG("ProbeFFieldFlagsPrivate: 缺少 EntryPoint 锚点, 中止");
        PDBG("<<<<<<<<<< [ProbeFFieldFlagsPrivate] END <<<<<<<<<<");
        return;
    }
    PDBG("ProbeFFieldFlagsPrivate: namePrivateOff=0x{:X}, ownerOff=0x{:X}, nextOff=0x{:X}, classPrivateOff=0x{:X}",
         namePrivateOff, ownerOff, nextOff, classPrivateOff);

    // EObjectFlags 有效位掩码 (RF_Public ~ RF_AllocatedInSharedPage 等)
    constexpr uint32_t kValidFlagsMask = 0x3FFFFFFF;

    // 在 FField 的 0x08~0x40 范围按 4 字节对齐搜索 int32
    // 排除 VTable(8字节) 和所有已确认的 FField 成员偏移
    for (int32_t off = 0x08; off < 0x40; off += 4) {
        if (namePrivateOff >= 0 && off >= namePrivateOff && off < namePrivateOff + 8) continue;
        if (ownerOff >= 0 && off >= ownerOff && off < ownerOff + 16) continue;
        if (nextOff >= 0 && off >= nextOff && off < nextOff + 8) continue;
        if (classPrivateOff >= 0 && off >= classPrivateOff && off < classPrivateOff + 8) continue;

        int32_t val1 = -1;
        if (!ReadMemUnsafe(m_FFEntryPoint + off, &val1, 4)) continue;

        // ObjFlags 是位域标志, 值可能非零但应仅含有效标志位, 且不应太大
        uint32_t uval1 = (uint32_t)val1;
        if ((uval1 & ~kValidFlagsMask) != 0 || uval1 > 0xFFFF) continue;

        float confidence = 0.4f;
        std::string desc = std::format("EntryPoint=0x{:X}", uval1);

        // 交叉验证: 所有函数参数的 FProperty 应具有相同的 ObjFlags
        if (m_FFDeltaSeconds) {
            int32_t val2 = -1;
            ReadMemUnsafe(m_FFDeltaSeconds + off, &val2, 4);
            if (val2 == val1) {
                confidence += 0.3f;
                desc += std::format(", DeltaSeconds=0x{:X}(一致)", (uint32_t)val2);
            }
        }

        if (m_FFIsValidParam0) {
            int32_t val3 = -1;
            ReadMemUnsafe(m_FFIsValidParam0 + off, &val3, 4);
            if (val3 == val1) {
                confidence += 0.3f;
                desc += std::format(", IsValidP0=0x{:X}(一致)", (uint32_t)val3);
            }
        }

        m_Phase5FFieldFlagsPrivateCandidates.push_back({off, (uint64_t)uval1, desc, confidence});
        if (confidence >= 1.0f) break;
    }

    std::sort(m_Phase5FFieldFlagsPrivateCandidates.begin(), m_Phase5FFieldFlagsPrivateCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    PDBG("ProbeFFieldFlagsPrivate: 候选数={}", m_Phase5FFieldFlagsPrivateCandidates.size());
    if (!m_Phase5FFieldFlagsPrivateCandidates.empty() && m_Phase5FFieldFlagsPrivateCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase5FFieldFlagsPrivateCandidates[0];
        auto& r = GetResult("FField::FlagsPrivate");
        r.offset = best.offset; r.size = 4; r.typeName = "int32 (EObjectFlags)";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeFFieldFlagsPrivate: 选定 offset=0x{:X}, val=0x{:X}, conf={:.2f}, confirmed={}",
             best.offset, best.rawValue, best.confidence, r.confirmed);
    } else {
        PDBG("ProbeFFieldFlagsPrivate: 无满足阈值的候选 (最佳 conf={:.2f})",
             m_Phase5FFieldFlagsPrivateCandidates.empty() ? 0.0f : m_Phase5FFieldFlagsPrivateCandidates[0].confidence);
    }
    PDBG("<<<<<<<<<< [ProbeFFieldFlagsPrivate] END <<<<<<<<<<");
}

void UEProber::Phase5_ProbeFPropertyArrayDimAndElementSize() {
    PDBG(">>>>>>>>>> [ProbeArrayDim+ElemSize] BEGIN >>>>>>>>>>");
    m_Phase5FPropArrayDimCandidates.clear();
    m_Phase5FPropElemSizeCandidates.clear();

    int32_t namePrivateOff = GetConfirmedOffset("FField::NamePrivate");
    if (namePrivateOff < 0) {
        PDBG("ProbeAD+ES: FField::NamePrivate 未确认, 中止");
        PDBG("<<<<<<<<<< [ProbeArrayDim+ElemSize] END <<<<<<<<<<");
        return;
    }

    // 至少需要 EntryPoint(4) 和 IsValid首参(8) 两个不同 ElementSize 的锚点
    if (!m_FFEntryPoint || !m_FFIsValidParam0) {
        PDBG("ProbeAD+ES: 缺少锚点 EntryPoint={} IsValidP0={}, 中止",
             FormatPtr(m_FFEntryPoint), FormatPtr(m_FFIsValidParam0));
        PDBG("<<<<<<<<<< [ProbeArrayDim+ElemSize] END <<<<<<<<<<");
        return;
    }

    // 如果 ObjFlags 已确认, 从 ObjFlags+4 开始; 否则从 namePrivateOff+0x08 开始
    int32_t objFlagsOff = GetConfirmedOffset("FField::FlagsPrivate");
    int32_t searchStart = (objFlagsOff >= 0) ? (objFlagsOff + 4) : (namePrivateOff + 0x08);
    int32_t searchEnd = searchStart + 0x50;
    PDBG("ProbeAD+ES: namePrivateOff=0x{:X}, objFlagsOff=0x{:X}, 搜索范围 [0x{:X}, 0x{:X})",
         namePrivateOff, objFlagsOff, searchStart, searchEnd);

    for (int32_t off = searchStart; off < searchEnd; off += 4) {
        // ArrayDim: 所有锚点均应为 1
        int32_t ad1 = 0, ad2 = 0;
        bool r1 = ReadMemUnsafe(m_FFEntryPoint + off, &ad1, 4);
        bool r2 = ReadMemUnsafe(m_FFIsValidParam0 + off, &ad2, 4);
        if (!r1 || ad1 != 1 || !r2 || ad2 != 1) {
            PDBG("ProbeAD+ES: off=0x{:X} AD不匹配: r1={} ad1={} r2={} ad2={}", off, r1, ad1, r2, ad2);
            continue;
        }

        // ElementSize 紧邻其后 (+4)
        int32_t es1 = 0, es2 = 0;
        if (!ReadMemUnsafe(m_FFEntryPoint + off + 4, &es1, 4)) continue;
        if (!ReadMemUnsafe(m_FFIsValidParam0 + off + 4, &es2, 4)) continue;

        // EntryPoint(IntProperty) = 4, IsValid首参(ObjectProperty) = 8
        if (es1 != 4 || es2 != 8) {
            PDBG("ProbeAD+ES: off=0x{:X} AD匹配但 ES不匹配: es1={} es2={}", off, es1, es2);
            continue;
        }
        PDBG("ProbeAD+ES: off=0x{:X} 命中! AD1={} AD2={} ES1={} ES2={}", off, ad1, ad2, es1, es2);

        float confidence = 0.7f;
        std::string desc = std::format("EntryPoint: AD=1,ES=4; IsValid: AD=1,ES=8");

        // 交叉验证: DeltaSeconds (FloatProperty) = 4
        if (m_FFDeltaSeconds) {
            int32_t ad3 = 0, es3 = 0;
            if (ReadMemUnsafe(m_FFDeltaSeconds + off, &ad3, 4) && ad3 == 1 &&
                ReadMemUnsafe(m_FFDeltaSeconds + off + 4, &es3, 4) && es3 == 4) {
                confidence += 0.1f;
                desc += "; DeltaSec: AD=1,ES=4";
            }
        }

        // 交叉验证: IsValid ReturnValue (BoolProperty) = 1
        if (m_FFIsValidReturn) {
            int32_t ad4 = 0, es4 = 0;
            if (ReadMemUnsafe(m_FFIsValidReturn + off, &ad4, 4) && ad4 == 1 &&
                ReadMemUnsafe(m_FFIsValidReturn + off + 4, &es4, 4) && es4 == 1) {
                confidence += 0.2f;
                desc += "; IsValidRV: AD=1,ES=1";
            }
        }

        m_Phase5FPropArrayDimCandidates.push_back({off, 1, desc, confidence});
        m_Phase5FPropElemSizeCandidates.push_back({off + 4, (uint64_t)es1, desc, confidence});
        PDBG("ProbeAD+ES: 添加候选 ADoff=0x{:X} ESoff=0x{:X} conf={:.2f}", off, off + 4, confidence);
        if (confidence >= 1.0f) break;
    }

    auto sortFn = [](const auto& a, const auto& b) { return a.confidence > b.confidence; };
    std::sort(m_Phase5FPropArrayDimCandidates.begin(), m_Phase5FPropArrayDimCandidates.end(), sortFn);
    std::sort(m_Phase5FPropElemSizeCandidates.begin(), m_Phase5FPropElemSizeCandidates.end(), sortFn);

    PDBG("ProbeAD+ES: AD候选数={}, ES候选数={}",
         m_Phase5FPropArrayDimCandidates.size(), m_Phase5FPropElemSizeCandidates.size());
    if (!m_Phase5FPropArrayDimCandidates.empty() && m_Phase5FPropArrayDimCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase5FPropArrayDimCandidates[0];
        auto& r = GetResult("FProperty::ArrayDim");
        r.offset = best.offset; r.size = 4; r.typeName = "int32";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeAD: 选定 offset=0x{:X}, conf={:.2f}, confirmed={}",
             best.offset, best.confidence, r.confirmed);
    } else {
        PDBG("ProbeAD: 无满足阈值的候选");
    }
    if (!m_Phase5FPropElemSizeCandidates.empty() && m_Phase5FPropElemSizeCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase5FPropElemSizeCandidates[0];
        auto& r = GetResult("FProperty::ElementSize");
        r.offset = best.offset; r.size = 4; r.typeName = "int32";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeES: 选定 offset=0x{:X}, conf={:.2f}, confirmed={}",
             best.offset, best.confidence, r.confirmed);
    } else {
        PDBG("ProbeES: 无满足阈值的候选");
    }
    PDBG("<<<<<<<<<< [ProbeArrayDim+ElemSize] END <<<<<<<<<<");
}

void UEProber::Phase5_ProbeFPropertyFlags() {
    PDBG(">>>>>>>>>> [ProbeFPropertyFlags] BEGIN >>>>>>>>>>");
    m_Phase5FPropFlagsCandidates.clear();

    int32_t elemSizeOff = GetConfirmedOffset("FProperty::ElementSize");
    if (elemSizeOff < 0) {
        PDBG("ProbePropFlags: ElementSize 未确认, 中止");
        PDBG("<<<<<<<<<< [ProbeFPropertyFlags] END <<<<<<<<<<");
        return;
    }

    if (!m_FFEntryPoint || !m_FFIsValidParam0) {
        PDBG("ProbePropFlags: 缺少锚点 EntryPoint={} IsValidP0={}, 中止",
             FormatPtr(m_FFEntryPoint), FormatPtr(m_FFIsValidParam0));
        PDBG("<<<<<<<<<< [ProbeFPropertyFlags] END <<<<<<<<<<");
        return;
    }

    // 搜索范围: ElementSize 之后按 8 字节对齐
    int32_t searchStart = (elemSizeOff + 4 + 7) & ~7; // 对齐到 8
    int32_t searchEnd = searchStart + 0x30;
    PDBG("ProbePropFlags: elemSizeOff=0x{:X}, 搜索范围 [0x{:X}, 0x{:X})",
         elemSizeOff, searchStart, searchEnd);

    for (int32_t off = searchStart; off < searchEnd; off += 8) {
        uint64_t flags1 = 0, flags2 = 0;
        if (!ReadMemUnsafe(m_FFEntryPoint + off, &flags1, 8)) continue;
        if (!ReadMemUnsafe(m_FFIsValidParam0 + off, &flags2, 8)) continue;

        // 所有锚点必含 CPF_Parm
        if (!(flags1 & ECPFFlags::CPF_Parm) || !(flags2 & ECPFFlags::CPF_Parm)) {
            PDBG("ProbePropFlags: off=0x{:X} flags1=0x{:X} flags2=0x{:X} 缺少CPF_Parm", off, flags1, flags2);
            continue;
        }
        // EntryPoint 不含 CPF_ReturnParm
        if (flags1 & ECPFFlags::CPF_ReturnParm) {
            PDBG("ProbePropFlags: off=0x{:X} EntryPoint含CPF_ReturnParm, 跳过", off);
            continue;
        }
        PDBG("ProbePropFlags: off=0x{:X} 命中! flags1=0x{:X} flags2=0x{:X}", off, flags1, flags2);

        float confidence = 0.6f;
        std::string desc = std::format("EntryPoint=0x{:X}, IsValid首参=0x{:X}", flags1, flags2);

        // IsValid ReturnValue 应含 CPF_ReturnParm | CPF_OutParm
        if (m_FFIsValidReturn) {
            uint64_t flags3 = 0;
            if (ReadMemUnsafe(m_FFIsValidReturn + off, &flags3, 8) &&
                (flags3 & ECPFFlags::CPF_ReturnParm) && (flags3 & ECPFFlags::CPF_OutParm)) {
                confidence += 0.2f;
                desc += std::format(", IsValidRV=0x{:X}(RetParm+OutParm)", flags3);
            }
        }

        // K2_GetActorLocation ReturnValue 应含 CPF_ReturnParm
        if (m_FFK2LocReturn) {
            uint64_t flags4 = 0;
            if (ReadMemUnsafe(m_FFK2LocReturn + off, &flags4, 8) &&
                (flags4 & ECPFFlags::CPF_ReturnParm) && (flags4 & ECPFFlags::CPF_OutParm)) {
                confidence += 0.2f;
                desc += std::format(", K2LocRV=0x{:X}(RetParm+OutParm)", flags4);
            }
        }

        m_Phase5FPropFlagsCandidates.push_back({off, flags1, desc, confidence});
        PDBG("ProbePropFlags: 添加候选 off=0x{:X} conf={:.2f}", off, confidence);
        if (confidence >= 1.0f) break;
    }

    std::sort(m_Phase5FPropFlagsCandidates.begin(), m_Phase5FPropFlagsCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    PDBG("ProbePropFlags: 候选数={}", m_Phase5FPropFlagsCandidates.size());
    if (!m_Phase5FPropFlagsCandidates.empty() && m_Phase5FPropFlagsCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase5FPropFlagsCandidates[0];
        auto& r = GetResult("FProperty::PropertyFlags");
        r.offset = best.offset; r.size = 8; r.typeName = "uint64";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbePropFlags: 选定 offset=0x{:X}, conf={:.2f}, confirmed={}",
             best.offset, best.confidence, r.confirmed);
    } else {
        PDBG("ProbePropFlags: 无满足阈值的候选 (最佳 conf={:.2f})",
             m_Phase5FPropFlagsCandidates.empty() ? 0.0f : m_Phase5FPropFlagsCandidates[0].confidence);
    }
    PDBG("<<<<<<<<<< [ProbeFPropertyFlags] END <<<<<<<<<<");
}

void UEProber::Phase5_ProbeFPropertyOffsetInternal() {
    PDBG(">>>>>>>>>> [ProbeOffset_Internal] BEGIN >>>>>>>>>>");
    m_Phase5FPropOffsetCandidates.clear();

    int32_t propFlagsOff = GetConfirmedOffset("FProperty::PropertyFlags");
    if (propFlagsOff < 0) {
        PDBG("ProbeOffInt: PropertyFlags 未确认, 中止");
        PDBG("<<<<<<<<<< [ProbeOffset_Internal] END <<<<<<<<<<");
        return;
    }

    if (!m_FFEntryPoint || !m_FFIsValidParam0) {
        PDBG("ProbeOffInt: 缺少锚点 EntryPoint={} IsValidP0={}, 中止",
             FormatPtr(m_FFEntryPoint), FormatPtr(m_FFIsValidParam0));
        PDBG("<<<<<<<<<< [ProbeOffset_Internal] END <<<<<<<<<<");
        return;
    }

    // 搜索范围: PropertyFlags 之后按 4 字节对齐
    int32_t searchStart = propFlagsOff + 8;
    int32_t searchEnd = searchStart + 0x20;
    PDBG("ProbeOffInt: propFlagsOff=0x{:X}, 搜索范围 [0x{:X}, 0x{:X})",
         propFlagsOff, searchStart, searchEnd);

    for (int32_t off = searchStart; off < searchEnd; off += 4) {
        int32_t val1 = 0, val2 = 0;
        if (!ReadMemUnsafe(m_FFEntryPoint + off, &val1, 4)) continue;
        if (!ReadMemUnsafe(m_FFIsValidParam0 + off, &val2, 4)) continue;

        // EntryPoint = 0, IsValid首参 = 0 (均为首参, 偏移 0)
        if (val1 != 0 || val2 != 0) {
            PDBG("ProbeOffInt: off=0x{:X} val1={} val2={} 不为0, 跳过", off, val1, val2);
            continue;
        }
        PDBG("ProbeOffInt: off=0x{:X} 命中! EntryPoint=0, IsValidP0=0", off);

        float confidence = 0.6f;
        std::string desc = "EntryPoint=0, IsValid首参=0";

        // IsValid ReturnValue = 8 (在 ObjectProperty(size=8) 之后)
        if (m_FFIsValidReturn) {
            int32_t val3 = 0;
            if (ReadMemUnsafe(m_FFIsValidReturn + off, &val3, 4) && val3 == 8) {
                confidence += 0.3f;
                desc += ", IsValidRV=8";
            }
        }

        // DeltaSeconds = 0 (首参)
        if (m_FFDeltaSeconds) {
            int32_t val4 = 0;
            if (ReadMemUnsafe(m_FFDeltaSeconds + off, &val4, 4) && val4 == 0) {
                confidence += 0.1f;
                desc += ", DeltaSec=0";
            }
        }

        m_Phase5FPropOffsetCandidates.push_back({off, 0, desc, confidence});
        PDBG("ProbeOffInt: 添加候选 off=0x{:X} conf={:.2f}", off, confidence);
        if (confidence >= 1.0f) break;
    }

    std::sort(m_Phase5FPropOffsetCandidates.begin(), m_Phase5FPropOffsetCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    PDBG("ProbeOffInt: 候选数={}", m_Phase5FPropOffsetCandidates.size());
    if (!m_Phase5FPropOffsetCandidates.empty() && m_Phase5FPropOffsetCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase5FPropOffsetCandidates[0];
        auto& r = GetResult("FProperty::Offset_Internal");
        r.offset = best.offset; r.size = 4; r.typeName = "int32";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeOffInt: 选定 offset=0x{:X}, conf={:.2f}, confirmed={}",
             best.offset, best.confidence, r.confirmed);
    } else {
        PDBG("ProbeOffInt: 无满足阈值的候选 (最佳 conf={:.2f})",
             m_Phase5FPropOffsetCandidates.empty() ? 0.0f : m_Phase5FPropOffsetCandidates[0].confidence);
    }
    PDBG("<<<<<<<<<< [ProbeOffset_Internal] END <<<<<<<<<<");
}

void UEProber::Phase5_ProbeFPropertySize() {
    PDBG(">>>>>>>>>> [ProbeFPropertySize] BEGIN >>>>>>>>>>");
    m_Phase5FPropSizeCandidates.clear();

    int32_t offsetInternalOff = GetConfirmedOffset("FProperty::Offset_Internal");
    if (offsetInternalOff < 0) {
        PDBG("ProbePropSize: Offset_Internal 未确认, 中止");
        PDBG("<<<<<<<<<< [ProbeFPropertySize] END <<<<<<<<<<");
        return;
    }

    int32_t namePrivateOff = GetConfirmedOffset("UObject::NamePrivate");
    if (namePrivateOff < 0) {
        PDBG("ProbePropSize: UObject::NamePrivate 未确认, 中止");
        PDBG("<<<<<<<<<< [ProbeFPropertySize] END <<<<<<<<<<");
        return;
    }

    if (!m_FFK2LocReturn || !m_FFIsValidParam0) {
        PDBG("ProbePropSize: 缺少锚点 K2LocRV={} IsValidP0={}, 中止",
             FormatPtr(m_FFK2LocReturn), FormatPtr(m_FFIsValidParam0));
        PDBG("<<<<<<<<<< [ProbeFPropertySize] END <<<<<<<<<<");
        return;
    }

    // 查找 "Vector" ScriptStruct 和 "Object" UClass 用于验证
    uintptr_t vectorStruct = FindObjectInGObjects("Vector", "ScriptStruct");
    uintptr_t objectClass = reinterpret_cast<uintptr_t>(UObject::GObjects->GetByIndex(1));
    PDBG("ProbePropSize: offsetInternalOff=0x{:X}, vectorStruct={}, objectClass={}",
         offsetInternalOff, FormatPtr(vectorStruct), FormatPtr(objectClass));

    if (!vectorStruct && !objectClass) {
        PDBG("ProbePropSize: 无法找到 Vector ScriptStruct 或 Object UClass, 中止");
        PDBG("<<<<<<<<<< [ProbeFPropertySize] END <<<<<<<<<<");
        return;
    }

    // 搜索范围: Offset_Internal 之后按 8 字节对齐, 到足够大的范围
    int32_t searchStart = (offsetInternalOff + 4 + 7) & ~7;
    int32_t searchEnd = searchStart + 0x40;
    PDBG("ProbePropSize: 搜索范围 [0x{:X}, 0x{:X})", searchStart, searchEnd);

    for (int32_t off = searchStart; off < searchEnd; off += 8) {
        float confidence = 0.0f;
        std::string desc;

        // K2_GetActorLocation ReturnValue (FStructProperty): Struct -> "Vector"
        if (m_FFK2LocReturn && vectorStruct) {
            uintptr_t ptr1 = 0;
            if (!ReadMem(m_FFK2LocReturn + off, &ptr1, 8) || ptr1 != vectorStruct) {
                PDBG("ProbePropSize: off=0x{:X} K2Loc ptr1={} != vectorStruct, 跳过",
                     off, FormatPtr(ptr1));
                continue;
            }
            confidence += 0.5f;
            desc += "K2Loc->Vector";
        }

        // IsValid 首参 (FObjectPropertyBase): PropertyClass -> "Object" UClass
        // 两个独立子类在同一偏移命中各自的特征指针, 这是非常强的证据
        if (m_FFIsValidParam0 && objectClass) {
            uintptr_t ptr2 = 0;
            if (!ReadMem(m_FFIsValidParam0 + off, &ptr2, 8)) continue;
            if (ptr2 == objectClass) {
                confidence += 0.5f;
                desc += desc.empty() ? "" : ", ";
                desc += "IsValidP0->Object";
            } else if (IsValidPtr(ptr2)) {
                std::string clsName;
                if (TryReadFName(ptr2 + namePrivateOff, clsName) && !clsName.empty()) {
                    confidence += 0.1f;
                    desc += desc.empty() ? "" : ", ";
                    desc += std::format("IsValidP0->\"{}\"", clsName);
                } else {
                    continue;
                }
            } else {
                continue;
            }
        }

        if (confidence < 0.5f) continue;

        m_Phase5FPropSizeCandidates.push_back({off, (uint64_t)off, desc, confidence});
        PDBG("ProbePropSize: 添加候选 off=0x{:X} conf={:.2f} desc={}", off, confidence, desc);
        if (confidence >= 1.0f) break;
    }

    std::sort(m_Phase5FPropSizeCandidates.begin(), m_Phase5FPropSizeCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    PDBG("ProbePropSize: 候选数={}", m_Phase5FPropSizeCandidates.size());
    if (!m_Phase5FPropSizeCandidates.empty() && m_Phase5FPropSizeCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase5FPropSizeCandidates[0];
        auto& r = GetResult("sizeof(FProperty)");
        r.offset = best.offset; r.size = 0; r.typeName = "size";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbePropSize: 选定 offset=0x{:X}, conf={:.2f}, confirmed={}",
             best.offset, best.confidence, r.confirmed);
    } else {
        PDBG("ProbePropSize: 无满足阈值的候选 (最佳 conf={:.2f})",
             m_Phase5FPropSizeCandidates.empty() ? 0.0f : m_Phase5FPropSizeCandidates[0].confidence);
    }
    PDBG("<<<<<<<<<< [ProbeFPropertySize] END <<<<<<<<<<");
}

void UEProber::Phase5_AutoProbe() {
    m_PhaseStatus[5] = EPhaseStatus::InProgress;
    PDBG("========== [Phase5_AutoProbe] BEGIN ==========");

    if (!HasConfirmed("UStruct::ChildProperties")) {
        PDBG("Phase5: UStruct::ChildProperties 未确认, 中止");
        PDBG("========== [Phase5_AutoProbe] END ==========");
        return;
    }

    // ---------- [Step 1/11] CollectAnchors ----------
    PDBG("---------- [Step 1/11] CollectAnchors ----------");
    Phase5_CollectAnchors();
    if (!m_FFEntryPoint) {
        PDBG("Phase5: EntryPoint 锚点获取失败, 探测终止");
        PDBG("========== [Phase5_AutoProbe] END ==========");
        return;
    }

    // ---------- [Step 2/11] FField::NamePrivate ----------
    PDBG("---------- [Step 2/11] FField::NamePrivate ----------");
    Phase5_ProbeFFieldNamePrivate();
    if (HasResult("FField::NamePrivate")) {
        if (!HasConfirmed("FField::NamePrivate") && GetResult("FField::NamePrivate").autoDetected)
            GetResult("FField::NamePrivate").confirmed = true;
        PDBG("Phase5: FField::NamePrivate => off=0x{:X}, confirmed={}", GetResult("FField::NamePrivate").offset, GetResult("FField::NamePrivate").confirmed);

        // ---------- [Step 3/11] FField::Owner ----------
        PDBG("---------- [Step 3/11] FField::Owner ----------");
        Phase5_ProbeFFieldOwner();
        PDBG("Phase5: FField::Owner => has={}, confirmed={}", HasResult("FField::Owner"), HasConfirmed("FField::Owner"));

        // ---------- [Step 4/11] FField::Next ----------
        PDBG("---------- [Step 4/11] FField::Next ----------");
        Phase5_ProbeFFieldNext();
        PDBG("Phase5: FField::Next => has={}, confirmed={}", HasResult("FField::Next"), HasConfirmed("FField::Next"));

        // ---------- [Step 5/11] FField::ClassPrivate ----------
        PDBG("---------- [Step 5/11] FField::ClassPrivate ----------");
        Phase5_ProbeFFieldClassPrivate();
        PDBG("Phase5: FField::ClassPrivate => has={}, confirmed={}", HasResult("FField::ClassPrivate"), HasConfirmed("FField::ClassPrivate"));

        // ---------- [Step 6/11] FField::FlagsPrivate ----------
        PDBG("---------- [Step 6/11] FField::FlagsPrivate ----------");
        Phase5_ProbeFFieldFlagsPrivate();
        PDBG("Phase5: FField::FlagsPrivate => has={}, confirmed={}", HasResult("FField::FlagsPrivate"), HasConfirmed("FField::FlagsPrivate"));

        // ---------- [Step 7/11] FProperty ArrayDim+ElementSize ----------
        PDBG("---------- [Step 7/11] FProperty ArrayDim+ElementSize ----------");
        Phase5_ProbeFPropertyArrayDimAndElementSize();
        if (HasResult("FProperty::ElementSize")) {
            if (!HasConfirmed("FProperty::ArrayDim") && GetResult("FProperty::ArrayDim").autoDetected)
                GetResult("FProperty::ArrayDim").confirmed = true;
            if (!HasConfirmed("FProperty::ElementSize") && GetResult("FProperty::ElementSize").autoDetected)
                GetResult("FProperty::ElementSize").confirmed = true;
            PDBG("Phase5: ArrayDim => off=0x{:X} confirmed={}", GetResult("FProperty::ArrayDim").offset, GetResult("FProperty::ArrayDim").confirmed);
            PDBG("Phase5: ElementSize => off=0x{:X} confirmed={}", GetResult("FProperty::ElementSize").offset, GetResult("FProperty::ElementSize").confirmed);

            // ---------- [Step 8/11] FProperty::PropertyFlags ----------
            PDBG("---------- [Step 8/11] FProperty::PropertyFlags ----------");
            Phase5_ProbeFPropertyFlags();
            if (HasResult("FProperty::PropertyFlags")) {
                if (!HasConfirmed("FProperty::PropertyFlags") && GetResult("FProperty::PropertyFlags").autoDetected)
                    GetResult("FProperty::PropertyFlags").confirmed = true;
                PDBG("Phase5: PropertyFlags => off=0x{:X} confirmed={}", GetResult("FProperty::PropertyFlags").offset, GetResult("FProperty::PropertyFlags").confirmed);

                // ---------- [Step 9/11] FProperty::Offset_Internal ----------
                PDBG("---------- [Step 9/11] FProperty::Offset_Internal ----------");
                Phase5_ProbeFPropertyOffsetInternal();
                if (HasResult("FProperty::Offset_Internal")) {
                    if (!HasConfirmed("FProperty::Offset_Internal") && GetResult("FProperty::Offset_Internal").autoDetected)
                        GetResult("FProperty::Offset_Internal").confirmed = true;
                    PDBG("Phase5: Offset_Internal => off=0x{:X} confirmed={}", GetResult("FProperty::Offset_Internal").offset, GetResult("FProperty::Offset_Internal").confirmed);

                    // ---------- [Step 10/11] sizeof(FProperty) ----------
                    PDBG("---------- [Step 10/11] sizeof(FProperty) ----------");
                    Phase5_ProbeFPropertySize();
                    PDBG("Phase5: sizeof(FProperty) => has={}, confirmed={}", HasResult("sizeof(FProperty)"), HasConfirmed("sizeof(FProperty)"));
                } else {
                    PDBG("Phase5: Offset_Internal 无结果, 跳过后续步骤");
                }
            } else {
                PDBG("Phase5: PropertyFlags 无结果, 跳过后续步骤");
            }
        } else {
            PDBG("Phase5: ElementSize 无结果, 跳过 FProperty 子步骤");
        }
    } else {
        PDBG("Phase5: FField::NamePrivate 无结果, 跳过所有后续步骤");
    }

    // ---------- [Step 11/11] Summary ----------
    PDBG("---------- [Step 11/11] Summary ----------");
    PDBG("Phase5 结果: Name={} Owner={} Next={} ClassPrivate={} Flags={} ArrayDim={} ElemSize={} PropFlags={} OffInt={} sizeof={}",
         HasConfirmed("FField::NamePrivate"), HasConfirmed("FField::Owner"),
         HasConfirmed("FField::Next"), HasConfirmed("FField::ClassPrivate"),
         HasConfirmed("FField::FlagsPrivate"), HasConfirmed("FProperty::ArrayDim"),
         HasConfirmed("FProperty::ElementSize"), HasConfirmed("FProperty::PropertyFlags"),
         HasConfirmed("FProperty::Offset_Internal"), HasConfirmed("sizeof(FProperty)"));
    PDBG("========== [Phase5_AutoProbe] END ==========");
}

// ============================================================
//  阶段 6: ProcessEvent VTable 索引
// ============================================================

void UEProber::Phase6_ScanProcessEvent() {
    m_Phase6ProcessEventCandidates.clear();
    LogInfo("探测 ProcessEvent VTable 索引");

    // ProcessEvent 特征: 在 VTable 中遍历，找到函数签名匹配的
    uintptr_t obj0 = reinterpret_cast<uintptr_t>(UObject::GObjects->GetByIndex(0));
    if (!obj0 || !IsValidPtr(obj0)) {
        LogError("无法获取 obj[0]");
        return;
    }

    uintptr_t vtable = 0;
    if (!ReadMem(obj0, &vtable, 8) || !IsValidPtr(vtable)) {
        LogError("无法读取 VTable");
        return;
    }

    uintptr_t textStart = GetTextSegStart();
    uintptr_t textEnd = GetTextSegEnd();

    // 遍历 VTable 中的函数指针
    for (int32_t idx = 0; idx < 0x200; ++idx) {
        uintptr_t funcPtr = 0;
        if (!ReadMem(vtable + idx * 8, &funcPtr, 8)) break;

        // 检查是否在 .text 段范围内
        if (funcPtr < textStart || funcPtr >= textEnd)
            continue;

        // ProcessEvent 是虚函数，暂时只列出所有有效索引
        // 需要用户结合已知索引确认
        if (idx >= 0x40 && idx <= 0x80) {
            m_Phase6ProcessEventCandidates.push_back({
                idx, funcPtr,
                std::format("VTable[0x{:X}] = {}", idx, FormatPtr(funcPtr)),
                0.3f
            });
        }
    }

    // 如果 SDK 中已有 ProcessEventIdx, 高亮它
    if (Offsets::ProcessEventIdx != 0) {
        for (auto& c : m_Phase6ProcessEventCandidates) {
            if (c.offset == Offsets::ProcessEventIdx) {
                c.confidence = 1.0f;
                c.description += " [SDK已知]";
            }
        }
    }

    std::sort(m_Phase6ProcessEventCandidates.begin(), m_Phase6ProcessEventCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });
}

void UEProber::Phase6_AutoProbe() {
    m_PhaseStatus[6] = EPhaseStatus::InProgress;
    LogInfo("===== 阶段 6: ProcessEvent VTable 索引 =====");
    Phase6_ScanProcessEvent();
    LogInfo("阶段 6 探测完成");
}

// ============================================================
//  验证工具: 调用 GetEngineVersion
// ============================================================

void UEProber::CallGetEngineVersion() {
    PDBG(">>>>>>>>>> [CallGetEngineVersion] BEGIN >>>>>>>>>>");

    int32_t namePrivateOff       = GetConfirmedOffset("UObject::NamePrivate");
    int32_t classPrivateOff      = GetConfirmedOffset("UObject::ClassPrivate");
    int32_t classDefaultObjOff = GetConfirmedOffset("UClass::ClassDefaultObject");
    int32_t childrenOff   = GetConfirmedOffset("UStruct::Children");
    int32_t nextOff       = GetConfirmedOffset("UField::Next");

    if (namePrivateOff < 0 || classPrivateOff < 0 || classDefaultObjOff < 0 || childrenOff < 0 || nextOff < 0) {
        PDBG("GetEngVer: 缺少必要偏移 name=0x{:X} class=0x{:X} defObj=0x{:X} children=0x{:X} next=0x{:X}",
             namePrivateOff, classPrivateOff, classDefaultObjOff, childrenOff, nextOff);
        PDBG("<<<<<<<<<< [CallGetEngineVersion] END <<<<<<<<<<");
        return;
    }

    // 查找 KismetSystemLibrary UClass
    uintptr_t kismetClass = FindObjectInGObjects("KismetSystemLibrary", "Class");
    if (!kismetClass) {
        PDBG("GetEngVer: KismetSystemLibrary UClass 未找到");
        PDBG("<<<<<<<<<< [CallGetEngineVersion] END <<<<<<<<<<");
        return;
    }
    PDBG("GetEngVer: KismetSystemLibrary @ {}", FormatPtr(kismetClass));

    // 读取 CDO (Class Default Object)
    uintptr_t cdo = 0;
    if (!ReadMem(kismetClass + classDefaultObjOff, &cdo, 8) || !IsValidPtr(cdo)) {
        PDBG("GetEngVer: CDO 读取失败 @ offset 0x{:X}", classDefaultObjOff);
        PDBG("<<<<<<<<<< [CallGetEngineVersion] END <<<<<<<<<<");
        return;
    }
    PDBG("GetEngVer: CDO @ {}", FormatPtr(cdo));

    // 沿 Children→Next 链查找 GetEngineVersion UFunction
    uintptr_t func = WalkChildrenChain(kismetClass, "GetEngineVersion", childrenOff, nextOff, namePrivateOff);
    if (!func) {
        PDBG("GetEngVer: GetEngineVersion UFunction 未找到");
        PDBG("<<<<<<<<<< [CallGetEngineVersion] END <<<<<<<<<<");
        return;
    }
    PDBG("GetEngVer: GetEngineVersion @ {}", FormatPtr(func));

    // 确定 ProcessEvent VTable 索引
    int32_t peIdx = Offsets::ProcessEventIdx; // SDK 已知值
    auto peIt = m_Results.find("ProcessEvent::VTableIdx");
    if (peIt != m_Results.end() && peIt->second.confirmed && peIt->second.offset > 0)
        peIdx = peIt->second.offset;
    PDBG("GetEngVer: ProcessEventIdx = 0x{:X}", peIdx);

    // 从 VTable 读取 ProcessEvent 函数指针
    uintptr_t vtable = 0;
    if (!ReadMem(cdo, &vtable, 8) || !IsValidPtr(vtable)) {
        PDBG("GetEngVer: VTable 读取失败");
        PDBG("<<<<<<<<<< [CallGetEngineVersion] END <<<<<<<<<<");
        return;
    }
    PDBG("GetEngVer: VTable @ {}", FormatPtr(vtable));

    uintptr_t peFunc = 0;
    if (!ReadMem(vtable + peIdx * 8, &peFunc, 8) || peFunc == 0) {
        PDBG("GetEngVer: ProcessEvent 函数指针读取失败 @ VTable[0x{:X}], peFunc={}",
             peIdx, FormatPtr(peFunc));
        PDBG("<<<<<<<<<< [CallGetEngineVersion] END <<<<<<<<<<");
        return;
    }

    // 验证函数指针在 .text 段范围内
    uintptr_t textStart = GetTextSegStart();
    uintptr_t textEnd = GetTextSegEnd();
    PDBG("GetEngVer: ProcessEvent func @ {}, .text=[{}, {})",
         FormatPtr(peFunc), FormatPtr(textStart), FormatPtr(textEnd));
    if (textStart && textEnd && (peFunc < textStart || peFunc >= textEnd)) {
        PDBG("GetEngVer: 警告 - ProcessEvent 不在 .text 段内, 但仍尝试调用");
    }

    // 调用 ProcessEvent(CDO, GetEngineVersion, &parms)
    using ProcessEventFn = void(*)(const void*, void*, void*);
    auto pe = reinterpret_cast<ProcessEventFn>(peFunc);

    struct { FString ReturnValue; } parms = {};
    pe(reinterpret_cast<const void*>(cdo), reinterpret_cast<void*>(func), &parms);

    std::string version = parms.ReturnValue.ToString();
    m_EngineVersion = version;
    PDBG("GetEngVer: EngineVersion = {}", version);
    PDBG("<<<<<<<<<< [CallGetEngineVersion] END <<<<<<<<<<");

    LogSuccess(std::format("Engine Version: {}", version));
}

// ============================================================
//  ImGui 绘制 — 主入口
// ============================================================

void UEProber::Draw(bool* p_open) {
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(950, 750), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.95f);

    if (!ImGui::Begin("UE 结构逆向探测器###UEProber", p_open,
        ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }

    // 菜单栏
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("操作")) {
            if (ImGui::MenuItem("导出结果")) m_CurrentPhase = 99;
            if (ImGui::MenuItem("Dump (AndUEDumper)")) m_CurrentPhase = 100;
            ImGui::Separator();
            if (ImGui::MenuItem("全部自动探测")) {
                Phase1_AutoProbe();
                if (HasConfirmed("UObject::NamePrivate") || GetResult("UObject::NamePrivate").autoDetected) {
                    if (!HasConfirmed("UObject::NamePrivate")) GetResult("UObject::NamePrivate").confirmed = true;
                    if (!HasConfirmed("UObject::ClassPrivate") && GetResult("UObject::ClassPrivate").autoDetected)
                        GetResult("UObject::ClassPrivate").confirmed = true;
                    if (!HasConfirmed("UObject::OuterPrivate") && GetResult("UObject::OuterPrivate").autoDetected)
                        GetResult("UObject::OuterPrivate").confirmed = true;
                    if (!HasConfirmed("UObject::InternalIndex") && GetResult("UObject::InternalIndex").autoDetected)
                        GetResult("UObject::InternalIndex").confirmed = true;
                    Phase2_AutoProbe();
                    if (GetResult("UStruct::SuperStruct").autoDetected && !HasConfirmed("UStruct::SuperStruct"))
                        GetResult("UStruct::SuperStruct").confirmed = true;
                    if (GetResult("UStruct::PropertiesSize").autoDetected && !HasConfirmed("UStruct::PropertiesSize"))
                        GetResult("UStruct::PropertiesSize").confirmed = true;
                    if (GetResult("UStruct::Children").autoDetected && !HasConfirmed("UStruct::Children"))
                        GetResult("UStruct::Children").confirmed = true;
                    if (GetResult("UStruct::ChildProperties").autoDetected && !HasConfirmed("UStruct::ChildProperties"))
                        GetResult("UStruct::ChildProperties").confirmed = true;
                    if (GetResult("UField::Next").autoDetected && !HasConfirmed("UField::Next"))
                        GetResult("UField::Next").confirmed = true;
                    if (GetResult("UStruct::MinAlignment").autoDetected && !HasConfirmed("UStruct::MinAlignment"))
                        GetResult("UStruct::MinAlignment").confirmed = true;
                    Phase3_AutoProbe();
                    if (GetResult("UClass::ClassDefaultObject").autoDetected && !HasConfirmed("UClass::ClassDefaultObject"))
                        GetResult("UClass::ClassDefaultObject").confirmed = true;
                    Phase4_AutoProbe();
                    Phase5_AutoProbe();
                    Phase6_AutoProbe();
                }
            }
            if (ImGui::MenuItem("清空结果")) {
                m_Results.clear();
                m_Log.clear();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    DrawPhaseSelector();
    ImGui::Separator();

    switch (m_CurrentPhase) {
        case 0: DrawResultsSummary(); break;
        case 1: DrawPhase1(); break;
        case 2: DrawPhase2(); break;
        case 3: DrawPhase3(); break;
        case 4: DrawPhase4(); break;
        case 5: DrawPhase5(); break;
        case 6: DrawPhase6(); break;
        case 99: DrawExportPanel(); break;
        case 100: DrawDumpPanel(); break;
    }

    ImGui::End();
}

// ============================================================
//  阶段选择器
// ============================================================

void UEProber::DrawPhaseSelector() {
    static const char* phaseNames[] = {
        "总览", "1.UObject", "2.UField/UStruct", "3.UClass",
        "4.UFunction", "5.FField/FProperty", "6.ProcessEvent"
    };
    static const ImVec4 statusColors[] = {
        ImVec4(0.5f, 0.5f, 0.5f, 1.0f), // NotStarted
        ImVec4(1.0f, 0.8f, 0.2f, 1.0f), // InProgress
        ImVec4(0.2f, 1.0f, 0.4f, 1.0f), // Completed
        ImVec4(1.0f, 0.3f, 0.3f, 1.0f), // Failed
    };

    for (int i = 0; i <= 6; ++i) {
        if (i > 0) ImGui::SameLine();

        bool isActive = (m_CurrentPhase == i);
        if (isActive)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));

        if (i > 0) {
            ImVec4 dotColor = statusColors[(int)m_PhaseStatus[i]];
            ImGui::PushStyleColor(ImGuiCol_Text, dotColor);
        }

        if (ImGui::Button(phaseNames[i]))
            m_CurrentPhase = i;

        if (i > 0) ImGui::PopStyleColor(); // text
        if (isActive) ImGui::PopStyleColor(); // button
    }
}

// ============================================================
//  绘制候选项表格 (通用)
// ============================================================

void UEProber::DrawCandidateTable(const std::string& label,
    std::vector<ScanCandidate>& candidates, OffsetResult& target)
{
    if (candidates.empty()) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "暂无候选项");
        return;
    }

    ImGui::Text("%s — %d 个候选项:", label.c_str(), (int)candidates.size());

    if (target.confirmed) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "[已确认: 0x%X]", target.offset);
    }

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    std::string tableId = "##cand_" + label;

    if (ImGui::BeginTable(tableId.c_str(), 5, flags, ImVec2(0, std::min(200.0f, candidates.size() * 28.0f + 30)))) {
        ImGui::TableSetupColumn("偏移",   ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("原始值",  ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("置信度",  ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("描述",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("操作",    ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)candidates.size(); ++i) {
            auto& c = candidates[i];
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            bool isConfirmed = (target.confirmed && target.offset == c.offset);
            if (isConfirmed)
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "0x%X", c.offset);
            else
                ImGui::Text("0x%X", c.offset);

            ImGui::TableNextColumn();
            ImGui::Text("0x%llX", (unsigned long long)c.rawValue);

            ImGui::TableNextColumn();
            ImVec4 confColor = c.confidence >= 0.8f ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) :
                               c.confidence >= 0.5f ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f) :
                                                       ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
            ImGui::TextColored(confColor, "%.0f%%", c.confidence * 100);

            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", c.description.c_str());

            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (isConfirmed) {
                if (ImGui::SmallButton("取消")) {
                    target.confirmed = false;
                }
            } else {
                if (ImGui::SmallButton("确认")) {
                    target.offset = c.offset;
                    target.evidence = c.description;
                    target.confirmed = true;
                    target.autoDetected = false; // 手动确认
                    LogSuccess(std::format("{} 手动确认: 偏移 0x{:X}", target.name, c.offset));
                }
            }
            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    // 手动输入
    ImGui::PushID(label.c_str());
    static char manualInput[16] = {};
    ImGui::PushItemWidth(80);
    ImGui::InputText("手动偏移", manualInput, sizeof(manualInput),
                     ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::SmallButton("手动设置")) {
        char* end = nullptr;
        long val = strtol(manualInput, &end, 16);
        if (end != manualInput && val >= 0) {
            target.offset = (int32_t)val;
            target.confirmed = true;
            target.evidence = "手动设置";
            LogSuccess(std::format("{} 手动设置: 偏移 0x{:X}", target.name, val));
        }
    }
    ImGui::PopID();
}

// ============================================================
//  内存 Dump 绘制
// ============================================================

void UEProber::DrawMemoryDump(uintptr_t address, int32_t size, const std::string& label) {
    if (!address || size <= 0) return;

    m_DumpBuffer.resize(size);
    if (!ReadMem(address, m_DumpBuffer.data(), size)) {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "读取失败: %s", FormatPtr(address).c_str());
        return;
    }

    ImGui::Text("%s @ %s (%d 字节)", label.c_str(), FormatPtr(address).c_str(), size);

    // Hex dump
    if (ImGui::BeginChild(("##dump_" + label).c_str(), ImVec2(0, std::min(200.0f, size / 16.0f * 20 + 40)), true,
        ImGuiWindowFlags_HorizontalScrollbar)) {
        for (int32_t row = 0; row < size; row += 16) {
            ImGui::Text("%04X: ", row);
            ImGui::SameLine();
            for (int col = 0; col < 16 && row + col < size; ++col) {
                uint8_t b = m_DumpBuffer[row + col];
                if (b == 0)
                    ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1), "%02X", b);
                else
                    ImGui::Text("%02X", b);
                if (col < 15) ImGui::SameLine();
            }
            ImGui::SameLine();
            ImGui::Text(" | ");
            ImGui::SameLine();
            for (int col = 0; col < 16 && row + col < size; ++col) {
                char c = (char)m_DumpBuffer[row + col];
                if (c >= 0x20 && c <= 0x7E)
                    ImGui::Text("%c", c);
                else
                    ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1), ".");
                if (col < 15) ImGui::SameLine();
            }
        }
    }
    ImGui::EndChild();
}

// ============================================================
//  阶段 1 绘制
// ============================================================

void UEProber::DrawPhase1() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "阶段 1: UObject 基础成员探测");
    ImGui::TextWrapped("探测 VTable, Index, Name, Class, Outer, Flags 六个成员的偏移。");
    ImGui::Spacing();

    // 前置配置
    if (ImGui::TreeNodeEx("前置配置", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushItemWidth(80);
        ImGui::InputInt("探测范围", &m_ProbeRange, 0x10, 0x40);
        ImGui::PopItemWidth();
        if (m_ProbeRange < 0x20) m_ProbeRange = 0x20;
        if (m_ProbeRange > 0x200) m_ProbeRange = 0x200;

        // 显示 obj[0]~obj[3]
        for (int i = 0; i <= 4; ++i) {
            uintptr_t obj = reinterpret_cast<uintptr_t>(UObject::GObjects->GetByIndex(i));
            if (obj && IsValidPtr(obj)) {
                ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                    "  obj[%d] = %s", i, FormatPtr(obj).c_str());
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                    "  obj[%d] = 无效", i);
            }
        }
        ImGui::TreePop();
    }

    ImGui::Spacing();
    if (ImGui::Button("自动探测阶段 1", ImVec2(200, 30))) {
        Phase1_AutoProbe();
    }
    ImGui::Spacing();

    // 各字段候选结果
    if (ImGui::TreeNodeEx("UObject::VTable", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto& r = GetResult("UObject::VTable");
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "偏移 0x00 (固定)");
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("UObject::InternalIndex")) {
        DrawCandidateTable("UObject::InternalIndex", m_Phase1InternalIndexCandidates, GetResult("UObject::InternalIndex"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("UObject::NamePrivate")) {
        DrawCandidateTable("UObject::NamePrivate", m_Phase1NamePrivateCandidates, GetResult("UObject::NamePrivate"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("UObject::ClassPrivate")) {
        DrawCandidateTable("UObject::ClassPrivate", m_Phase1ClassPrivateCandidates, GetResult("UObject::ClassPrivate"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("UObject::OuterPrivate")) {
        DrawCandidateTable("UObject::OuterPrivate", m_Phase1OuterPrivateCandidates, GetResult("UObject::OuterPrivate"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("UObject::ObjectFlags")) {
        DrawCandidateTable("UObject::ObjectFlags", m_Phase1ObjectFlagsCandidates, GetResult("UObject::ObjectFlags"));
        ImGui::TreePop();
    }

    // 内存 dump
    if (ImGui::TreeNode("内存查看")) {
        uintptr_t obj0 = reinterpret_cast<uintptr_t>(UObject::GObjects->GetByIndex(0));
        if (obj0 && IsValidPtr(obj0))
            DrawMemoryDump(obj0, 0x40, "obj[0]");
        uintptr_t obj1 = reinterpret_cast<uintptr_t>(UObject::GObjects->GetByIndex(1));
        if (obj1 && IsValidPtr(obj1))
            DrawMemoryDump(obj1, 0x40, "obj[1]");
        ImGui::TreePop();
    }
}

// ============================================================
//  阶段 2 绘制
// ============================================================

void UEProber::DrawPhase2() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "阶段 2: UField / UStruct 探测");
    ImGui::TextWrapped("探测 UField::Next, UStruct::Super, Size, MinAlignment, Children, ChildProperties");
    ImGui::Spacing();

    bool canAutoProbe = HasConfirmed("UObject::NamePrivate") && HasConfirmed("UObject::ClassPrivate");
    if (!canAutoProbe)
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "请先完成阶段 1 (确认 Name 和 Class)");

    if (ImGui::Button("自动探测阶段 2", ImVec2(200, 30)) && canAutoProbe) {
        Phase2_AutoProbe();
    }
    ImGui::Spacing();

    if (ImGui::TreeNode("UStruct::SuperStruct")) {
        DrawCandidateTable("UStruct::SuperStruct", m_Phase2SuperStructCandidates, GetResult("UStruct::SuperStruct"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("UStruct::MinAlignment")) {
        DrawCandidateTable("UStruct::MinAlignment", m_Phase2MinAlignCandidates, GetResult("UStruct::MinAlignment"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("UStruct::PropertiesSize")) {
        DrawCandidateTable("UStruct::PropertiesSize", m_Phase2SizeCandidates, GetResult("UStruct::PropertiesSize"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("UStruct::Children")) {
        DrawCandidateTable("UStruct::Children", m_Phase2ChildrenCandidates, GetResult("UStruct::Children"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("UStruct::ChildProperties")) {
        DrawCandidateTable("UStruct::ChildProperties", m_Phase2ChildPropsCandidates, GetResult("UStruct::ChildProperties"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("UField::Next")) {
        DrawCandidateTable("UField::Next", m_Phase2NextCandidates, GetResult("UField::Next"));
        ImGui::TreePop();
    }

    // 缓存的 UClass 地址
    if (ImGui::TreeNode("关键地址缓存")) {
        ImGui::Text("ClassClass:    %s", FormatPtr(m_ClassClass).c_str());
        ImGui::Text("ClassStruct:   %s", FormatPtr(m_ClassStruct).c_str());
        ImGui::Text("ClassField:    %s", FormatPtr(m_ClassField).c_str());
        ImGui::Text("ClassObject:   %s", FormatPtr(m_ClassObject).c_str());

        // 结构大小
        auto showSize = [&](const char* label, uintptr_t addr) {
            if (!addr) return;
            int32_t sz = GetStructSize(addr);
            if (sz > 0)
                ImGui::Text("  sizeof(%s) = 0x%X (%d)", label, sz, sz);
        };
        showSize("UObject", m_ClassObject);
        showSize("UField", m_ClassField);
        showSize("UStruct", m_ClassStruct);
        showSize("UClass", m_ClassClass);
        ImGui::TreePop();
    }
}

// ============================================================
//  阶段 3 绘制
// ============================================================

void UEProber::DrawPhase3() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "阶段 3: UClass 成员探测");
    ImGui::TextWrapped("探测 UClass::CastFlags, DefaultObject");
    ImGui::Spacing();

    if (ImGui::Button("自动探测阶段 3", ImVec2(200, 30))) {
        Phase3_AutoProbe();
    }
    ImGui::Spacing();

    if (ImGui::TreeNode("UClass::CastFlags")) {
        DrawCandidateTable("UClass::CastFlags", m_Phase3CastFlagsCandidates, GetResult("UClass::CastFlags"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("UClass::ClassDefaultObject")) {
        DrawCandidateTable("UClass::ClassDefaultObject", m_Phase2ClassDefaultObjCandidates, GetResult("UClass::ClassDefaultObject"));
        ImGui::TreePop();
    }
}

// ============================================================
//  阶段 4 绘制
// ============================================================

void UEProber::DrawPhase4() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "阶段 4: UFunction 探测");
    ImGui::TextWrapped("探测 FunctionFlags, NumParms, ParmsSize, ReturnValueOffset, Func");
    ImGui::Spacing();

    // 显示缓存的锚点函数地址
    if (m_FuncReceiveBeginPlay)
        ImGui::Text("ReceiveBeginPlay:    %s", FormatPtr(m_FuncReceiveBeginPlay).c_str());
    if (m_FuncReceiveTick)
        ImGui::Text("ReceiveTick:         %s", FormatPtr(m_FuncReceiveTick).c_str());
    if (m_FuncIsValid)
        ImGui::Text("IsValid:             %s", FormatPtr(m_FuncIsValid).c_str());
    if (m_FuncPrintString)
        ImGui::Text("PrintString:         %s", FormatPtr(m_FuncPrintString).c_str());
    if (m_FuncK2_GetActorLocation)
        ImGui::Text("K2_GetActorLocation: %s", FormatPtr(m_FuncK2_GetActorLocation).c_str());

    ImGui::Spacing();
    if (ImGui::Button("自动探测阶段 4", ImVec2(200, 30))) {
        Phase4_AutoProbe();
    }
    ImGui::Spacing();

    if (ImGui::TreeNode("UFunction::FunctionFlags")) {
        DrawCandidateTable("UFunction::FunctionFlags", m_Phase4FuncFlagsCandidates, GetResult("UFunction::FunctionFlags"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("UFunction::NumParms")) {
        DrawCandidateTable("UFunction::NumParms", m_Phase4NumParmsCandidates, GetResult("UFunction::NumParms"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("UFunction::ParmsSize")) {
        DrawCandidateTable("UFunction::ParmsSize", m_Phase4ParmsSizeCandidates, GetResult("UFunction::ParmsSize"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("UFunction::ReturnValueOffset")) {
        DrawCandidateTable("UFunction::ReturnValueOffset", m_Phase4ReturnValueOffCandidates, GetResult("UFunction::ReturnValueOffset"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("UFunction::Func")) {
        DrawCandidateTable("UFunction::Func", m_Phase4ExecFuncCandidates, GetResult("UFunction::Func"));
        ImGui::TreePop();
    }

    // 内存 dump
    if (ImGui::TreeNode("UFunction 内存查看")) {
        if (m_FuncReceiveBeginPlay)
            DrawMemoryDump(m_FuncReceiveBeginPlay, 0xF0, "ReceiveBeginPlay");
        if (m_FuncReceiveTick)
            DrawMemoryDump(m_FuncReceiveTick, 0xF0, "ReceiveTick");
        if (m_FuncIsValid)
            DrawMemoryDump(m_FuncIsValid, 0xF0, "IsValid");
        if (m_FuncPrintString)
            DrawMemoryDump(m_FuncPrintString, 0xF0, "PrintString");
        if (m_FuncK2_GetActorLocation)
            DrawMemoryDump(m_FuncK2_GetActorLocation, 0xF0, "K2_GetActorLocation");
        ImGui::TreePop();
    }
}

// ============================================================
//  阶段 5 绘制
// ============================================================

void UEProber::DrawPhase5() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "阶段 5: FField / FProperty 探测");
    ImGui::TextWrapped("探测 FField::VTable, Owner, Name, Next, ClassPrivate 及 FProperty 成员");
    ImGui::Spacing();

    if (ImGui::Button("自动探测阶段 5", ImVec2(200, 30))) {
        Phase5_AutoProbe();
    }
    ImGui::Spacing();

    if (ImGui::TreeNode("FField::NamePrivate")) {
        DrawCandidateTable("FField::NamePrivate", m_Phase5FFieldNamePrivateCandidates, GetResult("FField::NamePrivate"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("FField::Next")) {
        DrawCandidateTable("FField::Next", m_Phase5FFieldNextCandidates, GetResult("FField::Next"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("FField::Owner")) {
        DrawCandidateTable("FField::Owner", m_Phase5FFieldOwnerCandidates, GetResult("FField::Owner"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("FField::ClassPrivate")) {
        DrawCandidateTable("FField::ClassPrivate", m_Phase5FFieldClassCandidates, GetResult("FField::ClassPrivate"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("FField::FlagsPrivate")) {
        DrawCandidateTable("FField::FlagsPrivate", m_Phase5FFieldFlagsPrivateCandidates, GetResult("FField::FlagsPrivate"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("FProperty::ArrayDim")) {
        DrawCandidateTable("FProperty::ArrayDim", m_Phase5FPropArrayDimCandidates, GetResult("FProperty::ArrayDim"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("FProperty::ElementSize")) {
        DrawCandidateTable("FProperty::ElementSize", m_Phase5FPropElemSizeCandidates, GetResult("FProperty::ElementSize"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("FProperty::PropertyFlags")) {
        DrawCandidateTable("FProperty::PropertyFlags", m_Phase5FPropFlagsCandidates, GetResult("FProperty::PropertyFlags"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("FProperty::Offset_Internal")) {
        DrawCandidateTable("FProperty::Offset_Internal", m_Phase5FPropOffsetCandidates, GetResult("FProperty::Offset_Internal"));
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("sizeof(FProperty)")) {
        DrawCandidateTable("sizeof(FProperty)", m_Phase5FPropSizeCandidates, GetResult("sizeof(FProperty)"));
        ImGui::TreePop();
    }
}

// ============================================================
//  阶段 6 绘制
// ============================================================

void UEProber::DrawPhase6() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "阶段 6: ProcessEvent VTable 索引");
    ImGui::TextWrapped("确定 UObject::ProcessEvent 在虚函数表中的索引");
    ImGui::Spacing();

    ImGui::Text("SDK 已知索引: 0x%X", Offsets::ProcessEventIdx);
    ImGui::Spacing();

    if (ImGui::Button("扫描 VTable", ImVec2(200, 30))) {
        Phase6_AutoProbe();
    }
    ImGui::Spacing();

    if (ImGui::TreeNode("ProcessEvent VTable 索引")) {
        DrawCandidateTable("ProcessEvent VTable Index", m_Phase6ProcessEventCandidates, GetResult("ProcessEvent::VTableIdx"));
        ImGui::TreePop();
    }
}

// ============================================================
//  结果总览
// ============================================================

void UEProber::DrawResultsSummary() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "探测结果总览");
    ImGui::Spacing();

    // 按类别分组
    struct Category {
        const char* name;
        std::vector<std::string> fields;
    };
    Category categories[] = {
        {"UObject", {"UObject::VTable", "UObject::InternalIndex", "UObject::NamePrivate",
                     "UObject::ClassPrivate", "UObject::OuterPrivate", "UObject::ObjectFlags"}},
        {"UField",  {"UField::Next"}},
        {"UStruct", {"UStruct::SuperStruct", "UStruct::PropertiesSize", "UStruct::MinAlignment",
                     "UStruct::Children", "UStruct::ChildProperties"}},
        {"UClass",  {"UClass::CastFlags", "UClass::ClassDefaultObject"}},
        {"UFunction", {"UFunction::NumParms", "UFunction::ParmsSize",
                       "UFunction::FunctionFlags", "UFunction::Func"}},
        {"FField",  {"FField::NamePrivate", "FField::Next", "FField::Owner",
                     "FField::ClassPrivate", "FField::FlagsPrivate"}},
        {"FProperty", {"FProperty::ArrayDim", "FProperty::ElementSize",
                       "FProperty::PropertyFlags", "FProperty::Offset_Internal",
                       "sizeof(FProperty)"}},
        {"ProcessEvent", {"ProcessEvent::VTableIdx"}},
    };

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("ResultsSummary", 5, flags)) {
        ImGui::TableSetupColumn("字段",    ImGuiTableColumnFlags_WidthFixed, 200);
        ImGui::TableSetupColumn("偏移",    ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("类型",    ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("状态",    ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("证据",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (auto& cat : categories) {
            // Category header
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "%s", cat.name);
            ImGui::TableNextColumn(); ImGui::TableNextColumn();
            ImGui::TableNextColumn(); ImGui::TableNextColumn();

            for (auto& fieldName : cat.fields) {
                auto it = m_Results.find(fieldName);
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                ImGui::Text("  %s", fieldName.c_str());

                ImGui::TableNextColumn();
                if (it != m_Results.end() && it->second.offset >= 0) {
                    if (it->second.confirmed)
                        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "0x%X", it->second.offset);
                    else
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "0x%X?", it->second.offset);
                } else {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "未知");
                }

                ImGui::TableNextColumn();
                if (it != m_Results.end())
                    ImGui::Text("%s", it->second.typeName.c_str());

                ImGui::TableNextColumn();
                if (it != m_Results.end()) {
                    if (it->second.confirmed)
                        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "已确认");
                    else if (it->second.autoDetected)
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "待确认");
                    else
                        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "未探测");
                } else {
                    ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "未探测");
                }

                ImGui::TableNextColumn();
                if (it != m_Results.end() && !it->second.evidence.empty())
                    ImGui::TextWrapped("%s", it->second.evidence.c_str());
            }
        }

        ImGui::EndTable();
    }

    // 日志区域
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::TreeNode("探测日志")) {
        if (ImGui::Button("清空日志")) m_Log.clear();
        ImGui::BeginChild("##probe_log", ImVec2(0, 200), true);
        for (int i = (int)m_Log.size() - 1; i >= 0 && i >= (int)m_Log.size() - 100; --i) {
            ImGui::TextColored(m_Log[i].color, "%s", m_Log[i].text.c_str());
        }
        ImGui::EndChild();
        ImGui::TreePop();
    }
}

// ============================================================
//  导出面板
// ============================================================

void UEProber::DrawExportPanel() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "导出探测结果");
    ImGui::Spacing();

    // 自动获取引擎版本
    if (m_EngineVersion.empty()) {
        int32_t namePrivateOff       = GetConfirmedOffset("UObject::NamePrivate");
        int32_t classPrivateOff      = GetConfirmedOffset("UObject::ClassPrivate");
        int32_t classDefaultObjOff = GetConfirmedOffset("UClass::ClassDefaultObject");
        int32_t childrenOff   = GetConfirmedOffset("UStruct::Children");
        int32_t nextOff       = GetConfirmedOffset("UField::Next");
        if (namePrivateOff >= 0 && classPrivateOff >= 0 && classDefaultObjOff >= 0 && childrenOff >= 0 && nextOff >= 0)
            CallGetEngineVersion();
    }

    ImGui::Text("C++ 结构体定义 (SDK 格式):");
    ImGui::Separator();

    // ---- 字段定义 ----
    struct FieldInfo {
        std::string resultKey;   // m_Results 中的 key
        std::string memberName;  // C++ 成员名
        std::string typeName;    // C++ 类型
        int32_t     size;        // 字段占用字节数
    };

    // ---- 类定义 ----
    struct ClassDef {
        std::string className;
        std::string parentClass;     // 空 = 无父类
        std::string sizeStructName;  // GetStructSize 参数, 空 = 用 sizeResultKey
        std::string sizeResultKey;   // 从 m_Results 获取 (如 "sizeof(FProperty)")
        std::vector<FieldInfo> fields;
    };

    ClassDef classes[] = {
        {"UObject", "", "UObject", "", {
            {"UObject::VTable", "VTable", "void**",          8},
            {"UObject::ObjectFlags",    "ObjectFlags",    "EObjectFlags",    4},
            {"UObject::InternalIndex",  "InternalIndex",  "int32",            4},
            {"UObject::ClassPrivate",   "ClassPrivate",   "class UClass*",    8},
            {"UObject::NamePrivate",    "NamePrivate",    "FName",            8},
            {"UObject::OuterPrivate",   "OuterPrivate",   "class UObject*",   8},
        }},
        {"UField", "UObject", "UField", "", {
            {"UField::Next", "Next", "class UField*", 8},
        }},
        {"UStruct", "UField", "UStruct", "", {
            {"UStruct::SuperStruct",      "SuperStruct",     "class UStruct*", 8},
            {"UStruct::Children",        "Children",        "class UField*",  8},
            {"UStruct::ChildProperties", "ChildProperties", "class FField*",  8},
            {"UStruct::PropertiesSize",            "PropertiesSize",  "int32",          4},
            {"UStruct::MinAlignment",    "MinAlignment",    "int32",          4},
        }},
        {"UClass", "UStruct", "UClass", "", {
            {"UClass::CastFlags",     "CastFlags",     "EClassCastFlags", 8},
            {"UClass::ClassDefaultObject", "ClassDefaultObject", "class UObject*",  8},
        }},
        {"UFunction", "UStruct", "UFunction", "", {
            {"UFunction::FunctionFlags",    "FunctionFlags",    "EFunctionFlags",  4},
            {"UFunction::NumParms",        "NumParms",         "uint8",           1},
            {"UFunction::ParmsSize",        "ParmsSize",        "uint16",          2},
            {"UFunction::ReturnValueOffset","ReturnValueOffset","uint16",          2},
            {"UFunction::Func",             "Func",             "FNativeFuncPtr",  8},
        }},
        {"FField", "", "", "", {
            {"FField::VTable",       "VTable",       "void**",             8},
            {"FField::NamePrivate",         "NamePrivate",  "FName",              8},
            {"FField::FlagsPrivate",     "FlagsPrivate", "EObjectFlags",       4},
            {"FField::ClassPrivate", "ClassPrivate", "class FFieldClass*", 8},
            {"FField::Owner",        "Owner",        "FFieldVariant",     16},
            {"FField::Next",         "Next",         "class FField*",      8},
        }},
        {"FProperty", "FField", "", "sizeof(FProperty)", {
            {"FProperty::ArrayDim",        "ArrayDim",        "int32",          4},
            {"FProperty::ElementSize",     "ElementSize",     "int32",          4},
            {"FProperty::PropertyFlags",   "PropertyFlags",   "EPropertyFlags", 8},
            {"FProperty::Offset_Internal", "Offset_Internal", "int32",          4},
        }},
    };

    // ---- 生成代码 ----
    std::string code;
    std::string verLabel = m_EngineVersion.empty() ? "Unknown" : m_EngineVersion;
    code += std::format("// ===== By UEProber GetEngineVersion: \"{}\" =====\n\n", verLabel);

    // 记录每个类的实际内容结尾 (最后一个字段 offset+size),
    // 用于非 POD 基类的尾部填充复用 (Itanium ABI)
    std::map<std::string, int32_t> classContentEnd;

    for (auto& cls : classes) {
        // 收集已确认偏移的字段, 按偏移排序
        struct ResolvedField {
            std::string memberName;
            std::string typeName;
            int32_t offset;
            int32_t size;
        };
        std::vector<ResolvedField> resolved;

        for (auto& f : cls.fields) {
            // VTable 固定在 0, 不在 m_Results 中
            if (f.resultKey == "FField::VTable") {
                resolved.push_back({f.memberName, f.typeName, 0, f.size});
                continue;
            }
            auto it = m_Results.find(f.resultKey);
            if (it != m_Results.end() && it->second.offset >= 0) {
                resolved.push_back({f.memberName, f.typeName, it->second.offset, f.size});
            }
        }

        if (resolved.empty()) continue;

        std::sort(resolved.begin(), resolved.end(),
            [](const auto& a, const auto& b) { return a.offset < b.offset; });

        // 计算本类实际内容结尾
        int32_t contentEnd = 0;
        for (auto& f : resolved)
            contentEnd = std::max(contentEnd, f.offset + f.size);
        classContentEnd[cls.className] = contentEnd;

        // 获取 sizeof
        int32_t structSize = 0;
        if (!cls.sizeStructName.empty())
            structSize = GetStructSize(cls.sizeStructName);
        if (structSize <= 0 && !cls.sizeResultKey.empty()) {
            auto it = m_Results.find(cls.sizeResultKey);
            if (it != m_Results.end() && it->second.offset > 0)
                structSize = it->second.offset; // sizeof(FProperty) 存的是偏移值即大小
        }

        // 获取父类大小 (子类成员起始偏移)
        // 对于含 VTable 的非 POD 基类, 使用 contentEnd 以启用尾部填充复用
        int32_t parentSize = 0;
        if (!cls.parentClass.empty()) {
            auto ceIt = classContentEnd.find(cls.parentClass);
            if (ceIt != classContentEnd.end())
                parentSize = ceIt->second;
            else
                parentSize = GetStructSize(cls.parentClass);
        }

        // 判断是否为含 VTable 的根类 (需要非平凡析构函数使其 non-POD)
        bool isNonPodRoot = cls.parentClass.empty() &&
            std::any_of(resolved.begin(), resolved.end(),
                [](const ResolvedField& f) { return f.memberName == "VTable"; });

        // ---- 生成 class 定义 ----
        if (cls.parentClass.empty())
            code += std::format("class {} {{\npublic:\n", cls.className);
        else
            code += std::format("class {} : public {} {{\npublic:\n", cls.className, cls.parentClass);

        // 非 POD 根类添加析构函数, 使 Itanium ABI 允许派生类复用尾部填充
        if (isNonPodRoot)
            code += std::format("    ~{}() {{}}\n\n", cls.className);

        int32_t cursor = parentSize; // 当前写入位置
        int padIdx = 0;

        for (auto& f : resolved) {
            if (f.offset < cursor) continue; // 跟父类重叠, 跳过

            // 插入填充
            if (f.offset > cursor) {
                int32_t gap = f.offset - cursor;
                code += std::format("    {:50s}Pad_{:02X}[0x{:X}];\n",
                    "uint8", cursor, gap);
                ++padIdx;
            }

            // 写入字段
            code += std::format("    {:50s}{};\n", f.typeName, f.memberName);
            cursor = f.offset + f.size;
        }

        // 尾部填充
        if (structSize > 0 && cursor < structSize) {
            int32_t gap = structSize - cursor;
            code += std::format("    {:50s}Pad_{:02X}[0x{:X}];\n",
                "uint8", cursor, gap);
        }

        code += "};\n\n";

        // ---- static_assert ----
        if (structSize > 0) {
            code += std::format("static_assert(alignof({}) == 0x{:06X}, \"Wrong alignment on {}\");\n",
                cls.className, 8, cls.className);
            code += std::format("static_assert(sizeof({}) == 0x{:06X}, \"Wrong size on {}\");\n",
                cls.className, structSize, cls.className);
        }

        for (auto& f : resolved) {
            if (f.offset < parentSize) continue;
            code += std::format("static_assert(offsetof({}, {}) == 0x{:06X}, "
                "\"Member '{}::{}' has a wrong offset!\");\n",
                cls.className, f.memberName, f.offset, cls.className, f.memberName);
        }

        code += "\n";
    }

    ImGui::InputTextMultiline("##export_code", code.data(), code.size() + 1,
        ImVec2(-1, 400), ImGuiInputTextFlags_ReadOnly);

    if (ImGui::Button("输出到日志文件")) {
        GetLogFile("ReverseProber")->Append("{}", code);
        LogSuccess("已输出到日志文件");
    }

    ImGui::SameLine();
    if (ImGui::Button("调用 GetEngineVersion")) {
        CallGetEngineVersion();
    }
}

// ============================================================
//  偏移量表格绘制 (通用辅助)
// ============================================================

void UEProber::DrawOffsetTable(const std::string& category) {
    // 此函数预留给更详细的分类表格
}

// ============================================================
//  Dump 面板 (AndUEDumper 集成)
// ============================================================

void UEProber::DrawDumpPanel() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "AndUEDumper 集成 Dump");
    ImGui::Spacing();

    // ---- 阶段 A: 游戏检测 ----
    if (!m_GameDetected) {
        ImGui::TextWrapped("点击下方按钮自动检测当前运行的游戏，获取 GObjects 和 FName 解析器。");
        ImGui::Spacing();
        if (ImGui::Button("检测游戏", ImVec2(200, 40))) {
            DetectGame();
        }
        return;
    }

    // ---- 游戏已检测 ----
    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "游戏: %s", m_GameDetection.gameName.c_str());
    ImGui::Text("包名: %s", m_GameDetection.packageName.c_str());
    ImGui::Text("UE Base: 0x%lX", m_GameDetection.ueBaseAddress);
    ImGui::Text("GUObjectArrayPtr: 0x%lX", m_GameDetection.guobjectArrayPtr);
    ImGui::Text("Objects Field Addr: 0x%lX", m_GameDetection.objectsFieldAddr);
    ImGui::Spacing();

    if (m_GObjectsInitialized) {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "GObjects 和 FName 已初始化，探测功能已就绪。");
        ImGui::TextWrapped("请使用阶段 1~6 进行偏移探测，完成后返回此面板执行 Dump。");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ---- 探测偏移统计 ----
    {
        static const char* kOffsetNames[] = {
            "UObject::ObjectFlags", "UObject::InternalIndex", "UObject::ClassPrivate", "UObject::NamePrivate", "UObject::OuterPrivate",
            "UField::Next",
            "UStruct::SuperStruct", "UStruct::Children", "UStruct::ChildProperties", "UStruct::PropertiesSize",
            "UFunction::FunctionFlags", "UFunction::NumParms", "UFunction::ParmsSize", "UFunction::Func",
            "FField::ClassPrivate", "FField::Next", "FField::NamePrivate", "FField::FlagsPrivate",
            "FProperty::ArrayDim", "FProperty::ElementSize", "FProperty::PropertyFlags", "FProperty::Offset_Internal", "sizeof(FProperty)",
        };
        constexpr int kTotal = sizeof(kOffsetNames) / sizeof(kOffsetNames[0]);
        int confirmed = 0;
        for (int i = 0; i < kTotal; ++i) {
            if (HasConfirmed(kOffsetNames[i])) ++confirmed;
        }
        bool useProbed = (confirmed > 0);
        if (useProbed) {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "探测偏移: 已启用 (%d/%d)", confirmed, kTotal);
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "探测偏移: 未使用 (将使用 Profile 默认偏移)");
        }
        // 显示未确认的偏移名称
        if (confirmed < kTotal) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "  未确认:");
            ImGui::Indent();
            for (int i = 0; i < kTotal; ++i) {
                if (!HasConfirmed(kOffsetNames[i])) {
                    ImGui::BulletText("%s", kOffsetNames[i]);
                }
            }
            ImGui::Unindent();
        }
    }

    ImGui::Spacing();

    // ---- 阶段 B: Dump (需要探测完成) ----
    EDumpStatus status = m_DumpStatus.load();
    switch (status) {
        case EDumpStatus::Idle:
            ImGui::Text("Dump 状态: 空闲");
            break;
        case EDumpStatus::Running:
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Dump 状态: 正在 Dump...");
            break;
        case EDumpStatus::Success:
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Dump 状态: 成功!");
            if (!m_DumpOutputDir.empty())
                ImGui::Text("输出目录: %s", m_DumpOutputDir.c_str());
            break;
        case EDumpStatus::Failed:
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Dump 状态: 失败");
            if (!m_DumpError.empty())
                ImGui::TextWrapped("错误: %s", m_DumpError.c_str());
            break;
    }

    ImGui::Spacing();

    bool canDump = (status != EDumpStatus::Running);
    if (!canDump) ImGui::BeginDisabled();

    if (ImGui::Button("开始 Dump (使用探测偏移)", ImVec2(280, 40))) {
        StartDump();
    }

    if (!canDump) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextWrapped("将当前探测到的偏移发送给 AndUEDumper 进行 SDK dump。");
}

void UEProber::DetectGame() {
    if (m_GameDetected) return;

    GameDetectionResult result;
    if (!DetectAndPrepareGame(result)) {
        LogError("游戏检测失败: 没有匹配的 Profile 或初始化失败");
        return;
    }

    m_GameDetection = result;
    m_GameDetected = true;

    LogSuccess(std::format("检测到游戏: {} ({})", result.gameName, result.packageName));
    LogInfo(std::format("UE Base: 0x{:X}, GUObjectArray: 0x{:X}, ObjectsFieldAddr: 0x{:X}",
                         result.ueBaseAddress, result.guobjectArrayPtr, result.objectsFieldAddr));

    // ---- 初始化 GObjects: 使用 profile 提供的地址, 在进程内读取 Objects 指针 ----
    auto* objArray = new TUObjectArray();
    objArray->Objects = KT::Read<FUObjectItem**>(result.objectsFieldAddr);
    objArray->MaxElements = 327680;
    objArray->NumElements = 327680;
    objArray->MaxChunks = 5;
    objArray->NumChunks = 5;
    UObject::GObjects.InitManually(objArray);
    m_GObjectsInitialized = true;

    LogSuccess("GObjects 初始化完成 (来自 Profile::GetGUObjectArrayPtr)");

    // ---- 设置 FName 解析器: 使用 profile 的 GetNameByID ----
    FName::s_NameResolver = [](int32_t id) -> std::string {
        return ProfileGetNameByID(id);
    };

    LogSuccess("FName 解析器已设置 (来自 Profile::GetNameByID)");
}

void UEProber::StartDump() {
    if (m_DumpStatus.load() == EDumpStatus::Running)
        return;

    if (!m_GameDetected) {
        m_DumpError = "请先检测游戏。";
        m_DumpStatus.store(EDumpStatus::Failed);
        return;
    }

    // 从探测结果中收集 ProbedOffsets
    ProbedOffsets offsets{};

    // UObject
    if (HasConfirmed("UObject::ObjectFlags"))          offsets.objFlags = GetConfirmedOffset("UObject::ObjectFlags");
    if (HasConfirmed("UObject::InternalIndex"))          offsets.objIndex = GetConfirmedOffset("UObject::InternalIndex");
    if (HasConfirmed("UObject::ClassPrivate"))          offsets.objClass = GetConfirmedOffset("UObject::ClassPrivate");
    if (HasConfirmed("UObject::NamePrivate"))           offsets.objName  = GetConfirmedOffset("UObject::NamePrivate");
    if (HasConfirmed("UObject::OuterPrivate"))          offsets.objOuter = GetConfirmedOffset("UObject::OuterPrivate");

    // UField
    if (HasConfirmed("UField::Next"))            offsets.fieldNext = GetConfirmedOffset("UField::Next");

    // UStruct
    if (HasConfirmed("UStruct::SuperStruct"))          offsets.structSuper     = GetConfirmedOffset("UStruct::SuperStruct");
    if (HasConfirmed("UStruct::Children"))       offsets.structChildren  = GetConfirmedOffset("UStruct::Children");
    if (HasConfirmed("UStruct::ChildProperties")) offsets.structChildProps = GetConfirmedOffset("UStruct::ChildProperties");
    if (HasConfirmed("UStruct::PropertiesSize")) offsets.structSize      = GetConfirmedOffset("UStruct::PropertiesSize");

    // UFunction
    if (HasConfirmed("UFunction::FunctionFlags")) offsets.funcFlags    = GetConfirmedOffset("UFunction::FunctionFlags");
    if (HasConfirmed("UFunction::NumParms"))       offsets.funcNumParams = GetConfirmedOffset("UFunction::NumParms");
    if (HasConfirmed("UFunction::ParmsSize"))      offsets.funcParamSize = GetConfirmedOffset("UFunction::ParmsSize");
    if (HasConfirmed("UFunction::Func"))           offsets.funcFunc      = GetConfirmedOffset("UFunction::Func");

    // FField
    if (HasConfirmed("FField::ClassPrivate"))     offsets.ffieldClass = GetConfirmedOffset("FField::ClassPrivate");
    if (HasConfirmed("FField::Next"))             offsets.ffieldNext  = GetConfirmedOffset("FField::Next");
    if (HasConfirmed("FField::NamePrivate"))      offsets.ffieldName  = GetConfirmedOffset("FField::NamePrivate");
    if (HasConfirmed("FField::FlagsPrivate"))     offsets.ffieldFlags = GetConfirmedOffset("FField::FlagsPrivate");

    // FProperty
    if (HasConfirmed("FProperty::ArrayDim"))      offsets.fpropArrayDim = GetConfirmedOffset("FProperty::ArrayDim");
    if (HasConfirmed("FProperty::ElementSize"))   offsets.fpropElemSize = GetConfirmedOffset("FProperty::ElementSize");
    if (HasConfirmed("FProperty::PropertyFlags")) offsets.fpropFlags    = GetConfirmedOffset("FProperty::PropertyFlags");
    if (HasConfirmed("FProperty::Offset_Internal")) offsets.fpropOffset = GetConfirmedOffset("FProperty::Offset_Internal");
    if (HasConfirmed("sizeof(FProperty)"))        offsets.fpropSize    = GetConfirmedOffset("sizeof(FProperty)");

    StartDumpWithProbedOffsets(offsets, m_DumpStatus, m_DumpError, m_DumpOutputDir);
}

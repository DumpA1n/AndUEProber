#include "UEProber.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <set>
#include <setjmp.h>
#include <signal.h>
#include <thread>
#include <unistd.h>

#include "Utils/ElfScanner/ElfScannerManager.h"
#include "Utils/Logger.h"

// Probe diagnostic logging switch.
#define PROBER_DEBUG 1

#if PROBER_DEBUG
#define PDBG(fmt, ...) FLOGF("[PB] " fmt, ##__VA_ARGS__)
#else
#define PDBG(fmt, ...) do {} while(0)
#endif

// Use case-insensitive comparisons for names resolved through FName.
static bool FNameEq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// ============================================================
// Signal-based probe recovery uses sigsetjmp/siglongjmp for SIGSEGV/SIGBUS.
// Some profiles call engine name-resolution functions through pointers.
// An invalid ComparisonIndex can cause a direct dereference inside the engine.
// C++ exceptions do not catch these signals; this recovery is not general C++ fault safety.
// ============================================================
static thread_local sigjmp_buf t_safeJmpBuf;
static thread_local volatile sig_atomic_t t_inSafeProbe = 0;

static struct sigaction s_prevSEGV{};
static struct sigaction s_prevBUS{};
static std::atomic<bool> s_safeProbeInstalled{false};

static void SafeProbeHandler(int sig, siginfo_t* info, void* ctx) {
    if (t_inSafeProbe) {
        t_inSafeProbe = 0;
        siglongjmp(t_safeJmpBuf, 1);
    }
    // Forward signals outside a probe to the registered crash handler.
    const struct sigaction& prev = (sig == SIGSEGV) ? s_prevSEGV : s_prevBUS;
    if (prev.sa_flags & SA_SIGINFO) {
        prev.sa_sigaction(sig, info, ctx);
    } else if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN) {
        prev.sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

static void EnsureSafeProbeInstalled() {
    if (s_safeProbeInstalled.exchange(true))
        return;
    struct sigaction sa{};
    sa.sa_sigaction = SafeProbeHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &s_prevSEGV);
    sigaction(SIGBUS, &sa, &s_prevBUS);
}

// ============================================================
// EFunctionFlags values used by the probe.
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

// EClassCastFlags values corresponding to the profile's Basic.h definitions.
namespace ECastFlags {
    constexpr uint64_t CASTCLASS_UField          = 0x0000000000000001;
    constexpr uint64_t CASTCLASS_UEnum           = 0x0000000000000004;
    constexpr uint64_t CASTCLASS_UStruct         = 0x0000000000000008;
    constexpr uint64_t CASTCLASS_UScriptStruct   = 0x0000000000000010;
    constexpr uint64_t CASTCLASS_UClass          = 0x0000000000000020;
    constexpr uint64_t CASTCLASS_UProperty       = 0x0000000000008000;
    constexpr uint64_t CASTCLASS_UFunction       = 0x0000000000080000;
    constexpr uint64_t CASTCLASS_UPackage        = 0x0000000400000000;

    // Expected Class cast flags: UField | UStruct | UClass = 0x29.
    constexpr uint64_t KNOWN_CLASS_FLAGS    = CASTCLASS_UField | CASTCLASS_UStruct | CASTCLASS_UClass; // 0x29
    // Expected Package cast flags: 0x0000000400000000.
    constexpr uint64_t KNOWN_PACKAGE_FLAGS  = CASTCLASS_UPackage;
}

// CPF property flags.
namespace ECPFFlags {
    constexpr uint64_t CPF_Parm        = 0x0000000000000080;
    constexpr uint64_t CPF_OutParm     = 0x0000000000000100;
    constexpr uint64_t CPF_ReturnParm  = 0x0000000000000400;
    constexpr uint64_t CPF_Net         = 0x0000000000000020;
}

// ============================================================
// Construction.
// ============================================================

UEProber::UEProber() {
    memset(m_DumpAddrInput, 0, sizeof(m_DumpAddrInput));

    for (int i = 0; i < 7; ++i)
        m_PhaseStatus[i] = EPhaseStatus::NotStarted;
}

// ============================================================
// Memory access.
// ============================================================

bool UEProber::TryReadFName(uintptr_t address, std::string& outName) {
    // FName layout is engine-version + build-flag dependent (UE5.1 reordered
    // Number<->DisplayIndex; case-preserving adds DisplayIndex). Read each field
    // at the matched profile's offset instead of assuming Number@+0x4 — on WW
    // (case-preserving, Number@+0x8) a fixed {Comparison,Number@4} struct reads
    // DisplayIndex as "Number" and the >0xFFFF guard rejects nearly every FName.
    const FNameLayout layout = ProfileGetFNameLayout();
    if (layout.comparisonOffset < 0)
        return false;
    const int readSize = (layout.size >= 4 && layout.size <= 16) ? layout.size : 12;
    uint8_t fnameBuf[16] = {0};
    if (!KMgrRead(address, fnameBuf, readSize))
        return false;

    int32_t comparisonIndex = 0;
    memcpy(&comparisonIndex, fnameBuf + layout.comparisonOffset, sizeof(comparisonIndex));
    if (comparisonIndex < 0 || comparisonIndex > 0x2000000)
        return false;

    // Inline Number only exists on non-outline builds; use it as a sanity gate.
    if (layout.numberOffset >= 0 && layout.numberOffset + 4 <= readSize) {
        uint32_t number = 0;
        memcpy(&number, fnameBuf + layout.numberOffset, sizeof(number));
        if (number > 0xFFFF)
            return false;
    }

    // ProfileGetNameByID can dereference native pointers internally.
    // An invalid ComparisonIndex can signal; recovery uses sigsetjmp.
    EnsureSafeProbeInstalled();

    // Use a C buffer locally; this does not make longjmp across called C++ frames safe.
    char resultBuf[1024] = {0};
    size_t resultLen = 0;

    t_inSafeProbe = 1;
    if (sigsetjmp(t_safeJmpBuf, 1) != 0) {
        // Recovered from SIGSEGV/SIGBUS.
        return false;
    }

    try {
        std::string result = ProfileGetNameByID(comparisonIndex);
        if (!result.empty() && result.size() < sizeof(resultBuf)) {
            resultLen = result.size();
            memcpy(resultBuf, result.c_str(), resultLen);
        }
    } catch (...) {
        t_inSafeProbe = 0;
        return false;
    }
    t_inSafeProbe = 0;

    if (resultLen == 0 || resultLen > 1024)
        return false;

    bool hasAlpha = false;
    for (size_t i = 0; i < resultLen; ++i) {
        char c = resultBuf[i];
        if (c < 0x20 || c > 0x7E) return false;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
            hasAlpha = true;
    }
    if (!hasAlpha) return false;

    outName = std::string(resultBuf, resultLen);
    return true;
}

bool UEProber::TryGetFullName(uintptr_t objAddr, std::string& outFullName) {
    int32_t nameOff  = GetConfirmedOffset("UObject::NamePrivate");
    int32_t classOff = GetConfirmedOffset("UObject::ClassPrivate");
    int32_t outerOff = GetConfirmedOffset("UObject::OuterPrivate");
    if (nameOff < 0 || classOff < 0 || outerOff < 0) return false;
    if (!objAddr || !IsValidPtr(objAddr)) return false;

    // Read Class->Name.
    uintptr_t classPtr = 0;
    if (!KMgrRead(objAddr + classOff, &classPtr, 8) || !IsValidPtr(classPtr)) return false;
    std::string className;
    if (!TryReadFName(classPtr + nameOff, className)) return false;

    // Build the Outer prefix with a maximum depth of 16.
    std::string outerChain;
    uintptr_t cur = objAddr;
    for (int depth = 0; depth < 16; ++depth) {
        uintptr_t outer = 0;
        if (!KMgrRead(cur + outerOff, &outer, 8) || !outer || !IsValidPtr(outer)) break;
        std::string outerName;
        if (!TryReadFName(outer + nameOff, outerName)) break;
        outerChain = outerName + "." + outerChain;
        cur = outer;
    }

    // Read the object's own name.
    std::string objName;
    if (!TryReadFName(objAddr + nameOff, objName)) return false;

    outFullName = className + " " + outerChain + objName;
    return true;
}

bool UEProber::IsValidPtr(uintptr_t ptr) {
    return KMgrIsValidPtr(ptr);
}

uintptr_t UEProber::FindObjectInGObjects(const std::string& targetName, const std::string& className) {
    int32_t namePrivateOffset = GetConfirmedOffset("UObject::NamePrivate");
    int32_t classPrivateOffset = GetConfirmedOffset("UObject::ClassPrivate");
    if (namePrivateOffset < 0) return 0;

    bool filterByClass = !className.empty() && classPrivateOffset >= 0;
    int32_t count = BridgeGetObjectNum();

    for (int32_t i = 1; i < count; ++i) {
        uintptr_t obj = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(i));
        if (!obj || !IsValidPtr(obj)) continue;

        if (filterByClass) {
            uintptr_t objClass = 0;
            if (!KMgrRead(obj + classPrivateOffset, &objClass, 8) || !IsValidPtr(objClass)) continue;
            std::string clsName;
            if (!TryReadFName(objClass + namePrivateOffset, clsName) || !FNameEq(clsName, className)) continue;
        }

        std::string name;
        if (!TryReadFName(obj + namePrivateOffset, name)) continue;
        if (FNameEq(name, targetName)) return obj;
    }
    return 0;
}

void UEProber::DetectReflectionModel() {
    if (m_ReflectionModel != EReflectionModel::Unknown) return;

    int32_t nameOff  = GetConfirmedOffset("UObject::NamePrivate");
    int32_t classOff = GetConfirmedOffset("UObject::ClassPrivate");
    if (nameOff < 0 || classOff < 0) {
        PDBG("DetectReflectionModel: Name/Class 未确认, 暂不判别 (保持 Unknown -> 默认 FField 行为)");
        return;
    }

    bool foundInstance = false;
    int32_t total = BridgeGetObjectNum();
    for (int32_t i = 1; i < total; ++i) {
        uintptr_t obj = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(i));
        if (!obj || !IsValidPtr(obj)) continue;
        uintptr_t cls = 0;
        if (!KMgrRead(obj + classOff, &cls, 8) || !IsValidPtr(cls)) continue;
        std::string clsName;
        if (!TryReadFName(cls + nameOff, clsName) || !clsName.ends_with("Property")) continue;
        std::string objName;
        TryReadFName(obj + nameOff, objName);
        if (objName.rfind("Default__", 0) == 0) continue;  // Property UClass CDOs occur in both reflection models.
        foundInstance = true;
        PDBG("DetectReflectionModel: 命中属性实例 [{}] class=\"{}\" name=\"{}\"", i, clsName, objName);
        break;
    }
    m_ReflectionModel = foundInstance ? EReflectionModel::UProperty : EReflectionModel::FField;
    PDBG("DetectReflectionModel: {} 模型 (非CDO属性实例 {})",
         foundInstance ? "UProperty(<=4.24)" : "FField(4.25+)",
         foundInstance ? "存在" : "不存在");
}

int32_t UEProber::GetStructSize(const std::string& structName) {
    int32_t sizeOff = GetConfirmedOffset("UStruct::PropertiesSize");
    if (sizeOff < 0) return 0;

    // Prefer the cached address.
    uintptr_t addr = 0;
    if (structName == "UObject") {
        addr = m_ClassObject;
        if (!addr) addr = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(1));
    } else if (structName == "UStruct")   addr = m_ClassStruct;
    else if (structName == "UClass")      addr = m_ClassClass;
    else if (structName == "UField")      addr = m_ClassField;
    else if (structName == "UFunction")   addr = m_ClassFunction;

    // Otherwise search GObjects for the matching UClass or UScriptStruct.
    if (!addr) {
        // Remove the U/A/F type prefix before matching an object name.
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
    KMgrRead(structAddr + sizeOff, &size, 4);
    return size;
}

// ============================================================
// Helpers.
// ============================================================

uintptr_t UEProber::GetTextSegStart() {
    return Elf.UE4().base();
}

uintptr_t UEProber::GetTextSegEnd() {
    return Elf.UE4().end();
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
// Logging.
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
// Phase 1: UObject fields.
// ============================================================

void UEProber::Phase1_ProbeInternalIndex(uintptr_t objAddr, int32_t expectedIndex) {
    PDBG(">>>>>>>>>> [ProbeInternalIndex] BEGIN >>>>>>>>>>");
    m_Phase1InternalIndexCandidates.clear();
    PDBG("ProbeInternalIndex: objAddr=0x{:X}, expectedIndex={}", objAddr, expectedIndex);

    for (int32_t off = 0; off < m_ProbeRange; off += 4) {
        uint32_t val = 0;
        if (!KMgrRead(objAddr + off, &val, 4)) continue;
        if ((int32_t)val == expectedIndex) {
            float confidence = 0.5f;
            // Cross-check other objects.
            int crossMatch = 0;
            for (int idx = 0; idx <= 3; ++idx) {
                if (idx == expectedIndex) { crossMatch++; continue; }
                uintptr_t otherObj = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(idx));
                if (!otherObj) continue;
                uint32_t otherVal = 0;
                if (KMgrRead(otherObj + off, &otherVal, 4) && (int32_t)otherVal == idx)
                    crossMatch++;
            }
            confidence = (float)crossMatch / 4.0f;

            PDBG("ProbeInternalIndex: 偏移 0x{:X} -> 值={}, 交叉验证 {}/4, confidence={:.2f}",
                 off, val, crossMatch, confidence);
            m_Phase1InternalIndexCandidates.push_back({
                off, val,
                std::format("在 obj[{}] 偏移 0x{:X} 找到值 {}, 交叉验证 {}/4",
                    expectedIndex, off, val, crossMatch),
                confidence
            });

            // Stop after all cross-checks match.
            if (confidence >= 1.0f) {
                PDBG("ProbeInternalIndex: 置信度 100%, early break");
                break;
            }
        }
    }

    PDBG("ProbeInternalIndex: 扫描完毕, 候选数={}", m_Phase1InternalIndexCandidates.size());
    std::sort(m_Phase1InternalIndexCandidates.begin(), m_Phase1InternalIndexCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase1InternalIndexCandidates.empty()) {
        auto& best = m_Phase1InternalIndexCandidates[0];
        PDBG("ProbeInternalIndex: 最佳候选 偏移=0x{:X}, confidence={:.2f}", best.offset, best.confidence);
        if (best.confidence >= 0.75f) {
            auto& r = GetResult("UObject::InternalIndex");
            r.offset = best.offset;
            r.size = 4;
            r.typeName = "int32";
            r.evidence = best.description;
            r.autoDetected = true;
            if (best.confidence >= 1.0f) r.confirmed = true;
            PDBG("ProbeInternalIndex: UObject::InternalIndex 自动探测: 偏移 0x{:X} (置信度 {:.0f}%){}",
                best.offset, best.confidence * 100, r.confirmed ? " [已自动确认]" : "");
        } else {
            PDBG("ProbeInternalIndex: 最佳候选置信度不足 (< 0.75), 不写入结果");
        }
    } else {
        PDBG("ProbeInternalIndex: 无候选项");
    }
    PDBG("<<<<<<<<<< [ProbeInternalIndex] END <<<<<<<<<<");
}

void UEProber::Phase1_ProbeNamePrivate(uintptr_t objAddr, const std::string& expectedName) {
    PDBG(">>>>>>>>>> [ProbeNamePrivate] BEGIN >>>>>>>>>>");
    m_Phase1NamePrivateCandidates.clear();
    PDBG("ProbeNamePrivate: objAddr=0x{:X}, expectedName=\"{}\"", objAddr, expectedName);

    const int32_t maxRange = std::min(m_ProbeRange, 0x40);
    PDBG("ProbeNamePrivate: 搜索范围 [0x0, 0x{:X})", maxRange);

    for (int32_t off = 0; off < maxRange; off += 4) {
        std::string name;
        if (!TryReadFName(objAddr + off, name)) continue;

        float confidence = 0.0f;
        if (FNameEq(name, expectedName))
            confidence = 1.0f;
        else if (!name.empty() && name.size() < 256)
            confidence = 0.1f;
        else
            continue;

        PDBG("ProbeNamePrivate: off=0x{:X} name=\"{}\", confidence={:.2f}", off, name, confidence);
        if (confidence > 0.05f) {
            m_Phase1NamePrivateCandidates.push_back({
                off, 0,
                std::format("偏移 0x{:X} -> Name=\"{}\"", off, name),
                confidence
            });
        }

        if (confidence >= 1.0f) {
            PDBG("ProbeNamePrivate: 精确匹配, early break");
            break;
        }
    }

    PDBG("ProbeNamePrivate: 扫描完毕, 候选数={}", m_Phase1NamePrivateCandidates.size());
    std::sort(m_Phase1NamePrivateCandidates.begin(), m_Phase1NamePrivateCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase1NamePrivateCandidates.empty() && m_Phase1NamePrivateCandidates[0].confidence >= 0.9f) {
        auto& best = m_Phase1NamePrivateCandidates[0];
        PDBG("ProbeNamePrivate: 最佳候选 偏移=0x{:X}, confidence={:.2f}, desc={}", best.offset, best.confidence, best.description);
        auto& r = GetResult("UObject::NamePrivate");
        r.offset = best.offset;
        r.size = 8;
        r.typeName = "FName";
        r.evidence = best.description;
        r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeNamePrivate: UObject::NamePrivate 自动探测: 偏移 0x{:X}{}", best.offset, r.confirmed ? " [已自动确认]" : "");
    } else {
        PDBG("ProbeNamePrivate: 无满足置信度的候选项");
    }
    PDBG("<<<<<<<<<< [ProbeNamePrivate] END <<<<<<<<<<");
}

void UEProber::Phase1_ProbeClassPrivate(uintptr_t objAddr, const std::string& expectedClassName) {
    PDBG(">>>>>>>>>> [ProbeClassPrivate] BEGIN >>>>>>>>>>");
    m_Phase1ClassPrivateCandidates.clear();
    PDBG("ProbeClassPrivate: objAddr=0x{:X}, expectedClassName=\"{}\"", objAddr, expectedClassName);
    int32_t namePrivateOffset = GetConfirmedOffset("UObject::NamePrivate");
    PDBG("ProbeClassPrivate: namePrivateOffset=0x{:X}", namePrivateOffset);
    if (namePrivateOffset < 0) {
        PDBG("ProbeClassPrivate: namePrivateOffset < 0, 中止");
        PDBG("<<<<<<<<<< [ProbeClassPrivate] END <<<<<<<<<<");
        return;
    }

    for (int32_t off = 0; off < m_ProbeRange; off += 8) {
        uintptr_t ptr = 0;
        if (!KMgrRead(objAddr + off, &ptr, 8)) continue;
        if (!IsValidPtr(ptr)) continue;

        std::string name;
        if (TryReadFName(ptr + namePrivateOffset, name)) {
            float confidence = 0.0f;
            if (name == expectedClassName)
                confidence = 1.0f;
            else if (!name.empty())
                confidence = 0.1f;

            PDBG("ProbeClassPrivate: 偏移 0x{:X} -> ptr=0x{:X}, Name=\"{}\", confidence={:.2f}",
                 off, ptr, name, confidence);
            if (confidence > 0.05f) {
                m_Phase1ClassPrivateCandidates.push_back({
                    off, ptr,
                    std::format("偏移 0x{:X} -> 0x{:X} -> Name = \"{}\"",
                        off, ptr, name),
                    confidence
                });
            }

            // Stop after an exact match.
            if (confidence >= 1.0f) {
                PDBG("ProbeClassPrivate: 精确匹配, early break");
                break;
            }
        }
    }

    PDBG("ProbeClassPrivate: 扫描完毕, 候选数={}", m_Phase1ClassPrivateCandidates.size());
    std::sort(m_Phase1ClassPrivateCandidates.begin(), m_Phase1ClassPrivateCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase1ClassPrivateCandidates.empty() && m_Phase1ClassPrivateCandidates[0].confidence >= 0.9f) {
        auto& best = m_Phase1ClassPrivateCandidates[0];
        PDBG("ProbeClassPrivate: 最佳候选 偏移=0x{:X}, confidence={:.2f}", best.offset, best.confidence);
        auto& r = GetResult("UObject::ClassPrivate");
        r.offset = best.offset;
        r.size = 8;
        r.typeName = "UClass*";
        r.evidence = best.description;
        r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeClassPrivate: UObject::ClassPrivate 自动探测: 偏移 0x{:X}{}", best.offset, r.confirmed ? " [已自动确认]" : "");
    } else {
        PDBG("ProbeClassPrivate: 无满足置信度的候选项");
    }
    PDBG("<<<<<<<<<< [ProbeClassPrivate] END <<<<<<<<<<");
}

void UEProber::Phase1_ProbeOuterPrivate(uintptr_t obj2Addr, uintptr_t obj1Addr) {
    PDBG(">>>>>>>>>> [ProbeOuterPrivate] BEGIN >>>>>>>>>>");
    m_Phase1OuterPrivateCandidates.clear();
    PDBG("ProbeOuterPrivate: obj2Addr=0x{:X}, expected obj1Addr=0x{:X}", obj2Addr, obj1Addr);

    for (int32_t off = 0; off < m_ProbeRange; off += 8) {
        uintptr_t ptr = 0;
        if (!KMgrRead(obj2Addr + off, &ptr, 8)) continue;

        if (ptr == obj1Addr) {
            PDBG("ProbeOuterPrivate: 偏移 0x{:X} -> 精确匹配 obj1Addr={}", off, ptr);
            m_Phase1OuterPrivateCandidates.push_back({
                off, ptr,
                std::format("偏移 0x{:X} -> 0x{:X} == obj[1] 地址", off, ptr),
                1.0f
            });
            // Stop after an exact match.
            PDBG("ProbeOuterPrivate: 精确匹配, early break");
            break;
        } else if (IsValidPtr(ptr)) {
            int32_t namePrivateOffset = GetConfirmedOffset("UObject::NamePrivate");
            std::string name;
            if (namePrivateOffset >= 0 && TryReadFName(ptr + namePrivateOffset, name)) {
                m_Phase1OuterPrivateCandidates.push_back({
                    off, ptr,
                    std::format("偏移 0x{:X} -> 0x{:X} -> Name = \"{}\"",
                        off, ptr, name),
                    0.1f
                });
            }
        }
    }

    PDBG("ProbeOuterPrivate: 扫描完毕, 候选数={}", m_Phase1OuterPrivateCandidates.size());
    std::sort(m_Phase1OuterPrivateCandidates.begin(), m_Phase1OuterPrivateCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase1OuterPrivateCandidates.empty() && m_Phase1OuterPrivateCandidates[0].confidence >= 0.9f) {
        auto& best = m_Phase1OuterPrivateCandidates[0];
        PDBG("ProbeOuterPrivate: 最佳候选 偏移=0x{:X}, confidence={:.2f}", best.offset, best.confidence);
        auto& r = GetResult("UObject::OuterPrivate");
        r.offset = best.offset;
        r.size = 8;
        r.typeName = "UObject*";
        r.evidence = best.description;
        r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeOuterPrivate: UObject::OuterPrivate 自动探测: 偏移 0x{:X}{}", best.offset, r.confirmed ? " [已自动确认]" : "");
    } else {
        PDBG("ProbeOuterPrivate: 无满足置信度的候选项");
    }
    PDBG("<<<<<<<<<< [ProbeOuterPrivate] END <<<<<<<<<<");
}

void UEProber::Phase1_ProbeObjectFlags(uintptr_t objAddr) {
    PDBG(">>>>>>>>>> [ProbeObjectFlags] BEGIN >>>>>>>>>>");
    m_Phase1ObjectFlagsCandidates.clear();
    PDBG("ProbeObjectFlags: objAddr=0x{:X}", objAddr);

    // Exclude established offsets.
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
        if (!KMgrRead(objAddr + off, &val, 4)) continue;

        // The anchor heuristic expects obj[0] flags to equal RF_Public (1).
        if (val != 1) continue;

        // Cross-check that other object flags are nonzero and within the expected mask.
        int validCount = 0;
        for (int idx = 0; idx <= 3; ++idx) {
            uintptr_t otherObj = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(idx));
            if (!otherObj) continue;
            uint32_t otherVal = 0;
            if (KMgrRead(otherObj + off, &otherVal, 4) && otherVal > 0 && otherVal <= 0x03FFFFFF)
                validCount++;
        }

        float confidence = (float)validCount / 4.0f;
        PDBG("ProbeObjectFlags: 偏移 0x{:X} -> 0x{:08X}, 交叉验证 {}/4, confidence={:.2f}",
             off, val, validCount, confidence);
        m_Phase1ObjectFlagsCandidates.push_back({
            off, val,
            std::format("偏移 0x{:X} -> 0x{:08X}, {}/4 对象有合理 Flag 值", off, val, validCount),
            confidence
        });

        // Stop after all cross-checks match.
        if (confidence >= 1.0f) {
            PDBG("ProbeObjectFlags: 置信度 100%, early break");
            break;
        }
    }

    PDBG("ProbeObjectFlags: 扫描完毕, 候选数={}", m_Phase1ObjectFlagsCandidates.size());
    std::sort(m_Phase1ObjectFlagsCandidates.begin(), m_Phase1ObjectFlagsCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase1ObjectFlagsCandidates.empty() && m_Phase1ObjectFlagsCandidates[0].confidence >= 0.75f) {
        auto& best = m_Phase1ObjectFlagsCandidates[0];
        PDBG("ProbeObjectFlags: 最佳候选 偏移=0x{:X}, confidence={:.2f}", best.offset, best.confidence);
        auto& r = GetResult("UObject::ObjectFlags");
        r.offset = best.offset;
        r.size = 4;
        r.typeName = "int32";
        r.evidence = best.description;
        r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeObjectFlags: UObject::ObjectFlags 自动探测: 偏移 0x{:X} (置信度 {:.0f}%){}",
            best.offset, best.confidence * 100, r.confirmed ? " [已自动确认]" : "");
    } else {
        PDBG("ProbeObjectFlags: 无满足置信度的候选项");
    }
    PDBG("<<<<<<<<<< [ProbeObjectFlags] END <<<<<<<<<<");
}

void UEProber::Phase1_AutoProbe() {
    if (!m_GameDetected) { PDBG("请先检测游戏后再进行探测操作"); return; }
    PDBG(">>>>>>>>>> [Phase1_AutoProbe] BEGIN >>>>>>>>>>");
    m_PhaseStatus[1] = EPhaseStatus::InProgress;
    PDBG("===== 阶段 1: UObject 基础成员自动探测 =====");

    // This layout assumes the vtable pointer is at offset zero.
    {
        auto& r = GetResult("UObject::VTable");
        r.offset = 0; r.size = 8; r.typeName = "void**";
        r.evidence = "VTable 始终在偏移 0";
        r.autoDetected = true; r.confirmed = true;
        PDBG("UObject::VTable = 0x00 (固定)");
    }

    // GetByIndex(0) -> Package /Script/CoreUObject, Outer=nullptr, Flags=RF_Public(1)
    // GetByIndex(1) -> Class /Script/CoreUObject.Object, Class="Class", Outer=obj[0]
    uintptr_t obj0 = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(0));
    uintptr_t obj1 = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(1));

    if (!obj1 || !IsValidPtr(obj1)) {
        PDBG("Phase1_AutoProbe: 无法获取有效的 GetByIndex(1), 请检查 GObjects 地址");
        m_PhaseStatus[1] = EPhaseStatus::Failed;
        PDBG("<<<<<<<<<< [Phase1_AutoProbe] END <<<<<<<<<<");
        return;
    }

    PDBG("obj[0] = 0x{:X}", obj0);
    PDBG("obj[1] = 0x{:X}", obj1);

    // Probe obj[1].InternalIndex against 1.
    Phase1_ProbeInternalIndex(obj1, 1);

    // Probe obj[1].NamePrivate against Object.
    Phase1_ProbeNamePrivate(obj1, "Object");

    // Once Name is available, probe obj[1].ClassPrivate against Class.
    if (HasConfirmed("UObject::NamePrivate")) {
        Phase1_ProbeClassPrivate(obj1, "Class");
    }

    // Probe obj[1].OuterPrivate against obj[0].
    if (IsValidPtr(obj0)) {
        Phase1_ProbeOuterPrivate(obj1, obj0);
    }

    // Probe obj[0] flags against RF_Public and cross-check adjacent objects.
    if (IsValidPtr(obj0)) {
        Phase1_ProbeObjectFlags(obj0);
    }

    // Cross-check the full name after Name, Class and Outer are confirmed.
    if (HasConfirmed("UObject::NamePrivate") &&
        HasConfirmed("UObject::ClassPrivate") &&
        HasConfirmed("UObject::OuterPrivate")) {
        PDBG("--- GetFullName 验证 ---");
        for (int32_t idx = 0; idx < 5; ++idx) {
            uintptr_t obj = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(idx));
            if (!IsValidPtr(obj)) continue;
            std::string fullName;
            if (TryGetFullName(obj, fullName)) {
                PDBG("  obj[{}] = {}", idx, fullName);
            } else {
                PDBG("  obj[{}] = <GetFullName 失败>", idx);
            }
        }
    }

    PDBG("阶段 1 自动探测完成");
    PDBG("<<<<<<<<<< [Phase1_AutoProbe] END <<<<<<<<<<");
}

// ============================================================
// Phase 2: UField and UStruct.
// ============================================================

void UEProber::Phase2_ProbeSuperStruct(uintptr_t classAddr) {
    PDBG(">>>>>>>>>> [ProbeSuper] BEGIN >>>>>>>>>>");
    m_Phase2SuperStructCandidates.clear();
    int32_t namePrivateOffset = GetConfirmedOffset("UObject::NamePrivate");
    PDBG("ProbeSuper: classAddr=0x{:X}, namePrivateOffset=0x{:X}", classAddr, namePrivateOffset);
    if (namePrivateOffset < 0) { PDBG("ProbeSuper: namePrivateOffset < 0, 中止"); PDBG("<<<<<<<<<< [ProbeSuper] END <<<<<<<<<<"); return; }

    // classAddr is the Class pointer of object-array entry zero, expected to be Package.
    // The anchor expects its Super to point to the Object UClass at entry one.
    uintptr_t obj1 = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(1));
    PDBG("ProbeSuper: obj[1]=0x{:X}", obj1);

    // Search the assumed UStruct region between UObject and UStruct sizes.
    // The search depends on the previously resolved size results.
    int32_t sizeofUObject = GetStructSize("UObject");
    int32_t sizeofUStruct = GetStructSize("UStruct");
    int32_t searchStart = sizeofUObject;
    int32_t searchEnd = sizeofUStruct > 0 ? sizeofUStruct : sizeofUObject * 3;
    PDBG("ProbeSuper: 搜索范围 [0x{:X}, 0x{:X}), sizeofUObject=0x{:X}, sizeofUStruct=0x{:X}",
         searchStart, searchEnd, sizeofUObject, sizeofUStruct);

    int scannedPtrs = 0;
    for (int32_t off = searchStart; off < searchEnd; off += 8) {
        uintptr_t ptr = 0;
        if (!KMgrRead(classAddr + off, &ptr, 8)) continue;
        if (!IsValidPtr(ptr)) continue;
        scannedPtrs++;

        std::string name;
        if (!TryReadFName(ptr + namePrivateOffset, name)) continue;

        float confidence = 0.0f;
        if (ptr == obj1 || FNameEq(name, "Object")) {
            PDBG("ProbeSuper: off=0x{:X} 匹配 obj1/Object, ptr=0x{:X} name=\"{}\", 开始验证链",
                 off, ptr, name);
            // Match the entry-one address or expected name.
            // Cross-check that the Class metaclass has Struct as its Super.
            int32_t classPrivateOffset = GetConfirmedOffset("UObject::ClassPrivate");
            if (classPrivateOffset >= 0) {
                uintptr_t objClass = 0;
                if (KMgrRead(ptr + classPrivateOffset, &objClass, 8) && IsValidPtr(objClass)) {
                    // objClass is expected to be Object, whose Class is the Class metaclass.
                    uintptr_t classClassAddr = 0;
                    if (KMgrRead(objClass + classPrivateOffset, &classClassAddr, 8) && IsValidPtr(classClassAddr)) {
                        // Verify Class->Super resolves to Struct.
                        uintptr_t classSuperPtr = 0;
                        if (KMgrRead(classClassAddr + off, &classSuperPtr, 8) && IsValidPtr(classSuperPtr)) {
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
                            PDBG("ProbeSuper: Class->Super 读取失败/无效 (classSuperPtr=0x{:X}), conf=0.6", classSuperPtr);
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

        PDBG("ProbeSuper: off=0x{:X} ptr=0x{:X} name=\"{}\" confidence={:.2f}",
             off, ptr, name, confidence);

        if (confidence > 0.05f) {
            m_Phase2SuperStructCandidates.push_back({
                off, ptr,
                std::format("偏移 0x{:X} -> 0x{:X} -> Name = \"{}\"", off, ptr, name),
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

        // Cache UClass addresses along the inheritance chain.
        // Object-array entry one is expected to be the Object UClass.
        m_ClassObject = obj1;
        int32_t classPrivateOffset = GetConfirmedOffset("UObject::ClassPrivate");
        if (classPrivateOffset >= 0) {
            // Its Class pointer is expected to identify the Class metaclass.
            uintptr_t classUClass = 0;
            if (KMgrRead(obj1 + classPrivateOffset, &classUClass, 8) && IsValidPtr(classUClass)) {
                m_ClassClass = classUClass;
                // Class->Super is expected to identify Struct.
                uintptr_t structAddr = 0;
                if (KMgrRead(classUClass + best.offset, &structAddr, 8) && IsValidPtr(structAddr))
                    m_ClassStruct = structAddr;
                // Struct->Super is expected to identify Field.
                if (m_ClassStruct) {
                    uintptr_t fieldAddr = 0;
                    if (KMgrRead(m_ClassStruct + best.offset, &fieldAddr, 8) && IsValidPtr(fieldAddr))
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

int32_t UEProber::Phase2_ProbeUObjectSize() {
    PDBG(">>>>>>>>>> [ProbeUObjectSize] BEGIN >>>>>>>>>>");
    int32_t namePrivateOffset = GetConfirmedOffset("UObject::NamePrivate");
    int32_t classPrivateOffset = GetConfirmedOffset("UObject::ClassPrivate");
    if (namePrivateOffset < 0 || classPrivateOffset < 0) {
        PDBG("ProbeUObjectSize: 前置偏移不全, 中止");
        PDBG("<<<<<<<<<< [ProbeUObjectSize] END <<<<<<<<<<");
        return 0;
    }

    int32_t maxEnd = 0;
    for (const char* k : {"UObject::ObjectFlags", "UObject::InternalIndex", "UObject::ClassPrivate",
                          "UObject::NamePrivate", "UObject::OuterPrivate"}) {
        int32_t o = GetConfirmedOffset(k);
        if (o < 0) continue;
        int32_t sz = GetResult(k).size;
        int32_t end = o + (sz > 0 ? sz : 8);
        if (end > maxEnd) maxEnd = end;
    }
    if (maxEnd <= 0) maxEnd = 0x28;
    int32_t lo = (maxEnd + 7) & ~7;
    int32_t hi = maxEnd + 0x30;
    PDBG("ProbeUObjectSize: Range [0x{:X}, 0x{:X}]", lo, hi);

    uintptr_t kismet = FindObjectInGObjects("KismetSystemLibrary", "Class");
    if (!kismet) {
        PDBG("ProbeUObjectSize: KismetSystemLibrary 未找到, 中止");
        PDBG("<<<<<<<<<< [ProbeUObjectSize] END <<<<<<<<<<");
        return 0;
    }

    // Scan candidate Children offsets and candidate Next offsets to infer the UObject boundary.
    // Prefer the longest sibling chain with matching ClassPrivate, avoiding name calls.
    int bestChain = 0; int32_t bestNext = 0, bestHead = 0;
    for (int32_t offC = lo; offC <= hi; offC += 8) {
        uintptr_t head = 0;
        if (!KMgrRead(kismet + offC, &head, 8) || !IsValidPtr(head)) continue;
        uintptr_t headClass = 0;
        if (!KMgrRead(head + classPrivateOffset, &headClass, 8) || !IsValidPtr(headClass)) continue;

        for (int32_t offN = lo; offN <= hi; offN += 8) {
            int chain = 1;
            uintptr_t cur = head;
            for (int j = 0; j < 64; ++j) {
                uintptr_t nn = 0;
                if (!KMgrRead(cur + offN, &nn, 8) || !IsValidPtr(nn)) break;
                uintptr_t nc = 0;
                if (!KMgrRead(nn + classPrivateOffset, &nc, 8) || nc != headClass) break;
                chain++; cur = nn;
            }
            if (chain > bestChain) { bestChain = chain; bestNext = offN; bestHead = offC; }
        }
    }

    if (bestChain >= 4 && bestNext > 0) {
        std::string headName;
        uintptr_t headPtr = 0; KMgrRead(kismet + bestHead, &headPtr, 8);
        TryReadFName(headPtr + namePrivateOffset, headName);
        PDBG("ProbeUObjectSize: sizeof(UObject)=0x{:X} (UField::Next; ChildrenOff=0x{:X} head=\"{}\" 链长={})",
             bestNext, bestHead, headName, bestChain);
        PDBG("<<<<<<<<<< [ProbeUObjectSize] END <<<<<<<<<<");
        return bestNext;
    }
    PDBG("ProbeUObjectSize: 无足够长的兄弟链 (最长={}), 中止", bestChain);
    PDBG("<<<<<<<<<< [ProbeUObjectSize] END <<<<<<<<<<");
    return 0;
}

void UEProber::Phase2_ProbePropertiesSize(uintptr_t objectUClass, int32_t sizeofUObject) {
    PDBG(">>>>>>>>>> [ProbePropertiesSize] BEGIN >>>>>>>>>>");
    PDBG("ProbePropertiesSize: objectUClass=0x{:X}, sizeof(UObject)=0x{:X}", objectUClass, sizeofUObject);
    if (!objectUClass || sizeofUObject <= 0) {
        PDBG("ProbePropertiesSize: 入参无效, 中止");
        PDBG("<<<<<<<<<< [ProbePropertiesSize] END <<<<<<<<<<");
        return;
    }

    // The Object UClass PropertiesSize candidate is matched against the inferred UObject size.
    // Search the assumed UStruct range and cross-check the Struct UClass size
    // to reject unrelated integers with the same value.
    uintptr_t structClass = m_ClassStruct ? m_ClassStruct : FindObjectInGObjects("Struct", "Class");
    for (int32_t off = sizeofUObject; off <= sizeofUObject + 0x40; off += 4) {
        int32_t v = 0;
        if (!KMgrRead(objectUClass + off, &v, 4) || v != sizeofUObject) continue;

        int32_t vStruct = 0;
        bool xok = structClass && KMgrRead(structClass + off, &vStruct, 4) &&
                   vStruct > sizeofUObject && vStruct < 0x400;
        if (structClass && !xok) {
            PDBG("ProbePropertiesSize: off=0x{:X} 值匹配但 Struct 交叉验证失败(struct=0x{:X}), 跳过", off, vStruct);
            continue;
        }

        auto& r = GetResult("UStruct::PropertiesSize");
        r.offset = off; r.size = 4; r.typeName = "int32"; r.autoDetected = true; r.confirmed = true;
        r.evidence = std::format("偏移 0x{:X}: Object 值==sizeof(UObject)=0x{:X}, Struct 值=0x{:X}", off, sizeofUObject, vStruct);
        PDBG("ProbePropertiesSize: 选定 offset=0x{:X} (Object=0x{:X}, Struct=0x{:X})", off, sizeofUObject, vStruct);

        // Derive MinAlignment from the assumed int32 field adjacent to PropertiesSize; custom layouts may differ.
        auto& rm = GetResult("UStruct::MinAlignment");
        rm.offset = off + 4; rm.size = 4; rm.typeName = "int32"; rm.autoDetected = true; rm.confirmed = true;
        rm.evidence = std::format("派生: PropertiesSize(0x{:X})+4", off);
        PDBG("ProbePropertiesSize: 派生 MinAlignment offset=0x{:X}", off + 4);
        PDBG("<<<<<<<<<< [ProbePropertiesSize] END <<<<<<<<<<");
        return;
    }
    PDBG("ProbePropertiesSize: [0x{:X},0x{:X}) 内未找到值==sizeof(UObject) 的字段, 中止",
         sizeofUObject, sizeofUObject + 0x40);
    PDBG("<<<<<<<<<< [ProbePropertiesSize] END <<<<<<<<<<");
}

void UEProber::Phase2_ProbeChildren(uintptr_t classAddr) {
    PDBG(">>>>>>>>>> [ProbeChildren] BEGIN >>>>>>>>>>");
    m_Phase2ChildrenCandidates.clear();
    int32_t namePrivateOffset = GetConfirmedOffset("UObject::NamePrivate");
    PDBG("ProbeChildren: classAddr=0x{:X}, namePrivateOffset=0x{:X}", classAddr, namePrivateOffset);
    if (namePrivateOffset < 0) { PDBG("ProbeChildren: namePrivateOffset < 0, 中止"); PDBG("<<<<<<<<<< [ProbeChildren] END <<<<<<<<<<"); return; }

    // Search the assumed UStruct region for Children.
    // The search depends on the previously resolved size results.
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
        if (!KMgrRead(classAddr + off, &ptr, 8)) continue;
        if (!IsValidPtr(ptr)) continue;
        scannedCount++;
        validPtrCount++;

        // Check target readability.
        uint64_t testRead = 0;
        if (!KMgrRead(ptr, &testRead, 8)) continue;

        std::string name;
        if (TryReadFName(ptr + namePrivateOffset, name) && !name.empty()) {
            float confidence = 0.0f;
            if (FNameEq(name, "ExecuteUbergraph"))
                confidence = 1.0f;
            else {
                // Check whether Class->Name is Function.
                int32_t classPrivateOffset = GetConfirmedOffset("UObject::ClassPrivate");
                if (classPrivateOffset >= 0) {
                    uintptr_t childClass = 0;
                    if (KMgrRead(ptr + classPrivateOffset, &childClass, 8) && IsValidPtr(childClass)) {
                        std::string className;
                        if (TryReadFName(childClass + namePrivateOffset, className) && FNameEq(className, "Function"))
                            confidence = 0.5f;
                    }
                }
                if (confidence == 0.0f)
                    confidence = 0.1f;
            }

            PDBG("ProbeChildren: off=0x{:X} ptr=0x{:X} name=\"{}\" conf={:.2f}",
                 off, ptr, name, confidence);

            m_Phase2ChildrenCandidates.push_back({
                off, ptr,
                std::format("偏移 0x{:X} -> 0x{:X} -> Name = \"{}\"", off, ptr, name),
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
    PDBG("ProbeChildProperties: classAddr=0x{:X}, childrenOffset=0x{:X}",
         classAddr, childrenOffset);
    if (childrenOffset < 0) { PDBG("ProbeChildProperties: childrenOffset < 0, 中止"); PDBG("<<<<<<<<<< [ProbeChildProperties] END <<<<<<<<<<"); return; }

    // Probe ChildProperties using the ExecuteUbergraph anchor;
    // its first field is expected to be named EntryPoint.
    uintptr_t childrenAddr = 0;
    if (!KMgrRead(classAddr + childrenOffset, &childrenAddr, 8) || !IsValidPtr(childrenAddr)) {
        PDBG("ProbeChildProperties: 无法读取 Children 地址, KMgrRead(classAddr+0x{:X}) 失败或 childrenAddr=0x{:X} 无效",
             childrenOffset, childrenAddr);
        PDBG("<<<<<<<<<< [ProbeChildProperties] END <<<<<<<<<<");
        return;
    }

    PDBG("ProbeChildProperties: childrenAddr(ExecuteUbergraph)=0x{:X}", childrenAddr);

    // Search the assumed UStruct region for ChildProperties.
    int32_t searchStart = GetStructSize("UObject");
    int32_t searchEnd = GetStructSize("UStruct");
    if (searchEnd <= searchStart) searchEnd = searchStart * 3;
    PDBG("ProbeChildProperties: 搜索范围 [0x{:X}, 0x{:X})", searchStart, searchEnd);

    for (int32_t off = searchStart; off < searchEnd; off += 8) {
        // Skip the known Children offset.
        if (off == childrenOffset) continue;

        uintptr_t ptr = 0;
        if (!KMgrRead(childrenAddr + off, &ptr, 8)) continue;
        if (!IsValidPtr(ptr)) continue;

        // Check target readability.
        uint64_t testRead = 0;
        if (!KMgrRead(ptr, &testRead, 8)) continue;

        // ChildProperties points to FField, not UObject.
        // Try FName candidates at different FField offsets.
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
                    bestDesc = std::format("偏移 0x{:X} -> 0x{:X} -> FField Name@0x{:X} = \"{}\"",
                        off, ptr, namePrivateOff, name);
                }
                if (conf >= 1.0f) break;
            }
        }

        if (bestConf > 0.05f) {
            PDBG("ProbeChildProperties: off=0x{:X} ptr=0x{:X} bestName=\"{}\" conf={:.2f}",
                 off, ptr, bestDesc, bestConf);
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
    PDBG("ProbeUFieldNext: KismetSystemLibrary @ 0x{:X}", kismetClass);

    // Read the first UFunction from UClass::Children.
    uintptr_t firstFunc = 0;
    if (!KMgrRead(kismetClass + childrenOffset, &firstFunc, 8) || !IsValidPtr(firstFunc)) {
        PDBG("ProbeUFieldNext: KMgrRead(kismetClass+0x{:X}) 失败或 firstFunc=0x{:X} 无效",
             childrenOffset, firstFunc);
        PDBG("<<<<<<<<<< [ProbeUFieldNext] END <<<<<<<<<<");
        return;
    }
    std::string firstFuncName;
    if (!TryReadFName(firstFunc + namePrivateOffset, firstFuncName) || firstFuncName.empty()) {
        PDBG("ProbeUFieldNext: TryReadFName(firstFunc+0x{:X}) 失败, firstFunc=0x{:X}",
             namePrivateOffset, firstFunc);
        PDBG("<<<<<<<<<< [ProbeUFieldNext] END <<<<<<<<<<");
        return;
    }
    PDBG("ProbeUFieldNext: Children 第一个函数 \"{}\" @ 0x{:X}",
         firstFuncName, firstFunc);

    // Search the inferred UField region for Next.
    int32_t nextSearchStart = GetStructSize("UObject");
    int32_t nextSearchEnd = GetStructSize("UField");
    PDBG("ProbeUFieldNext: 搜索范围 [0x{:X}, 0x{:X})", nextSearchStart, nextSearchEnd);

    for (int32_t off = nextSearchStart; off < nextSearchEnd; off += 8) {
        uintptr_t nextPtr = 0;
        if (!KMgrRead(firstFunc + off, &nextPtr, 8)) continue;
        if (!IsValidPtr(nextPtr)) continue;

        std::string nextName;
        if (!TryReadFName(nextPtr + namePrivateOffset, nextName) || nextName.empty()) continue;

        // Count the matching sibling chain.
        int chainLen = 2;
        uintptr_t cur = nextPtr;
        std::string lastFuncName = nextName;
        for (int j = 0; j < 200; ++j) {
            uintptr_t nn = 0;
            if (!KMgrRead(cur + off, &nn, 8) || !IsValidPtr(nn)) break;
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
    if (!m_GameDetected) { PDBG("请先检测游戏后再进行探测操作"); return; }
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

    // Distinguish UObject-derived properties from the FField reflection model.
    DetectReflectionModel();

    uintptr_t obj0 = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(0));
    PDBG("obj[0] = {}", obj0);
    if (!obj0) { PDBG("Phase2 中止: obj[0] 为空"); return; }

    uintptr_t obj1 = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(1));
    PDBG("obj[1] = {}", obj1);
    if (!obj1) { PDBG("Phase2 中止: obj[1] 为空"); return; }

    // Infer UObject size from UField::Next, then probe PropertiesSize.
    PDBG("---------- [Step 1/5] sizeof(UObject) & PropertiesSize ----------");
    int32_t sizeofViaNext = Phase2_ProbeUObjectSize();
    if (sizeofViaNext > 0) {
        Phase2_ProbePropertiesSize(obj1, sizeofViaNext);
        PDBG("Size 探测结果: PropertiesSize confirmed={}, MinAlignment confirmed={}",
             HasConfirmed("UStruct::PropertiesSize"), HasConfirmed("UStruct::MinAlignment"));
    } else {
        PDBG("sizeof(UObject) via UField::Next 失败, Size 探测跳过");
    }

    // Read the inferred UObject size to bound later searches.
    int32_t sizeofUObject = GetStructSize("UObject");
    PDBG("GetStructSize(UObject) = 0x{:X}", sizeofUObject);
    if (sizeofUObject <= 0) {
        PDBG("sizeof(UObject) 返回 0, 可能 UStruct::PropertiesSize 未确认或 UObject 未找到");
    }

    // === Super ===
    PDBG("---------- [Step 2/5] Super ----------");
    uintptr_t packageClass = 0;
    PDBG("读取 obj[0]+0x{:X} (Class 偏移) ...", classPrivateOffset);
    if (!KMgrRead(obj0 + classPrivateOffset, &packageClass, 8) || !IsValidPtr(packageClass)) {
        PDBG("Phase2 中止: KMgrRead(obj0+classPrivateOffset) 失败或 packageClass={} 无效", packageClass);
        return;
    }
    std::string pkgClassName;
    TryReadFName(packageClass + namePrivateOffset, pkgClassName);
    PDBG("obj[0].Class = 0x{:X}, Name=\"{}\"", packageClass, pkgClassName);
    Phase2_ProbeSuperStruct(packageClass);
    PDBG("Super 探测结果: confirmed={}", HasConfirmed("UStruct::SuperStruct"));

    // === Children ===
    PDBG("---------- [Step 3/5] Children ----------");
    uintptr_t obj1Class = 0;
    PDBG("读取 obj[1]+0x{:X} (Class 偏移) ...", classPrivateOffset);
    if (!KMgrRead(obj1 + classPrivateOffset, &obj1Class, 8) || !IsValidPtr(obj1Class)) {
        PDBG("Phase2 中止: KMgrRead(obj1+classPrivateOffset) 失败或 obj1Class={} 无效", obj1Class);
        return;
    }
    std::string obj1ClassName;
    TryReadFName(obj1Class + namePrivateOffset, obj1ClassName);
    if (!FNameEq(obj1ClassName, "Class")) {
        PDBG("obj[1].Class.Name 不是 Class, 实际=\"{}\"", obj1ClassName);
    }
    PDBG("obj[1] = 0x{:X}, Class = 0x{:X}, Class.Name=\"{}\"",
         obj1, obj1Class, obj1ClassName);
    Phase2_ProbeChildren(obj1);
    PDBG("Children 探测结果: confirmed={}", HasConfirmed("UStruct::Children"));

    // Probe ChildProperties only for the FField model.
    PDBG("---------- [Step 4/5] ChildProperties ----------");
    if (m_ReflectionModel == EReflectionModel::UProperty) {
        PDBG("UProperty(<=4.24) 模型: 无 ChildProperties, 跳过探测 (属性走 UStruct::Children)");
    } else if (HasConfirmed("UStruct::Children")) {
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
// Phase 3: UClass fields.
// ============================================================

void UEProber::Phase3_ProbeCastFlags() {
    PDBG(">>>>>>>>>> [ProbeCastFlags] BEGIN >>>>>>>>>>");
    m_Phase3CastFlagsCandidates.clear();
    PDBG("ProbeCastFlags: 开始探测 UClass::CastFlags");

    int32_t namePrivateOffset = GetConfirmedOffset("UObject::NamePrivate");
    int32_t classPrivateOffset = GetConfirmedOffset("UObject::ClassPrivate");
    int32_t superStructOffset = GetConfirmedOffset("UStruct::SuperStruct");
    PDBG("ProbeCastFlags: namePrivateOffset=0x{:X}, classPrivateOffset=0x{:X}, superStructOffset=0x{:X}",
         namePrivateOffset, classPrivateOffset, superStructOffset);
    if (namePrivateOffset < 0 || classPrivateOffset < 0) {
        PDBG("ProbeCastFlags: 前置偏移未确认, 中止");
        PDBG("<<<<<<<<<< [ProbeCastFlags] END <<<<<<<<<<");
        return;
    }

    // Ensure m_ClassClass is available.
    if (!m_ClassClass) {
        // The Object UClass's Class pointer identifies the Class metaclass.
        uintptr_t obj1 = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(1));
        if (!obj1) {
            PDBG("ProbeCastFlags: obj[1] 无效, 中止");
            PDBG("<<<<<<<<<< [ProbeCastFlags] END <<<<<<<<<<");
            return;
        }
        uintptr_t obj1Class = 0;
        KMgrRead(obj1 + classPrivateOffset, &obj1Class, 8);
        if (!IsValidPtr(obj1Class)) {
            PDBG("ProbeCastFlags: obj1->ClassPrivate 无效, 中止");
            PDBG("<<<<<<<<<< [ProbeCastFlags] END <<<<<<<<<<");
            return;
        }
        m_ClassClass = obj1Class;
        PDBG("ProbeCastFlags: m_ClassClass=0x{:X}", m_ClassClass);
    }
    // Ensure m_ClassStruct is available.
    if (!m_ClassStruct && superStructOffset >= 0) {
        uintptr_t structAddr = 0;
        KMgrRead(m_ClassClass + superStructOffset, &structAddr, 8);
        if (IsValidPtr(structAddr)) m_ClassStruct = structAddr;
        PDBG("ProbeCastFlags: m_ClassStruct=0x{:X}", m_ClassStruct);
    }

    // Get the Package UClass from object-array entry zero.
    uintptr_t packageClass = 0;
    {
        uintptr_t obj0 = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(0));
        if (obj0 && IsValidPtr(obj0)) {
            KMgrRead(obj0 + classPrivateOffset, &packageClass, 8);
            if (!IsValidPtr(packageClass)) packageClass = 0;
        }
    }

    // Anchor expectations: Class cast flags 0x29 and Package cast flags 0x0000000400000000.
    // Search between inferred UStruct and UClass sizes for CastFlags.
    int32_t sizeofUStruct = GetStructSize("UStruct");
    int32_t sizeofUClass = GetStructSize("UClass");
    int32_t searchStart = sizeofUStruct;
    int32_t searchEnd = sizeofUClass;
    PDBG("ProbeCastFlags: 搜索范围 [0x{:X}, 0x{:X}), ClassClass=0x{:X}, ClassStruct=0x{:X}, PackageClass=0x{:X}",
         searchStart, searchEnd, m_ClassClass, m_ClassStruct, packageClass);

    for (int32_t off = searchStart; off < searchEnd; off += 8) {
        uint64_t valClass = 0, valStruct = 0, valPackage = 0;
        bool readClass = m_ClassClass && KMgrRead(m_ClassClass + off, &valClass, 8);
        bool readStruct = m_ClassStruct && KMgrRead(m_ClassStruct + off, &valStruct, 8);
        bool readPackage = packageClass && KMgrRead(packageClass + off, &valPackage, 8);

        if (!readClass) continue;
        if (valClass == 0) continue;

        float confidence = 0.0f;
        std::string desc;

        // Require the Class anchor value to equal 0x29.
        if (valClass == ECastFlags::KNOWN_CLASS_FLAGS) {
            confidence += 0.5f;
            desc += std::format("Class=0x{:X} (精确匹配 0x29); ", valClass);
        } else if (valClass & ECastFlags::CASTCLASS_UClass) {
            confidence += 0.2f;
            desc += std::format("Class=0x{:X} (含 CASTCLASS_UClass); ", valClass);
        }

        // Require Struct flags to include CASTCLASS_UStruct and exclude CASTCLASS_UClass.
        if (readStruct && (valStruct & ECastFlags::CASTCLASS_UStruct) && !(valStruct & ECastFlags::CASTCLASS_UClass)) {
            confidence += 0.2f;
            desc += std::format("Struct=0x{:X}; ", valStruct);
        }

        // Require the Package anchor value to equal 0x0000000400000000.
        if (readPackage && valPackage == ECastFlags::KNOWN_PACKAGE_FLAGS) {
            confidence += 0.3f;
            desc += std::format("Package=0x{:X} (精确匹配); ", valPackage);
        } else if (readPackage && valPackage != 0 && valPackage != valClass) {
            confidence += 0.1f;
            desc += std::format("Package=0x{:X}; ", valPackage);
        }

        if (confidence > 0.1f) {
            PDBG("ProbeCastFlags: 偏移 0x{:X} -> Class=0x{:X}, Struct=0x{:X}, Package=0x{:X}, confidence={:.2f}",
                 off, valClass, valStruct, valPackage, confidence);
            m_Phase3CastFlagsCandidates.push_back({
                off, valClass,
                std::format("偏移 0x{:X}: Class=0x{:X}, Struct=0x{:X} ({})",
                    off, valClass, valStruct, desc),
                confidence
            });
        }
    }

    PDBG("ProbeCastFlags: 扫描完毕, 候选数={}", m_Phase3CastFlagsCandidates.size());
    std::sort(m_Phase3CastFlagsCandidates.begin(), m_Phase3CastFlagsCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase3CastFlagsCandidates.empty() && m_Phase3CastFlagsCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase3CastFlagsCandidates[0];
        PDBG("ProbeCastFlags: 最佳候选 偏移=0x{:X}, confidence={:.2f}", best.offset, best.confidence);
        auto& r = GetResult("UClass::CastFlags");
        r.offset = best.offset; r.size = 8; r.typeName = "EClassCastFlags";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeCastFlags: UClass::CastFlags 自动探测: 偏移 0x{:X}{}",
            best.offset, r.confirmed ? " [已自动确认]" : "");
    } else {
        PDBG("ProbeCastFlags: 无满足置信度的候选项");
    }
    PDBG("<<<<<<<<<< [ProbeCastFlags] END <<<<<<<<<<");
}

void UEProber::Phase3_ProbeClassDefaultObject(uintptr_t classAddr) {
    PDBG(">>>>>>>>>> [ProbeClassDefaultObject] BEGIN >>>>>>>>>>");
    m_Phase2ClassDefaultObjCandidates.clear();
    PDBG("ProbeClassDefaultObject: classAddr=0x{:X}", classAddr);

    // Resolve Default__Object first, then compare pointer candidates directly.
    uintptr_t defaultObj = FindObjectInGObjects("Default__Object");
    if (!defaultObj) {
        PDBG("ProbeClassDefaultObject: 未在 GObjects 中找到 Default__Object, 中止");
        PDBG("<<<<<<<<<< [ProbeClassDefaultObject] END <<<<<<<<<<");
        return;
    }
    PDBG("ProbeClassDefaultObject: Default__Object=0x{:X}", defaultObj);

    // Search between inferred UStruct and UClass sizes for DefaultObject.
    int32_t searchStart = GetStructSize("UStruct");
    int32_t searchEnd = GetStructSize("UClass");
    PDBG("ProbeClassDefaultObject: 搜索范围 [0x{:X}, 0x{:X})", searchStart, searchEnd);

    for (int32_t off = searchStart; off < searchEnd; off += 8) {
        uintptr_t ptr = 0;
        if (!KMgrRead(classAddr + off, &ptr, 8)) continue;

        float confidence = 0.0f;
        if (ptr == defaultObj)
            confidence = 1.0f;
        else if (IsValidPtr(ptr))
            confidence = 0.05f;

        if (confidence > 0.04f) {
            PDBG("ProbeClassDefaultObject: 偏移 0x{:X} -> ptr={}, confidence={:.2f}", off, ptr, confidence);
            m_Phase2ClassDefaultObjCandidates.push_back({
                off, ptr,
                std::format("偏移 0x{:X} -> 0x{:X} {}", off, ptr,
                    ptr == defaultObj ? "== Default__Object [精确匹配]" : "(其他指针)"),
                confidence
            });
        }

        if (confidence >= 1.0f) {
            PDBG("ProbeClassDefaultObject: 精确匹配, early break");
            break;
        }
    }

    PDBG("ProbeClassDefaultObject: 扫描完毕, 候选数={}", m_Phase2ClassDefaultObjCandidates.size());
    std::sort(m_Phase2ClassDefaultObjCandidates.begin(), m_Phase2ClassDefaultObjCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase2ClassDefaultObjCandidates.empty() && m_Phase2ClassDefaultObjCandidates[0].confidence >= 0.9f) {
        auto& best = m_Phase2ClassDefaultObjCandidates[0];
        PDBG("ProbeClassDefaultObject: 最佳候选 偏移=0x{:X}, confidence={:.2f}", best.offset, best.confidence);
        auto& r = GetResult("UClass::ClassDefaultObject");
        r.offset = best.offset; r.size = 8; r.typeName = "UObject*";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeClassDefaultObject: UClass::ClassDefaultObject 自动探测: 偏移 0x{:X}{}",
            best.offset, r.confirmed ? " [已自动确认]" : "");
    } else {
        PDBG("ProbeClassDefaultObject: 无满足置信度的候选项");
    }
    PDBG("<<<<<<<<<< [ProbeClassDefaultObject] END <<<<<<<<<<");
}

void UEProber::Phase3_AutoProbe() {
    if (!m_GameDetected) { PDBG("请先检测游戏后再进行探测操作"); return; }
    PDBG(">>>>>>>>>> [Phase3_AutoProbe] BEGIN >>>>>>>>>>");
    m_PhaseStatus[3] = EPhaseStatus::InProgress;
    PDBG("===== 阶段 3: UClass 自动探测 =====");

    Phase3_ProbeCastFlags();

    // === DefaultObject ===
    uintptr_t obj1 = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(1));
    PDBG("Phase3_AutoProbe: obj[1]=0x{:X}", obj1);
    if (obj1 && IsValidPtr(obj1)) {
        Phase3_ProbeClassDefaultObject(obj1);
    } else {
        PDBG("Phase3_AutoProbe: obj[1] 无效, 跳过 ProbeClassDefaultObject");
    }

    PDBG("阶段 3 自动探测完成");
    PDBG("<<<<<<<<<< [Phase3_AutoProbe] END <<<<<<<<<<");
}

// ============================================================
// Phase 4: UFunction fields.
// Anchors: ReceiveBeginPlay, ReceiveTick, IsValid, PrintString and K2_GetActorLocation.
// ============================================================

// Find a named UFunction along Children->Next.
uintptr_t UEProber::WalkChildrenChain(
    uintptr_t classAddr, const std::string& funcName,
    int32_t childrenOff, int32_t nextOff, int32_t namePrivateOff)
{
    uintptr_t cur = 0;
    if (!KMgrRead(classAddr + childrenOff, &cur, 8) || !IsValidPtr(cur))
        return 0;

    for (int i = 0; i < 500 && cur; ++i) {
        std::string n;
        if (TryReadFName(cur + namePrivateOff, n) && n == funcName)
            return cur;
        uintptr_t next = 0;
        if (!KMgrRead(cur + nextOff, &next, 8)) break;
        cur = (next && IsValidPtr(next)) ? next : 0;
    }
    return 0;
}

void UEProber::Phase4_CollectAnchors() {
    PDBG(">>>>>>>>>> [CollectAnchors] BEGIN >>>>>>>>>>");
    int32_t namePrivateOff     = GetConfirmedOffset("UObject::NamePrivate");
    int32_t classPrivateOff    = GetConfirmedOffset("UObject::ClassPrivate");
    int32_t childrenOff = GetConfirmedOffset("UStruct::Children");
    int32_t nextOff     = GetConfirmedOffset("UField::Next");
    PDBG("CollectAnchors: namePrivateOff=0x{:X}, classPrivateOff=0x{:X}, childrenOff=0x{:X}, nextOff=0x{:X}",
         namePrivateOff, classPrivateOff, childrenOff, nextOff);
    if (namePrivateOff < 0 || classPrivateOff < 0 || childrenOff < 0 || nextOff < 0) {
        PDBG("CollectAnchors: 缺少前置偏移 (Name/Class/Children/Next), 中止");
        PDBG("<<<<<<<<<< [CollectAnchors] END <<<<<<<<<<");
        return;
    }

    // Find ReceiveBeginPlay, ReceiveTick and K2_GetActorLocation in Actor children.
    uintptr_t actorClass = FindObjectInGObjects("Actor", "Class");
    if (!actorClass) {
        PDBG("CollectAnchors: 未找到 Actor UClass");
    } else {
        PDBG("CollectAnchors: Actor UClass @ 0x{:X}", actorClass);
        if (!m_FuncReceiveBeginPlay)
            m_FuncReceiveBeginPlay = WalkChildrenChain(actorClass, "ReceiveBeginPlay", childrenOff, nextOff, namePrivateOff);
        if (!m_FuncReceiveTick)
            m_FuncReceiveTick = WalkChildrenChain(actorClass, "ReceiveTick", childrenOff, nextOff, namePrivateOff);
        if (!m_FuncK2_GetActorLocation)
            m_FuncK2_GetActorLocation = WalkChildrenChain(actorClass, "K2_GetActorLocation", childrenOff, nextOff, namePrivateOff);
    }

    // Find IsValid and PrintString in KismetSystemLibrary children.
    uintptr_t kismetClass = FindObjectInGObjects("KismetSystemLibrary", "Class");
    if (!kismetClass) {
        PDBG("CollectAnchors: 未找到 KismetSystemLibrary UClass");
    } else {
        PDBG("CollectAnchors: KismetSystemLibrary UClass @ 0x{:X}", kismetClass);
        if (!m_FuncIsValid)
            m_FuncIsValid = WalkChildrenChain(kismetClass, "IsValid", childrenOff, nextOff, namePrivateOff);
        if (!m_FuncPrintString)
            m_FuncPrintString = WalkChildrenChain(kismetClass, "PrintString", childrenOff, nextOff, namePrivateOff);
    }

    // Log collected anchors.
    auto logAnchor = [&](const char* name, uintptr_t addr) {
        if (addr) PDBG("CollectAnchors: 锚点 \"{}\" @ 0x{:X}", name, addr);
        else      PDBG("CollectAnchors: 锚点 \"{}\" 未找到", name);
    };
    logAnchor("ReceiveBeginPlay",    m_FuncReceiveBeginPlay);
    logAnchor("ReceiveTick",         m_FuncReceiveTick);
    logAnchor("IsValid",             m_FuncIsValid);
    logAnchor("PrintString",         m_FuncPrintString);
    logAnchor("K2_GetActorLocation", m_FuncK2_GetActorLocation);
    PDBG("<<<<<<<<<< [CollectAnchors] END <<<<<<<<<<");
}

void UEProber::Phase4_ProbeFunctionFlags() {
    PDBG(">>>>>>>>>> [ProbeFunctionFlags] BEGIN >>>>>>>>>>");
    m_Phase4FuncFlagsCandidates.clear();
    PDBG("ProbeFunctionFlags: 开始探测 UFunction::FunctionFlags");

    if (!m_FuncIsValid || !m_FuncPrintString || !m_FuncReceiveBeginPlay) {
        PDBG("ProbeFunctionFlags: 缺少 IsValid/PrintString/ReceiveBeginPlay 锚点, 中止");
        PDBG("<<<<<<<<<< [ProbeFunctionFlags] END <<<<<<<<<<");
        return;
    }
    PDBG("ProbeFunctionFlags: IsValid=0x{:X}, PrintString=0x{:X}, ReceiveBeginPlay=0x{:X}",
         m_FuncIsValid, m_FuncPrintString, m_FuncReceiveBeginPlay);

    int32_t sizeofUStruct = GetStructSize("UStruct");
    int32_t sizeofUFunction = GetStructSize("UFunction");
    int32_t searchStart = sizeofUStruct;
    int32_t searchEnd = sizeofUFunction;
    PDBG("ProbeFunctionFlags: 搜索范围 [0x{:X}, 0x{:X})", searchStart, searchEnd);

    if (searchStart <= 0 || searchEnd <= 0 || searchStart >= searchEnd) {
        PDBG("ProbeFunctionFlags: 搜索范围无效: UStruct Size=0x{:X}, UFunction Size=0x{:X}, 中止",
             sizeofUStruct, sizeofUFunction);
        PDBG("<<<<<<<<<< [ProbeFunctionFlags] END <<<<<<<<<<");
        return;
    }

    // Expected flag masks.
    const uint32_t isValidExpected  = EFuncFlags::FUNC_Native | EFuncFlags::FUNC_Final | EFuncFlags::FUNC_BlueprintPure;
    const uint32_t printStrExpected = EFuncFlags::FUNC_Native | EFuncFlags::FUNC_Final | EFuncFlags::FUNC_HasDefaults;
    const uint32_t bpEventExpected  = EFuncFlags::FUNC_Event  | EFuncFlags::FUNC_BlueprintEvent;

    for (int32_t off = searchStart; off < searchEnd; off += 4) {
        uint32_t valIsValid = 0, valPrintStr = 0, valBeginPlay = 0;
        if (!KMgrRead(m_FuncIsValid + off, &valIsValid, 4)) continue;
        if (!KMgrRead(m_FuncPrintString + off, &valPrintStr, 4)) continue;
        if (!KMgrRead(m_FuncReceiveBeginPlay + off, &valBeginPlay, 4)) continue;

        // IsValid requires Native, Final and BlueprintPure.
        if ((valIsValid & isValidExpected) != isValidExpected) continue;
        // PrintString requires Native, Final and HasDefaults but excludes BlueprintPure.
        if ((valPrintStr & printStrExpected) != printStrExpected) continue;
        if (valPrintStr & EFuncFlags::FUNC_BlueprintPure) continue;
        // ReceiveBeginPlay requires Event and BlueprintEvent but excludes Native.
        if ((valBeginPlay & bpEventExpected) != bpEventExpected) continue;
        if (valBeginPlay & EFuncFlags::FUNC_Native) continue;

        float confidence = 0.8f;
        std::string desc = std::format("IsValid=0x{:X}, PrintString=0x{:X}, BeginPlay=0x{:X}", valIsValid, valPrintStr, valBeginPlay);

        // Cross-check ReceiveTick against the same event flag pattern.
        if (m_FuncReceiveTick) {
            uint32_t valTick = 0;
            if (KMgrRead(m_FuncReceiveTick + off, &valTick, 4) &&
                (valTick & bpEventExpected) == bpEventExpected &&
                !(valTick & EFuncFlags::FUNC_Native)) {
                confidence += 0.1f;
                desc += std::format(", Tick=0x{:X}", valTick);
            } else {
                PDBG("ProbeFunctionFlags: off=0x{:X} ReceiveTick 交叉验证未通过 (val=0x{:X})", off, valTick);
            }
        }
        // Cross-check K2_GetActorLocation for Native, Final and BlueprintPure.
        if (m_FuncK2_GetActorLocation) {
            uint32_t valLoc = 0;
            if (KMgrRead(m_FuncK2_GetActorLocation + off, &valLoc, 4) &&
                (valLoc & isValidExpected) == isValidExpected) {
                confidence += 0.1f;
                desc += std::format(", GetLoc=0x{:X}", valLoc);
            } else {
                PDBG("ProbeFunctionFlags: off=0x{:X} K2_GetActorLocation 交叉验证未通过 (val=0x{:X})", off, valLoc);
            }
        }

        PDBG("ProbeFunctionFlags: off=0x{:X} val=0x{:X}, confidence={:.2f}", off, valIsValid, confidence);
        m_Phase4FuncFlagsCandidates.push_back({off, valIsValid, desc, confidence});
        if (confidence >= 1.0f) {
            PDBG("ProbeFunctionFlags: 精确匹配/置信度 100%, early break");
            break;
        }
    }

    PDBG("ProbeFunctionFlags: 扫描完毕, 候选数={}", m_Phase4FuncFlagsCandidates.size());
    std::sort(m_Phase4FuncFlagsCandidates.begin(), m_Phase4FuncFlagsCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase4FuncFlagsCandidates.empty() && m_Phase4FuncFlagsCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase4FuncFlagsCandidates[0];
        auto& r = GetResult("UFunction::FunctionFlags");
        r.offset = best.offset; r.size = 4; r.typeName = "EFunctionFlags";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeFunctionFlags: 最佳候选 偏移=0x{:X}, confidence={:.2f}", best.offset, best.confidence);
        PDBG("ProbeFunctionFlags: UFunction::FunctionFlags 自动探测: 偏移 0x{:X}{}", best.offset, r.confirmed ? " [已自动确认]" : "");
    } else {
        PDBG("ProbeFunctionFlags: 无满足置信度的候选项");
    }
    PDBG("<<<<<<<<<< [ProbeFunctionFlags] END <<<<<<<<<<");
}

void UEProber::Phase4_ProbeNumParmsAndParmsSize() {
    PDBG(">>>>>>>>>> [ProbeNumParmsAndParmsSize] BEGIN >>>>>>>>>>");
    m_Phase4NumParmsCandidates.clear();
    m_Phase4ParmsSizeCandidates.clear();
    PDBG("ProbeNumParmsAndParmsSize: 开始探测 UFunction::NumParms 和 ParmsSize");

    // Require at least three anchors with different NumParms values.
    if (!m_FuncReceiveBeginPlay || !m_FuncIsValid) {
        PDBG("ProbeNumParmsAndParmsSize: 缺少 ReceiveBeginPlay/IsValid 锚点, 中止");
        PDBG("<<<<<<<<<< [ProbeNumParmsAndParmsSize] END <<<<<<<<<<");
        return;
    }
    PDBG("ProbeNumParmsAndParmsSize: ReceiveBeginPlay=0x{:X}, IsValid=0x{:X}",
         m_FuncReceiveBeginPlay, m_FuncIsValid);

    int32_t funcFlagsOff = GetConfirmedOffset("UFunction::FunctionFlags");
    if (funcFlagsOff < 0) {
        PDBG("ProbeNumParmsAndParmsSize: 需要先确认 FunctionFlags, 中止");
        PDBG("<<<<<<<<<< [ProbeNumParmsAndParmsSize] END <<<<<<<<<<");
        return;
    }

    int32_t searchStart = GetStructSize("UStruct");
    int32_t searchEnd = GetStructSize("UFunction");
    PDBG("ProbeNumParmsAndParmsSize: 搜索范围 [0x{:X}, 0x{:X}), funcFlagsOff=0x{:X}",
         searchStart, searchEnd, funcFlagsOff);
    if (searchStart <= 0 || searchEnd <= 0 || searchStart >= searchEnd) {
        PDBG("ProbeNumParmsAndParmsSize: 搜索范围无效: UStruct=0x{:X}, UFunction=0x{:X}, 中止",
             searchStart, searchEnd);
        PDBG("<<<<<<<<<< [ProbeNumParmsAndParmsSize] END <<<<<<<<<<");
        return;
    }

    // === NumParms (uint8) ===
    // PrintString=6, IsValid=2, ReceiveTick=1, ReceiveBeginPlay=0
    for (int32_t off = searchStart; off < searchEnd; off += 1) {
        if (off >= funcFlagsOff && off < funcFlagsOff + 4) continue; // Skip FunctionFlags itself.
        uint8_t valBP = 0, valIsValid = 0;
        if (!KMgrRead(m_FuncReceiveBeginPlay + off, &valBP, 1)) continue;
        if (!KMgrRead(m_FuncIsValid + off, &valIsValid, 1)) continue;

        if (valBP != 0 || valIsValid != 2) continue;

        float confidence = 0.6f;
        std::string desc = std::format("BeginPlay=0, IsValid=2");

        if (m_FuncReceiveTick) {
            uint8_t valTick = 0;
            if (KMgrRead(m_FuncReceiveTick + off, &valTick, 1) && valTick == 1) {
                confidence += 0.15f;
                desc += ", Tick=1";
            } else {
                PDBG("ProbeNumParmsAndParmsSize: [NumParms] off=0x{:X} ReceiveTick 验证失败 (val={}), 跳过", off, (uint32_t)valTick);
                continue;
            }
        }
        if (m_FuncPrintString) {
            uint8_t valPS = 0;
            if (KMgrRead(m_FuncPrintString + off, &valPS, 1) && valPS == 6) {
                confidence += 0.25f;
                desc += ", PrintString=6";
            } else {
                PDBG("ProbeNumParmsAndParmsSize: [NumParms] off=0x{:X} PrintString 验证失败 (val={}), 跳过", off, (uint32_t)valPS);
                continue;
            }
        }

        PDBG("ProbeNumParmsAndParmsSize: [NumParms] off=0x{:X} val=0x{:X}, confidence={:.2f}", off, (uint32_t)valIsValid, confidence);
        m_Phase4NumParmsCandidates.push_back({off, valIsValid, desc, confidence});
        if (confidence >= 1.0f) {
            PDBG("ProbeNumParmsAndParmsSize: [NumParms] 精确匹配/置信度 100%, early break");
            break;
        }
    }

    // === ParmsSize (uint16) ===
    // IsValid=9, ReceiveTick=4, ReceiveBeginPlay=0
    for (int32_t off = searchStart; off < searchEnd; off += 2) {
        if (off >= funcFlagsOff && off < funcFlagsOff + 4) continue; // Skip FunctionFlags itself.
        uint16_t valBP = 0, valIsValid = 0;
        if (!KMgrRead(m_FuncReceiveBeginPlay + off, &valBP, 2)) continue;
        if (!KMgrRead(m_FuncIsValid + off, &valIsValid, 2)) continue;

        if (valBP != 0 || valIsValid != 9) continue;

        float confidence = 0.7f;
        std::string desc = std::format("BeginPlay=0, IsValid=9");

        if (m_FuncReceiveTick) {
            uint16_t valTick = 0;
            if (KMgrRead(m_FuncReceiveTick + off, &valTick, 2) && valTick == 4) {
                confidence += 0.15f;
                desc += ", Tick=4";
            } else {
                PDBG("ProbeNumParmsAndParmsSize: [ParmsSize] off=0x{:X} ReceiveTick 交叉验证未通过 (val={})", off, (uint32_t)valTick);
            }
        }
        if (m_FuncPrintString) {
            uint16_t valPS = 0;
            if (KMgrRead(m_FuncPrintString + off, &valPS, 2) && valPS > 20) {
                confidence += 0.15f;
                desc += std::format(", PrintString={}", valPS);
            }
        }

        PDBG("ProbeNumParmsAndParmsSize: [ParmsSize] off=0x{:X} val=0x{:X}, confidence={:.2f}", off, (uint32_t)valIsValid, confidence);
        m_Phase4ParmsSizeCandidates.push_back({off, valIsValid, desc, confidence});
        if (confidence >= 1.0f) {
            PDBG("ProbeNumParmsAndParmsSize: [ParmsSize] 精确匹配/置信度 100%, early break");
            break;
        }
    }

    PDBG("ProbeNumParmsAndParmsSize: NumParms 扫描完毕, 候选数={}", m_Phase4NumParmsCandidates.size());
    PDBG("ProbeNumParmsAndParmsSize: ParmsSize 扫描完毕, 候选数={}", m_Phase4ParmsSizeCandidates.size());
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
        PDBG("ProbeNumParmsAndParmsSize: 最佳候选 [NumParms] 偏移=0x{:X}, confidence={:.2f}", best.offset, best.confidence);
        PDBG("ProbeNumParmsAndParmsSize: UFunction::NumParms 自动探测: 偏移 0x{:X}{}", best.offset, r.confirmed ? " [已自动确认]" : "");
    } else {
        PDBG("ProbeNumParmsAndParmsSize: [NumParms] 无满足置信度的候选项");
    }
    if (!m_Phase4ParmsSizeCandidates.empty() && m_Phase4ParmsSizeCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase4ParmsSizeCandidates[0];
        auto& r = GetResult("UFunction::ParmsSize");
        r.offset = best.offset; r.size = 2; r.typeName = "uint16";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeNumParmsAndParmsSize: 最佳候选 [ParmsSize] 偏移=0x{:X}, confidence={:.2f}", best.offset, best.confidence);
        PDBG("ProbeNumParmsAndParmsSize: UFunction::ParmsSize 自动探测: 偏移 0x{:X}{}", best.offset, r.confirmed ? " [已自动确认]" : "");
    } else {
        PDBG("ProbeNumParmsAndParmsSize: [ParmsSize] 无满足置信度的候选项");
    }
    PDBG("<<<<<<<<<< [ProbeNumParmsAndParmsSize] END <<<<<<<<<<");
}

void UEProber::Phase4_ProbeReturnValueOffset() {
    PDBG(">>>>>>>>>> [ProbeReturnValueOffset] BEGIN >>>>>>>>>>");
    m_Phase4ReturnValueOffCandidates.clear();
    PDBG("ProbeReturnValueOffset: 开始探测 UFunction::ReturnValueOffset");

    // Expected return offsets: K2_GetActorLocation=0, IsValid=8, PrintString=0xFFFF.
    if (!m_FuncK2_GetActorLocation || !m_FuncIsValid || !m_FuncPrintString) {
        PDBG("ProbeReturnValueOffset: 缺少 K2_GetActorLocation/IsValid/PrintString 锚点, 中止");
        PDBG("<<<<<<<<<< [ProbeReturnValueOffset] END <<<<<<<<<<");
        return;
    }
    PDBG("ProbeReturnValueOffset: K2_GetActorLocation=0x{:X}, IsValid=0x{:X}, PrintString=0x{:X}",
         m_FuncK2_GetActorLocation, m_FuncIsValid, m_FuncPrintString);

    int32_t funcFlagsOff = GetConfirmedOffset("UFunction::FunctionFlags");
    if (funcFlagsOff < 0) {
        PDBG("ProbeReturnValueOffset: 需要先确认 FunctionFlags, 中止");
        PDBG("<<<<<<<<<< [ProbeReturnValueOffset] END <<<<<<<<<<");
        return;
    }
    int32_t numParmsOff = GetConfirmedOffset("UFunction::NumParms");
    int32_t parmsSizeOff = GetConfirmedOffset("UFunction::ParmsSize");
    if (numParmsOff < 0)
        PDBG("ProbeReturnValueOffset: NumParms 未确认, off=-1, 该排除过滤将不生效");
    if (parmsSizeOff < 0)
        PDBG("ProbeReturnValueOffset: ParmsSize 未确认, off=-1, 该排除过滤将不生效");

    int32_t searchStart = GetStructSize("UStruct");
    int32_t searchEnd = GetStructSize("UFunction");
    PDBG("ProbeReturnValueOffset: 搜索范围 [0x{:X}, 0x{:X}), funcFlagsOff=0x{:X}, numParmsOff=0x{:X}, parmsSizeOff=0x{:X}",
         searchStart, searchEnd, funcFlagsOff, numParmsOff, parmsSizeOff);
    if (searchStart <= 0 || searchEnd <= 0 || searchStart >= searchEnd) {
        PDBG("ProbeReturnValueOffset: 搜索范围无效: UStruct=0x{:X}, UFunction=0x{:X}, 中止",
             searchStart, searchEnd);
        PDBG("<<<<<<<<<< [ProbeReturnValueOffset] END <<<<<<<<<<");
        return;
    }

    for (int32_t off = searchStart; off < searchEnd; off += 2) {
        if (off == numParmsOff || off == parmsSizeOff) continue;
        if (off >= funcFlagsOff && off < funcFlagsOff + 4) continue;

        uint16_t valLoc = 0, valIsValid = 0, valPS = 0;
        if (!KMgrRead(m_FuncK2_GetActorLocation + off, &valLoc, 2)) continue;
        if (!KMgrRead(m_FuncIsValid + off, &valIsValid, 2)) continue;
        if (!KMgrRead(m_FuncPrintString + off, &valPS, 2)) continue;

        // K2_GetActorLocation=0, IsValid=8, PrintString=0xFFFF
        if (valLoc != 0 || valIsValid != 8 || valPS != 0xFFFF) continue;

        float confidence = 0.9f;
        std::string desc = std::format("GetLoc=0, IsValid=8, PrintString=0xFFFF");

        // Cross-check ReceiveBeginPlay and ReceiveTick against 0xFFFF.
        if (m_FuncReceiveBeginPlay) {
            uint16_t valBP = 0;
            if (KMgrRead(m_FuncReceiveBeginPlay + off, &valBP, 2) && valBP == 0xFFFF) {
                confidence += 0.05f;
                desc += ", BeginPlay=0xFFFF";
            } else {
                PDBG("ProbeReturnValueOffset: off=0x{:X} ReceiveBeginPlay 交叉验证未通过 (val=0x{:X})", off, (uint32_t)valBP);
            }
        }
        if (m_FuncReceiveTick) {
            uint16_t valTick = 0;
            if (KMgrRead(m_FuncReceiveTick + off, &valTick, 2) && valTick == 0xFFFF) {
                confidence += 0.05f;
                desc += ", Tick=0xFFFF";
            } else {
                PDBG("ProbeReturnValueOffset: off=0x{:X} ReceiveTick 交叉验证未通过 (val=0x{:X})", off, (uint32_t)valTick);
            }
        }

        PDBG("ProbeReturnValueOffset: off=0x{:X} val=0x{:X}, confidence={:.2f}", off, (uint32_t)valLoc, confidence);
        m_Phase4ReturnValueOffCandidates.push_back({off, valLoc, desc, confidence});
        if (confidence >= 1.0f) {
            PDBG("ProbeReturnValueOffset: 精确匹配/置信度 100%, early break");
            break;
        }
    }

    PDBG("ProbeReturnValueOffset: 扫描完毕, 候选数={}", m_Phase4ReturnValueOffCandidates.size());
    std::sort(m_Phase4ReturnValueOffCandidates.begin(), m_Phase4ReturnValueOffCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase4ReturnValueOffCandidates.empty() && m_Phase4ReturnValueOffCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase4ReturnValueOffCandidates[0];
        auto& r = GetResult("UFunction::ReturnValueOffset");
        r.offset = best.offset; r.size = 2; r.typeName = "uint16";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeReturnValueOffset: 最佳候选 偏移=0x{:X}, confidence={:.2f}", best.offset, best.confidence);
        PDBG("ProbeReturnValueOffset: UFunction::ReturnValueOffset 自动探测: 偏移 0x{:X}{}", best.offset, r.confirmed ? " [已自动确认]" : "");
    } else {
        PDBG("ProbeReturnValueOffset: 无满足置信度的候选项");
    }
    PDBG("<<<<<<<<<< [ProbeReturnValueOffset] END <<<<<<<<<<");
}

void UEProber::Phase4_ProbeFunc() {
    PDBG(">>>>>>>>>> [ProbeFunc] BEGIN >>>>>>>>>>");
    m_Phase4ExecFuncCandidates.clear();
    PDBG("ProbeFunc: 开始探测 UFunction::Func");

    if (!m_FuncIsValid || !m_FuncPrintString || !m_FuncReceiveBeginPlay) {
        PDBG("ProbeFunc: 缺少 IsValid/PrintString/ReceiveBeginPlay 锚点, 中止");
        PDBG("<<<<<<<<<< [ProbeFunc] END <<<<<<<<<<");
        return;
    }
    PDBG("ProbeFunc: IsValid=0x{:X}, PrintString=0x{:X}, ReceiveBeginPlay=0x{:X}",
         m_FuncIsValid, m_FuncPrintString, m_FuncReceiveBeginPlay);

    uintptr_t textStart = GetTextSegStart();
    uintptr_t textEnd = GetTextSegEnd();
    PDBG("ProbeFunc: .text [0x{:X}, 0x{:X})", textStart, textEnd);
    if (textStart == 0 || textEnd == 0 || textEnd <= textStart) {
        PDBG("ProbeFunc: .text 段范围无效 (start=0x{:X}, end=0x{:X}), 中止", textStart, textEnd);
        PDBG("<<<<<<<<<< [ProbeFunc] END <<<<<<<<<<");
        return;
    }

    int32_t sizeofUStruct = GetStructSize("UStruct");
    int32_t sizeofUFunction = GetStructSize("UFunction");
    PDBG("ProbeFunc: 搜索范围 [0x{:X}, 0x{:X})", sizeofUStruct, sizeofUFunction);
    if (sizeofUStruct <= 0 || sizeofUFunction <= 0 || sizeofUStruct >= sizeofUFunction) {
        PDBG("ProbeFunc: UStruct/UFunction size 无效: UStruct=0x{:X}, UFunction=0x{:X}, 中止",
             sizeofUStruct, sizeofUFunction);
        PDBG("<<<<<<<<<< [ProbeFunc] END <<<<<<<<<<");
        return;
    }

    // Native anchors: IsValid, PrintString and K2_GetActorLocation.
    std::vector<uintptr_t> nativeFuncs = {m_FuncIsValid, m_FuncPrintString};
    if (m_FuncK2_GetActorLocation) nativeFuncs.push_back(m_FuncK2_GetActorLocation);

    // Blueprint anchors: ReceiveBeginPlay and ReceiveTick.
    std::vector<uintptr_t> bpFuncs = {m_FuncReceiveBeginPlay};
    if (m_FuncReceiveTick) bpFuncs.push_back(m_FuncReceiveTick);

    for (int32_t off = sizeofUStruct; off < sizeofUFunction; off += 8) {
        // Require distinct native pointers within the current range heuristic; that range is not a verified executable segment.
        int nativeInText = 0;
        std::set<uintptr_t> nativeValues;
        for (auto fnAddr : nativeFuncs) {
            uintptr_t ptr = 0;
            if (KMgrRead(fnAddr + off, &ptr, 8) && ptr >= textStart && ptr < textEnd) {
                nativeInText++;
                nativeValues.insert(ptr);
            }
        }
        bool nativeAllDiff = (nativeValues.size() == nativeFuncs.size());

        // Require matching Blueprint function pointers.
        std::set<uintptr_t> bpValues;
        for (auto fnAddr : bpFuncs) {
            uintptr_t ptr = 0;
            if (KMgrRead(fnAddr + off, &ptr, 8) && ptr != 0)
                bpValues.insert(ptr);
        }
        bool bpAllSame = (bpValues.size() == 1 && bpFuncs.size() >= 2);

        if (nativeInText < 2) continue;

        float confidence = 0.0f;
        if (nativeInText >= 3 && nativeAllDiff) confidence += 0.5f;
        else if (nativeInText >= 2 && nativeAllDiff) confidence += 0.4f;
        else if (nativeInText >= 2) confidence += 0.3f;

        if (bpAllSame) confidence += 0.5f;
        else if (bpValues.size() == 1) confidence += 0.3f; // Only one Blueprint sample is available.

        if (confidence < 0.5f) continue;

        PDBG("ProbeFunc: off=0x{:X} nativeInText={}, nativeAllDiff={}, bpAllSame={}, confidence={:.2f}",
             off, nativeInText, nativeAllDiff, bpAllSame, confidence);
        m_Phase4ExecFuncCandidates.push_back({
            off, 0,
            std::format("Native在.text={}/{} 互不同={}, BP值相同={}({}个)",
                nativeInText, (int)nativeFuncs.size(), nativeAllDiff, bpAllSame, (int)bpFuncs.size()),
            confidence
        });
        if (confidence >= 1.0f) {
            PDBG("ProbeFunc: 精确匹配/置信度 100%, early break");
            break;
        }
    }

    PDBG("ProbeFunc: 扫描完毕, 候选数={}", m_Phase4ExecFuncCandidates.size());
    std::sort(m_Phase4ExecFuncCandidates.begin(), m_Phase4ExecFuncCandidates.end(),
        [](const auto& a, const auto& b) { return a.confidence > b.confidence; });

    if (!m_Phase4ExecFuncCandidates.empty() && m_Phase4ExecFuncCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase4ExecFuncCandidates[0];
        auto& r = GetResult("UFunction::Func");
        r.offset = best.offset; r.size = 8; r.typeName = "FNativeFuncPtr";
        r.evidence = best.description; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbeFunc: 最佳候选 偏移=0x{:X}, confidence={:.2f}", best.offset, best.confidence);
        PDBG("ProbeFunc: UFunction::Func 自动探测: 偏移 0x{:X}{}", best.offset, r.confirmed ? " [已自动确认]" : "");
    } else {
        PDBG("ProbeFunc: 无满足置信度的候选项");
    }
    PDBG("<<<<<<<<<< [ProbeFunc] END <<<<<<<<<<");
}

void UEProber::Phase4_AutoProbe() {
    if (!m_GameDetected) { PDBG("请先检测游戏后再进行探测操作"); return; }
    PDBG(">>>>>>>>>> [Phase4_AutoProbe] BEGIN >>>>>>>>>>");
    m_PhaseStatus[4] = EPhaseStatus::InProgress;
    PDBG("===== 阶段 4: UFunction 自动探测 =====");

    if (!HasConfirmed("UObject::NamePrivate") || !HasConfirmed("UObject::ClassPrivate") ||
        !HasConfirmed("UStruct::Children") || !HasConfirmed("UField::Next")) {
        PDBG("Phase4_AutoProbe: 需要先完成阶段 1~2 (确认 Name, Class, Children, Next 偏移), 中止");
        PDBG("<<<<<<<<<< [Phase4_AutoProbe] END <<<<<<<<<<");
        return;
    }

    // Collect the five function anchors.
    PDBG("---------- [Step 1/3] CollectAnchors ----------");
    Phase4_CollectAnchors();

    int anchorCount = 0;
    if (m_FuncReceiveBeginPlay)    anchorCount++;
    if (m_FuncReceiveTick)         anchorCount++;
    if (m_FuncIsValid)             anchorCount++;
    if (m_FuncPrintString)         anchorCount++;
    if (m_FuncK2_GetActorLocation) anchorCount++;
    PDBG("Phase4_AutoProbe: 收集到 {} 个锚点", anchorCount);

    if (anchorCount < 3) {
        PDBG("Phase4_AutoProbe: 只找到 {} 个锚点, 至少需要 3 个, 中止", anchorCount);
        PDBG("<<<<<<<<<< [Phase4_AutoProbe] END <<<<<<<<<<");
        return;
    }

    // Probe in dependency order.
    PDBG("---------- [Step 2/3] FunctionFlags ----------");
    Phase4_ProbeFunctionFlags();
    PDBG("FunctionFlags 探测结果: confirmed={}", HasConfirmed("UFunction::FunctionFlags"));

    if (HasConfirmed("UFunction::FunctionFlags")) {
        PDBG("---------- [Step 3/3] NumParms/ParmsSize/ReturnValueOffset/Func ----------");
        Phase4_ProbeNumParmsAndParmsSize();
        PDBG("NumParms 探测结果: confirmed={}", HasConfirmed("UFunction::NumParms"));
        PDBG("ParmsSize 探测结果: confirmed={}", HasConfirmed("UFunction::ParmsSize"));
        Phase4_ProbeReturnValueOffset();
        PDBG("ReturnValueOffset 探测结果: confirmed={}", HasConfirmed("UFunction::ReturnValueOffset"));
        Phase4_ProbeFunc();
        PDBG("Func 探测结果: confirmed={}", HasConfirmed("UFunction::Func"));
    }

    PDBG("阶段 4 自动探测完成");
    PDBG("<<<<<<<<<< [Phase4_AutoProbe] END <<<<<<<<<<");
}

// ============================================================
// Phase 5: FField and FProperty.
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
    PDBG("CollectAnchors: FindObjectInGObjects(ExecuteUbergraph) = {}", execUbergraph);
    if (execUbergraph) {
        KMgrRead(execUbergraph + childPropsOff, &m_FFEntryPoint, 8);
        if (!IsValidPtr(m_FFEntryPoint)) m_FFEntryPoint = 0;
    }
    PDBG("CollectAnchors: m_FFEntryPoint={}", m_FFEntryPoint);

    // ReceiveTick -> "DeltaSeconds" (FloatProperty)
    PDBG("CollectAnchors: m_FuncReceiveTick={}", m_FuncReceiveTick);
    if (m_FuncReceiveTick) {
        KMgrRead(m_FuncReceiveTick + childPropsOff, &m_FFDeltaSeconds, 8);
        if (!IsValidPtr(m_FFDeltaSeconds)) m_FFDeltaSeconds = 0;
    }
    PDBG("CollectAnchors: m_FFDeltaSeconds={}", m_FFDeltaSeconds);

    // IsValid first parameter -> Next -> ReturnValue.
    PDBG("CollectAnchors: m_FuncIsValid={}", m_FuncIsValid);
    if (m_FuncIsValid) {
        KMgrRead(m_FuncIsValid + childPropsOff, &m_FFIsValidParam0, 8);
        if (!IsValidPtr(m_FFIsValidParam0)) m_FFIsValidParam0 = 0;
        // Resolve IsValid ReturnValue after probing Next.
    }
    PDBG("CollectAnchors: m_FFIsValidParam0={}", m_FFIsValidParam0);

    // K2_GetActorLocation -> "ReturnValue" (StructProperty)
    PDBG("CollectAnchors: m_FuncK2_GetActorLocation={}", m_FuncK2_GetActorLocation);
    if (m_FuncK2_GetActorLocation) {
        KMgrRead(m_FuncK2_GetActorLocation + childPropsOff, &m_FFK2LocReturn, 8);
        if (!IsValidPtr(m_FFK2LocReturn)) m_FFK2LocReturn = 0;
    }
    PDBG("CollectAnchors: m_FFK2LocReturn={}", m_FFK2LocReturn);

    // Expect ReceiveBeginPlay ChildProperties to be null for its zero-parameter anchor.
    PDBG("CollectAnchors: m_FuncReceiveBeginPlay={}", m_FuncReceiveBeginPlay);
    if (m_FuncReceiveBeginPlay) {
        uintptr_t bpChildProps = 0;
        KMgrRead(m_FuncReceiveBeginPlay + childPropsOff, &bpChildProps, 8);
        if (bpChildProps != 0) {
            PDBG("CollectAnchors: WARNING ReceiveBeginPlay ChildProperties 不为 nullptr: 0x{:X}, 可能 ChildProperties 偏移有误",
                bpChildProps);
        } else {
            PDBG("CollectAnchors: ReceiveBeginPlay ChildProperties = nullptr (预验证通过)");
        }
    }

    PDBG("CollectAnchors: 汇总: EntryPoint=0x{:X}, DeltaSeconds=0x{:X}, IsValidP0=0x{:X}, K2LocRV=0x{:X}",
        m_FFEntryPoint, m_FFDeltaSeconds,
        m_FFIsValidParam0, m_FFK2LocReturn);
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
    PDBG("ProbeFFieldNamePrivate: EntryPoint=0x{:X}, DeltaSeconds=0x{:X}",
         m_FFEntryPoint, m_FFDeltaSeconds);

    for (int32_t off = 0x08; off < 0x40; off += 4) {
        std::string name1;
        if (!TryReadFName(m_FFEntryPoint + off, name1) || !FNameEq(name1, "EntryPoint")) {
            PDBG("ProbeFFieldNamePrivate: off=0x{:X} name1=\"{}\" 不匹配", off, name1);
            continue;
        }
        PDBG("ProbeFFieldNamePrivate: off=0x{:X} 命中 EntryPoint! name1=\"{}\"", off, name1);

        float confidence = 0.7f;
        std::string desc = std::format("EntryPoint Name=\"{}\"", name1);

        // Cross-check DeltaSeconds.
        if (m_FFDeltaSeconds) {
            std::string name2;
            if (TryReadFName(m_FFDeltaSeconds + off, name2) && FNameEq(name2, "DeltaSeconds")) {
                confidence += 0.3f;
                desc += ", DeltaSeconds OK";
                PDBG("ProbeFFieldNamePrivate: off=0x{:X} DeltaSeconds 交叉验证通过, conf={:.2f}", off, confidence);
            } else {
                PDBG("ProbeFFieldNamePrivate: off=0x{:X} DeltaSeconds 交叉验证失败 name2=\"{}\", 跳过", off, name2);
                continue; // Require both anchors to match.
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
    PDBG("ProbeFFieldOwner: EntryPoint=0x{:X}, execUbergraph=0x{:X}",
         m_FFEntryPoint, execUbergraph);

    int32_t namePrivateOff = GetConfirmedOffset("FField::NamePrivate");
    PDBG("ProbeFFieldOwner: namePrivateOff=0x{:X}", namePrivateOff);

    for (int32_t off = 0x00; off < 0x30; off += 8) {
        if (off == namePrivateOff) continue; // Exclude the confirmed Name offset.

        uintptr_t ptr1 = 0;
        if (!KMgrRead(m_FFEntryPoint + off, &ptr1, 8)) continue;
        if (ptr1 != execUbergraph) {
            PDBG("ProbeFFieldOwner: off=0x{:X} ptr1={} != execUbergraph, 跳过", off, ptr1);
            continue;
        }
        PDBG("ProbeFFieldOwner: off=0x{:X} 命中! ptr1 == execUbergraph", off);

        float confidence = 0.7f;
        std::string desc = std::format("EntryPoint Owner -> ExecuteUbergraph");

        // Cross-check that DeltaSeconds owner identifies ReceiveTick.
        if (m_FFDeltaSeconds && m_FuncReceiveTick) {
            uintptr_t ptr2 = 0;
            if (KMgrRead(m_FFDeltaSeconds + off, &ptr2, 8) && ptr2 == m_FuncReceiveTick) {
                confidence += 0.2f;
                desc += ", DeltaSeconds->ReceiveTick OK";
            }
        }

        // Check the assumed bIsUObject flag after the owner pointer.
        uint64_t bIsUObj = 0;
        if (KMgrRead(m_FFEntryPoint + off + 8, &bIsUObj, 8) && (bIsUObj & 1)) {
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
        PDBG("ProbeFFieldNext: 缺少锚点 EntryPoint=0x{:X} IsValidP0=0x{:X}, 中止",
             m_FFEntryPoint, m_FFIsValidParam0);
        PDBG("<<<<<<<<<< [ProbeFFieldNext] END <<<<<<<<<<");
        return;
    }
    PDBG("ProbeFFieldNext: namePrivateOff=0x{:X}, ownerOff=0x{:X}, EntryPoint=0x{:X}, IsValidP0=0x{:X}",
         namePrivateOff, ownerOff, m_FFEntryPoint, m_FFIsValidParam0);

    for (int32_t off = 0x08; off < 0x30; off += 8) {
        if (off == namePrivateOff || off == ownerOff || (ownerOff >= 0 && off == ownerOff + 8)) continue;

        // Expect EntryPoint Next to be null for the one-parameter anchor.
        uintptr_t ptr1 = 0;
        KMgrRead(m_FFEntryPoint + off, &ptr1, 8);
        if (ptr1 != 0) {
            PDBG("ProbeFFieldNext: off=0x{:X} EntryPoint.Next=0x{:X} != nullptr, 跳过", off, ptr1);
            continue;
        }

        // Expect IsValid's first parameter Next to identify ReturnValue.
        uintptr_t ptr2 = 0;
        if (!KMgrRead(m_FFIsValidParam0 + off, &ptr2, 8) || !IsValidPtr(ptr2)) {
            PDBG("ProbeFFieldNext: off=0x{:X} IsValidP0.Next=0x{:X} 无效, 跳过", off, ptr2);
            continue;
        }

        std::string nextName;
        if (!TryReadFName(ptr2 + namePrivateOff, nextName) || !FNameEq(nextName, "ReturnValue")) {
            PDBG("ProbeFFieldNext: off=0x{:X} Next->Name=\"{}\" 不是ReturnValue, 跳过", off, nextName);
            continue;
        }
        PDBG("ProbeFFieldNext: off=0x{:X} 命中! EntryPoint.Next=nullptr, IsValidP0.Next->\"ReturnValue\"", off);

        // Expect exactly two fields in the IsValid anchor chain.
        uintptr_t nextNext = 0;
        KMgrRead(ptr2 + off, &nextNext, 8);
        bool chainLen2 = (nextNext == 0); // Expect ReturnValue Next to be null.

        float confidence = 0.8f;
        std::string desc = std::format("EntryPoint.Next=nullptr, IsValid.Next->\"ReturnValue\"");

        if (chainLen2) {
            confidence += 0.2f;
            desc += ", 链长=2";
            // Save the IsValid ReturnValue address.
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
        if (ownerOff >= 0 && off == ownerOff + 8) continue; // Skip the assumed bIsUObject location.

        // Expect EntryPoint ClassPrivate to identify FFieldClass with IntProperty at its name offset zero.
        uintptr_t ptr1 = 0;
        if (!KMgrRead(m_FFEntryPoint + off, &ptr1, 8) || !IsValidPtr(ptr1)) continue;

        std::string typeName1;
        if (!TryReadFName(ptr1, typeName1) || !FNameEq(typeName1, "IntProperty")) {
            PDBG("ProbeFFieldClassPrivate: off=0x{:X} ptr1=0x{:X} typeName=\"{}\" 不是IntProperty",
                 off, ptr1, typeName1);
            continue;
        }
        PDBG("ProbeFFieldClassPrivate: off=0x{:X} 命中! ptr1=0x{:X} -> \"IntProperty\"", off, ptr1);

        float confidence = 0.7f;
        std::string desc = std::format("EntryPoint ClassPrivate -> \"IntProperty\"");

        // Cross-check DeltaSeconds against FloatProperty.
        if (m_FFDeltaSeconds) {
            uintptr_t ptr2 = 0;
            if (KMgrRead(m_FFDeltaSeconds + off, &ptr2, 8) && IsValidPtr(ptr2)) {
                std::string typeName2;
                if (TryReadFName(ptr2, typeName2) && FNameEq(typeName2, "FloatProperty")) {
                    confidence += 0.2f;
                    desc += ", DeltaSeconds -> \"FloatProperty\"";
                }
            }
        }

        // Cross-check IsValid ReturnValue against BoolProperty.
        if (m_FFIsValidReturn) {
            uintptr_t ptr3 = 0;
            if (KMgrRead(m_FFIsValidReturn + off, &ptr3, 8) && IsValidPtr(ptr3)) {
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

    // Accepted EObjectFlags bit mask.
    constexpr uint32_t kValidFlagsMask = 0x3FFFFFFF;

    // Search int32 candidates at four-byte alignment in FField offsets 0x08–0x40.
    // Exclude the vtable pointer and confirmed FField member ranges.
    for (int32_t off = 0x08; off < 0x40; off += 4) {
        if (namePrivateOff >= 0 && off >= namePrivateOff && off < namePrivateOff + 8) continue;
        if (ownerOff >= 0 && off >= ownerOff && off < ownerOff + 16) continue;
        if (nextOff >= 0 && off >= nextOff && off < nextOff + 8) continue;
        if (classPrivateOff >= 0 && off >= classPrivateOff && off < classPrivateOff + 8) continue;

        int32_t val1 = -1;
        if (!KMgrRead(m_FFEntryPoint + off, &val1, 4)) continue;

        // Flags may be nonzero but must remain within the accepted mask and magnitude.
        uint32_t uval1 = (uint32_t)val1;
        if ((uval1 & ~kValidFlagsMask) != 0 || uval1 > 0xFFFF) continue;

        float confidence = 0.4f;
        std::string desc = std::format("EntryPoint=0x{:X}", uval1);

        // Cross-check equal ObjFlags across the function-parameter anchors.
        if (m_FFDeltaSeconds) {
            int32_t val2 = -1;
            KMgrRead(m_FFDeltaSeconds + off, &val2, 4);
            if (val2 == val1) {
                confidence += 0.3f;
                desc += std::format(", DeltaSeconds=0x{:X}(一致)", (uint32_t)val2);
            }
        }

        if (m_FFIsValidParam0) {
            int32_t val3 = -1;
            KMgrRead(m_FFIsValidParam0 + off, &val3, 4);
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

    // Require distinct element-size anchors: EntryPoint=4 and IsValid first parameter=8.
    if (!m_FFEntryPoint || !m_FFIsValidParam0) {
        PDBG("ProbeAD+ES: 缺少锚点 EntryPoint=0x{:X} IsValidP0=0x{:X}, 中止",
             m_FFEntryPoint, m_FFIsValidParam0);
        PDBG("<<<<<<<<<< [ProbeArrayDim+ElemSize] END <<<<<<<<<<");
        return;
    }

    // Start after ObjFlags when confirmed, otherwise after the inferred Name field.
    int32_t objFlagsOff = GetConfirmedOffset("FField::FlagsPrivate");
    int32_t searchStart = (objFlagsOff >= 0) ? (objFlagsOff + 4) : (namePrivateOff + 0x08);
    int32_t searchEnd = searchStart + 0x50;
    PDBG("ProbeAD+ES: namePrivateOff=0x{:X}, objFlagsOff=0x{:X}, 搜索范围 [0x{:X}, 0x{:X})",
         namePrivateOff, objFlagsOff, searchStart, searchEnd);

    for (int32_t off = searchStart; off < searchEnd; off += 4) {
        // Expect ArrayDim to equal one in all anchors.
        int32_t ad1 = 0, ad2 = 0;
        bool r1 = KMgrRead(m_FFEntryPoint + off, &ad1, 4);
        bool r2 = KMgrRead(m_FFIsValidParam0 + off, &ad2, 4);
        if (!r1 || ad1 != 1 || !r2 || ad2 != 1) {
            PDBG("ProbeAD+ES: off=0x{:X} AD不匹配: r1={} ad1={} r2={} ad2={}", off, r1, ad1, r2, ad2);
            continue;
        }

        // Assume ElementSize follows ArrayDim at +4.
        int32_t es1 = 0, es2 = 0;
        if (!KMgrRead(m_FFEntryPoint + off + 4, &es1, 4)) continue;
        if (!KMgrRead(m_FFIsValidParam0 + off + 4, &es2, 4)) continue;

        // Expected element sizes: IntProperty=4 and ObjectProperty=8.
        if (es1 != 4 || es2 != 8) {
            PDBG("ProbeAD+ES: off=0x{:X} AD匹配但 ES不匹配: es1={} es2={}", off, es1, es2);
            continue;
        }
        PDBG("ProbeAD+ES: off=0x{:X} 命中! AD1={} AD2={} ES1={} ES2={}", off, ad1, ad2, es1, es2);

        float confidence = 0.7f;
        std::string desc = std::format("EntryPoint: AD=1,ES=4; IsValid: AD=1,ES=8");

        // Cross-check DeltaSeconds element size against 4.
        if (m_FFDeltaSeconds) {
            int32_t ad3 = 0, es3 = 0;
            if (KMgrRead(m_FFDeltaSeconds + off, &ad3, 4) && ad3 == 1 &&
                KMgrRead(m_FFDeltaSeconds + off + 4, &es3, 4) && es3 == 4) {
                confidence += 0.1f;
                desc += "; DeltaSec: AD=1,ES=4";
            }
        }

        // Cross-check IsValid ReturnValue element size against 1.
        if (m_FFIsValidReturn) {
            int32_t ad4 = 0, es4 = 0;
            if (KMgrRead(m_FFIsValidReturn + off, &ad4, 4) && ad4 == 1 &&
                KMgrRead(m_FFIsValidReturn + off + 4, &es4, 4) && es4 == 1) {
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
        PDBG("ProbePropFlags: 缺少锚点 EntryPoint=0x{:X} IsValidP0=0x{:X}, 中止",
             m_FFEntryPoint, m_FFIsValidParam0);
        PDBG("<<<<<<<<<< [ProbeFPropertyFlags] END <<<<<<<<<<");
        return;
    }

    // Search at eight-byte alignment after ElementSize.
    int32_t searchStart = (elemSizeOff + 4 + 7) & ~7; // Align to eight bytes.
    int32_t searchEnd = searchStart + 0x30;
    PDBG("ProbePropFlags: elemSizeOff=0x{:X}, 搜索范围 [0x{:X}, 0x{:X})",
         elemSizeOff, searchStart, searchEnd);

    for (int32_t off = searchStart; off < searchEnd; off += 8) {
        uint64_t flags1 = 0, flags2 = 0;
        if (!KMgrRead(m_FFEntryPoint + off, &flags1, 8)) continue;
        if (!KMgrRead(m_FFIsValidParam0 + off, &flags2, 8)) continue;

        // Require CPF_Parm in every anchor.
        if (!(flags1 & ECPFFlags::CPF_Parm) || !(flags2 & ECPFFlags::CPF_Parm)) {
            PDBG("ProbePropFlags: off=0x{:X} flags1=0x{:X} flags2=0x{:X} 缺少CPF_Parm", off, flags1, flags2);
            continue;
        }
        // Exclude CPF_ReturnParm for EntryPoint.
        if (flags1 & ECPFFlags::CPF_ReturnParm) {
            PDBG("ProbePropFlags: off=0x{:X} EntryPoint含CPF_ReturnParm, 跳过", off);
            continue;
        }
        PDBG("ProbePropFlags: off=0x{:X} 命中! flags1=0x{:X} flags2=0x{:X}", off, flags1, flags2);

        float confidence = 0.6f;
        std::string desc = std::format("EntryPoint=0x{:X}, IsValid首参=0x{:X}", flags1, flags2);

        // Require CPF_ReturnParm and CPF_OutParm for IsValid ReturnValue.
        if (m_FFIsValidReturn) {
            uint64_t flags3 = 0;
            if (KMgrRead(m_FFIsValidReturn + off, &flags3, 8) &&
                (flags3 & ECPFFlags::CPF_ReturnParm) && (flags3 & ECPFFlags::CPF_OutParm)) {
                confidence += 0.2f;
                desc += std::format(", IsValidRV=0x{:X}(RetParm+OutParm)", flags3);
            }
        }

        // Require CPF_ReturnParm for K2_GetActorLocation ReturnValue.
        if (m_FFK2LocReturn) {
            uint64_t flags4 = 0;
            if (KMgrRead(m_FFK2LocReturn + off, &flags4, 8) &&
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
        PDBG("ProbeOffInt: 缺少锚点 EntryPoint=0x{:X} IsValidP0=0x{:X}, 中止",
             m_FFEntryPoint, m_FFIsValidParam0);
        PDBG("<<<<<<<<<< [ProbeOffset_Internal] END <<<<<<<<<<");
        return;
    }

    // Search at four-byte alignment after PropertyFlags.
    int32_t searchStart = propFlagsOff + 8;
    int32_t searchEnd = searchStart + 0x20;
    PDBG("ProbeOffInt: propFlagsOff=0x{:X}, 搜索范围 [0x{:X}, 0x{:X})",
         propFlagsOff, searchStart, searchEnd);

    for (int32_t off = searchStart; off < searchEnd; off += 4) {
        int32_t val1 = 0, val2 = 0;
        if (!KMgrRead(m_FFEntryPoint + off, &val1, 4)) continue;
        if (!KMgrRead(m_FFIsValidParam0 + off, &val2, 4)) continue;

        // Expect both first-parameter offsets to equal zero.
        if (val1 != 0 || val2 != 0) {
            PDBG("ProbeOffInt: off=0x{:X} val1={} val2={} 不为0, 跳过", off, val1, val2);
            continue;
        }
        PDBG("ProbeOffInt: off=0x{:X} 命中! EntryPoint=0, IsValidP0=0", off);

        float confidence = 0.6f;
        std::string desc = "EntryPoint=0, IsValid首参=0";

        // Expect IsValid ReturnValue at offset 8 after its ObjectProperty parameter.
        if (m_FFIsValidReturn) {
            int32_t val3 = 0;
            if (KMgrRead(m_FFIsValidReturn + off, &val3, 4) && val3 == 8) {
                confidence += 0.3f;
                desc += ", IsValidRV=8";
            }
        }

        // Expect DeltaSeconds at offset zero.
        if (m_FFDeltaSeconds) {
            int32_t val4 = 0;
            if (KMgrRead(m_FFDeltaSeconds + off, &val4, 4) && val4 == 0) {
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
        PDBG("ProbePropSize: 缺少锚点 K2LocRV=0x{:X} IsValidP0=0x{:X}, 中止",
             m_FFK2LocReturn, m_FFIsValidParam0);
        PDBG("<<<<<<<<<< [ProbeFPropertySize] END <<<<<<<<<<");
        return;
    }

    // Resolve Vector ScriptStruct and Object UClass as validation anchors.
    uintptr_t vectorStruct = FindObjectInGObjects("Vector", "ScriptStruct");
    uintptr_t objectClass = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(1));
    PDBG("ProbePropSize: offsetInternalOff=0x{:X}, vectorStruct=0x{:X}, objectClass=0x{:X}",
         offsetInternalOff, vectorStruct, objectClass);

    if (!vectorStruct && !objectClass) {
        PDBG("ProbePropSize: 无法找到 Vector ScriptStruct 或 Object UClass, 中止");
        PDBG("<<<<<<<<<< [ProbeFPropertySize] END <<<<<<<<<<");
        return;
    }

    // Search aligned pointers after Offset_Internal within the configured bound.
    int32_t searchStart = (offsetInternalOff + 4 + 7) & ~7;
    int32_t searchEnd = searchStart + 0x40;
    PDBG("ProbePropSize: 搜索范围 [0x{:X}, 0x{:X})", searchStart, searchEnd);

    for (int32_t off = searchStart; off < searchEnd; off += 8) {
        float confidence = 0.0f;
        std::string desc;

        // K2_GetActorLocation ReturnValue (FStructProperty): Struct -> "Vector"
        if (m_FFK2LocReturn && vectorStruct) {
            uintptr_t ptr1 = 0;
            if (!KMgrRead(m_FFK2LocReturn + off, &ptr1, 8) || ptr1 != vectorStruct) {
                PDBG("ProbePropSize: off=0x{:X} K2Loc ptr1=0x{:X} != vectorStruct, 跳过", off, ptr1);
                continue;
            }
            confidence += 0.5f;
            desc += "K2Loc->Vector";
        }

        // Expect FObjectPropertyBase::PropertyClass to identify Object.
        // Cross-check independent subclass pointers at the candidate offset.
        if (m_FFIsValidParam0 && objectClass) {
            uintptr_t ptr2 = 0;
            if (!KMgrRead(m_FFIsValidParam0 + off, &ptr2, 8)) continue;
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
    // (FEnumProperty layout probe inserted after this block — see Phase5_ProbeFEnumPropertyLayout below)
    if (!m_Phase5FPropSizeCandidates.empty() && m_Phase5FPropSizeCandidates[0].confidence >= 0.7f) {
        auto& best = m_Phase5FPropSizeCandidates[0];
        // SubPropertyBase = first-known-pointer offset captured BEFORE any leading-metadata
        // correction below. On standard UE layouts this equals sizeof(FProperty); on games
        // with leading-metadata padding (DeltaForce 1.201+) it's larger by the pad size and
        // is the correct anchor for FObjectPropertyBase / FStructProperty / FArrayProperty
        // / etc. derived-class tail data emitted by the dumper synthesize step.
        int32_t subPropBaseOff = best.offset;
        int32_t finalOff = best.offset;
        std::string finalDesc = best.description;

        // Use IsValid ReturnValue to constrain the FBoolProperty tail location.
        //
        // A known subclass pointer marks the base size only when
        // that pointer is the subclass's first field after the base.
        // Custom layouts can place metadata before the pointer, making its offset
        // larger than sizeof(FProperty).
        //
        // Cross-check with the native-bool IsValid ReturnValue anchor.
        // The heuristic expects the native-bool field bytes
        //   FieldSize=1, ByteOffset=0, ByteMask=1, FieldMask=0xFF
        // 01 00 01 FF; custom layouts require independent validation.
        //
        // Search from best.offset - 16 to best.offset + 4 for that pattern.
        // The pattern begins at the inferred FieldSize byte.
        // Infer a base boundary by rounding down to eight-byte alignment.
        // Prefer the match nearest best.offset when multiple candidates exist.
        if (m_FFIsValidReturn && best.offset >= 16) {
            constexpr int32_t kBack    = 16;
            constexpr int32_t kForward = 4;
            constexpr int32_t kSpan    = kBack + kForward + 4;  // Four-byte pattern length.
            uint8_t buf[kSpan] = {};
            int32_t base = best.offset - kBack;
            if (KMgrRead(m_FFIsValidReturn + base, buf, kSpan)) {
                // Find 01 00 01 FF and select the nearest match.
                int32_t bestPatternOff = -1;
                int32_t bestDist = INT32_MAX;
                for (int32_t i = 0; i + 4 <= kSpan; ++i) {
                    if (buf[i] == 0x01 && buf[i+1] == 0x00 &&
                        buf[i+2] == 0x01 && buf[i+3] == 0xFF) {
                        int32_t patternOff = base + i;
                        int32_t dist = std::abs(patternOff - best.offset);
                        if (dist < bestDist) {
                            bestDist = dist;
                            bestPatternOff = patternOff;
                        }
                    }
                }
                if (bestPatternOff >= 0) {
                    int32_t alignedSize = bestPatternOff & ~0x7;  // Round down to eight-byte alignment.
                    if (alignedSize != best.offset) {
                        PDBG("ProbePropSize: 修正! IsValidRV 命中 [01 00 01 FF] @ +0x{:X} (FieldSize 位置), sizeof = 0x{:X} (向下 8 对齐)",
                             bestPatternOff, alignedSize);
                        finalOff = alignedSize;
                        finalDesc += std::format(", FBool native bool pattern @ +0x{:X} → sizeof corrected to 0x{:X}",
                                                  bestPatternOff, alignedSize);
                        best.offset = alignedSize;
                        best.rawValue = (uint64_t)alignedSize;
                        best.description = finalDesc;
                    } else {
                        PDBG("ProbePropSize: IsValidRV 模式 @ +0x{:X} 与候选一致, 无需修正", bestPatternOff);
                    }
                } else {
                    PDBG("ProbePropSize: IsValidRV 在 [+0x{:X}..+0x{:X}) 未找到 [01 00 01 FF] 模式, 不修正",
                         base, base + kSpan);
                }
            } else {
                PDBG("ProbePropSize: 读 IsValidRV+0x{:X} {} 字节失败, 跳过修正", base, kSpan);
            }
        } else {
            PDBG("ProbePropSize: 缺 IsValidRV 锚点或候选 <16, 跳过 leading-metadata 修正");
        }

        auto& r = GetResult("sizeof(FProperty)");
        r.offset = finalOff; r.size = 0; r.typeName = "size";
        r.evidence = finalDesc; r.autoDetected = true;
        if (best.confidence >= 1.0f) r.confirmed = true;
        PDBG("ProbePropSize: 选定 offset=0x{:X}, conf={:.2f}, confirmed={}",
             finalOff, best.confidence, r.confirmed);

        // SubPropertyBase — same scan, captured pre-correction. Promoted to confirmed
        // on the same confidence threshold as sizeof(FProperty); the two share a
        // signal source (best.confidence) so confirming them together is correct.
        auto& rSub = GetResult("FProperty::SubPropertyBase");
        rSub.offset = subPropBaseOff; rSub.size = 0; rSub.typeName = "offset";
        rSub.evidence = std::format("first-known-pointer offset in FStructProperty / FObjectPropertyBase tail (pre-correction)");
        rSub.autoDetected = true;
        if (best.confidence >= 1.0f) rSub.confirmed = true;
        PDBG("ProbePropSize: SubPropertyBase=0x{:X} (sizeof=0x{:X}, gap={})",
             subPropBaseOff, finalOff, subPropBaseOff - finalOff);
    } else {
        PDBG("ProbePropSize: 无满足阈值的候选 (最佳 conf={:.2f})",
             m_Phase5FPropSizeCandidates.empty() ? 0.0f : m_Phase5FPropSizeCandidates[0].confidence);
    }
    PDBG("<<<<<<<<<< [ProbeFPropertySize] END <<<<<<<<<<");
}

void UEProber::Phase5_ProbeFEnumPropertyLayout() {
    PDBG(">>>>>>>>>> [ProbeFEnumPropertyLayout] BEGIN >>>>>>>>>>");

    int32_t fpropSize    = GetConfirmedOffset("sizeof(FProperty)");
    int32_t subBaseOff   = GetConfirmedOffset("FProperty::SubPropertyBase");
    int32_t classOff     = GetConfirmedOffset("UObject::ClassPrivate");
    int32_t namePrivOff  = GetConfirmedOffset("UObject::NamePrivate");
    int32_t childPropOff = GetConfirmedOffset("UStruct::ChildProperties");
    int32_t ffClassOff   = GetConfirmedOffset("FField::ClassPrivate");
    int32_t ffNextOff    = GetConfirmedOffset("FField::Next");
    if (fpropSize < 0 || classOff < 0 || namePrivOff < 0 || childPropOff < 0 ||
        ffClassOff < 0 || ffNextOff < 0) {
        PDBG("ProbeFEnumPropertyLayout: 依赖偏移未确认 (Size/Class/Name/ChildProps/FF.Class/FF.Next), 中止");
        PDBG("<<<<<<<<<< [ProbeFEnumPropertyLayout] END <<<<<<<<<<");
        return;
    }
    // FEnumProperty tail lives at SubPropertyBase when known (DFM padded layout),
    // else at FProperty.Size (standard layout).
    int32_t base = (subBaseOff > 0) ? subBaseOff : fpropSize;
    PDBG("ProbeFEnumPropertyLayout: base=0x{:X} (sizeof=0x{:X}, subBase=0x{:X})",
         base, fpropSize, subBaseOff);

    // ---------- Anchor: walk GObjects for any FEnumProperty instance ----------
    if (!m_FFEnumProp) {
        int32_t count = BridgeGetObjectNum();
        PDBG("ProbeFEnumPropertyLayout: 扫 GObjects 找 FEnumProperty 锚点 (Num={})", count);
        int32_t scanned = 0, eligibleStructs = 0, walkedFields = 0;
        for (int32_t i = 1; i < count && !m_FFEnumProp; ++i) {
            uintptr_t obj = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(i));
            if (!IsValidPtr(obj)) continue;
            ++scanned;
            uintptr_t cls = 0;
            if (!KMgrRead(obj + classOff, &cls, 8) || !IsValidPtr(cls)) continue;
            std::string clsName;
            if (!TryReadFName(cls + namePrivOff, clsName)) continue;
            // Only walk reflection-bearing classes (have ChildProperties)
            if (!FNameEq(clsName, "Class") && !FNameEq(clsName, "ScriptStruct") &&
                !FNameEq(clsName, "BlueprintGeneratedClass"))
                continue;
            ++eligibleStructs;

            uintptr_t prop = 0;
            if (!KMgrRead(obj + childPropOff, &prop, 8) || !IsValidPtr(prop)) continue;
            for (int32_t hop = 0; hop < 64 && IsValidPtr(prop); ++hop) {
                ++walkedFields;
                uintptr_t fcls = 0;
                if (!KMgrRead(prop + ffClassOff, &fcls, 8) || !IsValidPtr(fcls)) break;
                std::string fclsName;
                if (TryReadFName(fcls, fclsName) && FNameEq(fclsName, "EnumProperty")) {
                    m_FFEnumProp = prop;
                    PDBG("ProbeFEnumPropertyLayout: 锚点命中 obj[{}]=0x{:X} prop=0x{:X} (scanned={} eligible={} walked={})",
                         i, obj, prop, scanned, eligibleStructs, walkedFields);
                    break;
                }
                uintptr_t next = 0;
                if (!KMgrRead(prop + ffNextOff, &next, 8)) break;
                prop = next;
            }
        }
        if (!m_FFEnumProp) {
            PDBG("ProbeFEnumPropertyLayout: 未找到 FEnumProperty 锚点 (scanned={} eligible={} walked={}), cascade fallback 生效",
                 scanned, eligibleStructs, walkedFields);
        }
    }
    if (!m_FFEnumProp) {
        PDBG("<<<<<<<<<< [ProbeFEnumPropertyLayout] END <<<<<<<<<<");
        return;
    }

    // ---------- Identify which slot holds UnderlyingType (backing FProperty) vs Enum (UEnum) ----------
    auto isBackingProp = [&](uintptr_t ptr) -> bool {
        if (!IsValidPtr(ptr)) return false;
        uintptr_t fcls = 0;
        if (!KMgrRead(ptr + ffClassOff, &fcls, 8) || !IsValidPtr(fcls)) return false;
        std::string fclsName;
        if (!TryReadFName(fcls, fclsName)) return false;
        // UE enum backing types: ByteProperty (default), or sized int variants.
        return FNameEq(fclsName, "ByteProperty")    || FNameEq(fclsName, "IntProperty")    ||
               FNameEq(fclsName, "Int8Property")    || FNameEq(fclsName, "Int16Property")  ||
               FNameEq(fclsName, "Int64Property")   || FNameEq(fclsName, "UInt16Property") ||
               FNameEq(fclsName, "UInt32Property")  || FNameEq(fclsName, "UInt64Property");
    };
    auto isUEnum = [&](uintptr_t ptr) -> bool {
        if (!IsValidPtr(ptr)) return false;
        uintptr_t cls = 0;
        if (!KMgrRead(ptr + classOff, &cls, 8) || !IsValidPtr(cls)) return false;
        std::string clsName;
        if (!TryReadFName(cls + namePrivOff, clsName)) return false;
        return FNameEq(clsName, "Enum");
    };

    uintptr_t p0 = 0, p8 = 0;
    KMgrRead(m_FFEnumProp + base + 0, &p0, 8);
    KMgrRead(m_FFEnumProp + base + 8, &p8, 8);
    // Anchor on whichever slot is a confirmed UEnum — UnderlyingProp's other slot
    // may be a valid backing FProperty *or* nullptr (UE sets it null for the
    // default uint8 backing case to save memory). Both arrangements are legal.
    auto isUnderlyingOrNull = [&](uintptr_t ptr) -> bool {
        return ptr == 0 || isBackingProp(ptr);
    };
    bool layoutStd = isUEnum(p8) && isUnderlyingOrNull(p0); // UnderlyingType @ +0, Enum @ +8
    bool layoutAlt = isUEnum(p0) && isUnderlyingOrNull(p8); // Enum @ +0, UnderlyingType @ +8

    int32_t underlyingOff = -1, enumOff = -1;
    std::string desc;
    if (layoutStd && !layoutAlt) {
        underlyingOff = base + 0; enumOff = base + 8;
        desc = std::format("standard: backing FProperty @ +0x{:X}, UEnum @ +0x{:X}", base, base + 8);
    } else if (layoutAlt && !layoutStd) {
        enumOff = base + 0; underlyingOff = base + 8;
        desc = std::format("alternate: UEnum @ +0x{:X}, backing FProperty @ +0x{:X}", base, base + 8);
    } else {
        PDBG("ProbeFEnumPropertyLayout: 无法确定 layout (std={}, alt={}, p0=0x{:X}, p8=0x{:X}), 跳过",
             layoutStd, layoutAlt, p0, p8);
        PDBG("<<<<<<<<<< [ProbeFEnumPropertyLayout] END <<<<<<<<<<");
        return;
    }

    {
        auto& rU = GetResult("FEnumProperty::UnderlyingType");
        rU.offset = underlyingOff; rU.size = 8; rU.typeName = "FProperty*";
        rU.evidence = desc; rU.autoDetected = true; rU.confirmed = true;
    }
    {
        auto& rE = GetResult("FEnumProperty::Enum");
        rE.offset = enumOff; rE.size = 8; rE.typeName = "UEnum*";
        rE.evidence = desc; rE.autoDetected = true; rE.confirmed = true;
    }
    PDBG("ProbeFEnumPropertyLayout: 选定 UnderlyingType=0x{:X}, Enum=0x{:X} ({})",
         underlyingOff, enumOff, desc);
    PDBG("<<<<<<<<<< [ProbeFEnumPropertyLayout] END <<<<<<<<<<");
}

void UEProber::Phase5_ProbeFContainerPropertyTails() {
    PDBG(">>>>>>>>>> [ProbeFContainerPropertyTails] BEGIN >>>>>>>>>>");

    int32_t fpropSize    = GetConfirmedOffset("sizeof(FProperty)");
    int32_t subBaseOff   = GetConfirmedOffset("FProperty::SubPropertyBase");
    int32_t classOff     = GetConfirmedOffset("UObject::ClassPrivate");
    int32_t namePrivOff  = GetConfirmedOffset("UObject::NamePrivate");
    int32_t childPropOff = GetConfirmedOffset("UStruct::ChildProperties");
    int32_t ffClassOff   = GetConfirmedOffset("FField::ClassPrivate");
    int32_t ffNextOff    = GetConfirmedOffset("FField::Next");
    if (fpropSize < 0 || classOff < 0 || namePrivOff < 0 || childPropOff < 0 ||
        ffClassOff < 0 || ffNextOff < 0) {
        PDBG("ProbeContainerTails: 依赖偏移未确认 (Size/Class/Name/ChildProps/FF.Class/FF.Next), 中止");
        PDBG("<<<<<<<<<< [ProbeFContainerPropertyTails] END <<<<<<<<<<");
        return;
    }

    // Candidate tail-slot offsets, priority order: probed SubPropertyBase first
    // (works for non-DFM-alt subclasses), then FProperty.Size and nearby slots
    // for derived classes that have their own per-class leading-metadata pad.
    std::vector<int32_t> candidates;
    if (subBaseOff > 0) candidates.push_back(subBaseOff);
    for (int32_t step = 0; step <= 0x18; step += 8) {
        int32_t cand = fpropSize + step;
        if (std::find(candidates.begin(), candidates.end(), cand) == candidates.end())
            candidates.push_back(cand);
    }
    PDBG("ProbeContainerTails: 候选偏移列表 [{}]: ",
         (int)candidates.size());
    for (int32_t c : candidates) PDBG("    +0x{:X}", c);

    // Known FProperty subclass FFieldClass names. Validates that the pointer
    // read out of a tail slot is actually an inner FProperty (vs random bytes).
    static const char* kKnownPropertyClassNames[] = {
        "Property",
        "ArrayProperty", "SetProperty", "MapProperty",
        "ObjectProperty", "ObjectPtrProperty", "ClassProperty",
        "StructProperty", "ByteProperty", "BoolProperty",
        "FloatProperty", "DoubleProperty",
        "IntProperty", "Int8Property", "Int16Property", "Int64Property",
        "UInt16Property", "UInt32Property", "UInt64Property", "Int32Property",
        "NameProperty", "StrProperty", "TextProperty",
        "DelegateProperty", "MulticastDelegateProperty",
        "MulticastInlineDelegateProperty", "MulticastSparseDelegateProperty",
        "InterfaceProperty", "WeakObjectProperty", "LazyObjectProperty",
        "SoftObjectProperty", "SoftClassProperty",
        "FieldPathProperty", "EnumProperty",
    };
    auto isKnownPropClass = [&](const std::string& s) {
        for (const char* n : kKnownPropertyClassNames) {
            if (FNameEq(s, n)) return true;
        }
        return false;
    };
    auto isInnerFProperty = [&](uintptr_t ptr) -> bool {
        if (!IsValidPtr(ptr)) return false;
        uintptr_t fcls = 0;
        if (!KMgrRead(ptr + ffClassOff, &fcls, 8) || !IsValidPtr(fcls)) return false;
        std::string fclsName;
        if (!TryReadFName(fcls, fclsName)) return false;
        return isKnownPropClass(fclsName);
    };

    // Anchor finder: walk GObjects for first FProperty whose FFieldClass.Name
    // matches `targetName`. Only walks reflection-bearing UClass / UScriptStruct
    // since those have ChildProperties chains (FField linked list).
    auto findAnchor = [&](const char* targetName) -> uintptr_t {
        int32_t count = BridgeGetObjectNum();
        int32_t scanned = 0, eligibleStructs = 0, walkedFields = 0;
        for (int32_t i = 1; i < count; ++i) {
            uintptr_t obj = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(i));
            if (!IsValidPtr(obj)) continue;
            ++scanned;
            uintptr_t cls = 0;
            if (!KMgrRead(obj + classOff, &cls, 8) || !IsValidPtr(cls)) continue;
            std::string clsName;
            if (!TryReadFName(cls + namePrivOff, clsName)) continue;
            if (!FNameEq(clsName, "Class") && !FNameEq(clsName, "ScriptStruct") &&
                !FNameEq(clsName, "BlueprintGeneratedClass"))
                continue;
            ++eligibleStructs;

            uintptr_t prop = 0;
            if (!KMgrRead(obj + childPropOff, &prop, 8) || !IsValidPtr(prop)) continue;
            for (int32_t hop = 0; hop < 64 && IsValidPtr(prop); ++hop) {
                ++walkedFields;
                uintptr_t fcls = 0;
                if (!KMgrRead(prop + ffClassOff, &fcls, 8) || !IsValidPtr(fcls)) break;
                std::string fclsName;
                if (TryReadFName(fcls, fclsName) && FNameEq(fclsName, targetName)) {
                    PDBG("ProbeContainerTails: {} 锚点命中 obj[{}]=0x{:X} prop=0x{:X} (scanned={} eligible={} walked={})",
                         targetName, i, obj, prop, scanned, eligibleStructs, walkedFields);
                    return prop;
                }
                uintptr_t next = 0;
                if (!KMgrRead(prop + ffNextOff, &next, 8)) break;
                prop = next;
            }
        }
        PDBG("ProbeContainerTails: {} 锚点未找到 (scanned={} eligible={} walked={})",
             targetName, scanned, eligibleStructs, walkedFields);
        return 0;
    };

    // Probe one container subclass: find first candidate offset whose slot reads
    // as a valid inner FProperty. Returns the offset, or -1 if none.
    auto probeContainerTail = [&](const char* targetName) -> int32_t {
        uintptr_t anchor = findAnchor(targetName);
        if (!anchor) return -1;
        for (int32_t off : candidates) {
            uintptr_t inner = 0;
            if (!KMgrRead(anchor + off, &inner, 8)) continue;
            if (isInnerFProperty(inner)) {
                PDBG("ProbeContainerTails: {} tail @ +0x{:X}, inner=0x{:X}", targetName, off, inner);
                return off;
            }
            PDBG("ProbeContainerTails: {} +0x{:X} -> 0x{:X} 非有效 inner FProperty", targetName, off, inner);
        }
        return -1;
    };

    // FArrayProperty.Inner
    {
        int32_t off = probeContainerTail("ArrayProperty");
        if (off > 0) {
            auto& r = GetResult("FArrayProperty::Inner");
            r.offset = off; r.size = 8; r.typeName = "FProperty*";
            r.evidence = std::format("FArrayProperty.Inner @ +0x{:X} via GObjects anchor + tail probe", off);
            r.autoDetected = true; r.confirmed = true;
        }
    }

    // FSetProperty.ElementProp
    {
        int32_t off = probeContainerTail("SetProperty");
        if (off > 0) {
            auto& r = GetResult("FSetProperty::ElementProp");
            r.offset = off; r.size = 8; r.typeName = "FProperty*";
            r.evidence = std::format("FSetProperty.ElementProp @ +0x{:X} via GObjects anchor + tail probe", off);
            r.autoDetected = true; r.confirmed = true;
        }
    }

    // FMapProperty.KeyProp + ValueProp (two consecutive slots; KeyProp at probed
    // offset, ValueProp at +sizeof(void*)). UE puts both inner properties side
    // by side; no game has been observed splitting them.
    {
        int32_t off = probeContainerTail("MapProperty");
        if (off > 0) {
            {
                auto& r = GetResult("FMapProperty::KeyProp");
                r.offset = off; r.size = 8; r.typeName = "FProperty*";
                r.evidence = std::format("FMapProperty.KeyProp @ +0x{:X} via GObjects anchor + tail probe", off);
                r.autoDetected = true; r.confirmed = true;
            }
            {
                auto& r = GetResult("FMapProperty::ValueProp");
                r.offset = off + (int32_t)sizeof(void *); r.size = 8; r.typeName = "FProperty*";
                r.evidence = std::format("FMapProperty.ValueProp @ +0x{:X} (KeyProp + 8)", off + (int32_t)sizeof(void *));
                r.autoDetected = true; r.confirmed = true;
            }
        }
    }

    PDBG("<<<<<<<<<< [ProbeFContainerPropertyTails] END <<<<<<<<<<");
}

void UEProber::Phase5_AutoProbe() {
    if (!m_GameDetected) { PDBG("请先检测游戏后再进行探测操作"); return; }
    m_PhaseStatus[5] = EPhaseStatus::InProgress;
    PDBG("========== [Phase5_AutoProbe] BEGIN ==========");

    if (m_ReflectionModel == EReflectionModel::UProperty) {
        PDBG("Phase5: UProperty(<=4.24) 模型无 FField/FProperty, 跳过整个 FField 探测阶段");
        m_PhaseStatus[5] = EPhaseStatus::Completed;
        PDBG("========== [Phase5_AutoProbe] END ==========");
        return;
    }

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

                    // ---------- [Step 10.5] FEnumProperty layout ----------
                    PDBG("---------- [Step 10.5] FEnumProperty layout ----------");
                    Phase5_ProbeFEnumPropertyLayout();
                    PDBG("Phase5: FEnumProperty::UnderlyingType => has={}, confirmed={}",
                         HasResult("FEnumProperty::UnderlyingType"), HasConfirmed("FEnumProperty::UnderlyingType"));
                    PDBG("Phase5: FEnumProperty::Enum => has={}, confirmed={}",
                         HasResult("FEnumProperty::Enum"), HasConfirmed("FEnumProperty::Enum"));

                    // ---------- [Step 10.6] FArray/FSet/FMap tail layouts ----------
                    PDBG("---------- [Step 10.6] FArray/FSet/FMap tail layouts ----------");
                    Phase5_ProbeFContainerPropertyTails();
                    PDBG("Phase5: FArrayProperty::Inner => has={}, confirmed={}",
                         HasResult("FArrayProperty::Inner"), HasConfirmed("FArrayProperty::Inner"));
                    PDBG("Phase5: FSetProperty::ElementProp => has={}, confirmed={}",
                         HasResult("FSetProperty::ElementProp"), HasConfirmed("FSetProperty::ElementProp"));
                    PDBG("Phase5: FMapProperty::KeyProp => has={}, confirmed={}",
                         HasResult("FMapProperty::KeyProp"), HasConfirmed("FMapProperty::KeyProp"));
                    PDBG("Phase5: FMapProperty::ValueProp => has={}, confirmed={}",
                         HasResult("FMapProperty::ValueProp"), HasConfirmed("FMapProperty::ValueProp"));
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
// Phase 6: ProcessEvent virtual index.
// ============================================================

void UEProber::Phase6_ScanProcessEvent() {
    PDBG(">>>>>>>>>> [ScanProcessEvent] BEGIN>>>>>>>>>>");
    m_Phase6ProcessEventCandidates.clear();
    PDBG("ScanProcessEvent: 开始探测 ProcessEvent VTable 索引 (via Profile::findProcessEvent)");

    uintptr_t obj0 = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(0));
    PDBG("ScanProcessEvent: obj[0]=0x{:X}", obj0);
    if (!obj0 || !IsValidPtr(obj0)) {
        PDBG("ScanProcessEvent: obj[0] 无效, 中止");
        PDBG("<<<<<<<<<< [ScanProcessEvent] END <<<<<<<<<<");
        return;
    }

    uintptr_t peAddr = 0;
    int peIndex = -1;
    if (!ProfileFindProcessEvent(reinterpret_cast<uint8_t*>(obj0), &peAddr, &peIndex)) {
        PDBG("ScanProcessEvent: Profile::findProcessEvent 未找到 ProcessEvent, 中止");
        PDBG("<<<<<<<<<< [ScanProcessEvent] END <<<<<<<<<<");
        return;
    }
    PDBG("ScanProcessEvent: findProcessEvent => index=0x{:X}, addr=0x{:X}", peIndex, peAddr);

    m_Phase6ProcessEventCandidates.push_back({
        peIndex, peAddr,
        std::format("VTable[0x{:X}] = 0x{:X} [Profile]", peIndex, peAddr),
        1.0f
    });
    PDBG("ScanProcessEvent: 扫描完毕, 候选数={}", m_Phase6ProcessEventCandidates.size());

    auto& result = GetResult("ProcessEvent::VTableIdx");
    result.offset = peIndex;
    result.autoDetected = true;
    result.confirmed = true;
    result.evidence = std::format("Profile::findProcessEvent addr=0x{:X}", peAddr);
    PDBG("ScanProcessEvent: 最佳候选 偏移=0x{:X}, confidence=1.00", peIndex);
    PDBG("ScanProcessEvent: ProcessEvent::VTableIdx 自动探测: 索引 0x{:X} [已自动确认]", peIndex);
    PDBG("<<<<<<<<<< [ScanProcessEvent] END <<<<<<<<<<");
}

void UEProber::Phase6_AutoProbe() {
    if (!m_GameDetected) { PDBG("请先检测游戏后再进行探测操作"); return; }
    PDBG(">>>>>>>>>> [Phase6_AutoProbe] BEGIN>>>>>>>>>>");
    m_PhaseStatus[6] = EPhaseStatus::InProgress;
    PDBG("===== 阶段 6: ProcessEvent VTable 索引 =====");

    PDBG("---------- [Step 1/2] ScanProcessEvent ----------");
    Phase6_ScanProcessEvent();
    PDBG("ProcessEvent 探测结果: confirmed={}", HasConfirmed("ProcessEvent::VTableIdx"));

    PDBG("---------- [Step 2/2] ProbeUEnumNames ----------");
    Phase6_ProbeUEnumNames();
    PDBG("UEnum::Names 探测结果: confirmed={}", HasConfirmed("UEnum::Names"));

    PDBG("阶段 6 自动探测完成");
    PDBG("<<<<<<<<<< [Phase6_AutoProbe] END <<<<<<<<<<");
}

void UEProber::Phase6_ProbeUEnumNames() {
    PDBG(">>>>>>>>>> [ProbeUEnumNames] BEGIN >>>>>>>>>>");

    int32_t classOff = GetConfirmedOffset("UObject::ClassPrivate");
    if (classOff < 0) {
        PDBG("ProbeUEnumNames: 缺 UObject::ClassPrivate, 中止");
        PDBG("<<<<<<<<<< [ProbeUEnumNames] END <<<<<<<<<<");
        return;
    }

    uintptr_t enumMeta = FindObjectInGObjects("Enum", "Class");  // the UEnum metaclass
    if (!enumMeta) {
        PDBG("ProbeUEnumNames: 找不到 \"Enum\" 元类, 中止");
        PDBG("<<<<<<<<<< [ProbeUEnumNames] END <<<<<<<<<<");
        return;
    }

    // TPair<FName,int64> stride: align8(FName.Size) + sizeof(int64).
    FNameLayout fl = ProfileGetFNameLayout();
    const int fnameSize = (fl.size >= 4 && fl.size <= 16) ? fl.size : 8;
    const int alignedFName = (fnameSize + 7) & ~7;
    const int pairSize  = alignedFName + 8;

    // Collect UEnum samples by class-pointer identity (cheap, no name resolve).
    std::vector<uintptr_t> enums;
    const int total = BridgeGetObjectNum();
    for (int i = 0; i < total && (int)enums.size() < 8; ++i) {
        uintptr_t o = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(i));
        if (!o) continue;
        uintptr_t oClass = 0;
        if (!KMgrRead(o + classOff, &oClass, 8)) continue;
        if (oClass == enumMeta) enums.push_back(o);
    }
    if (enums.empty()) {
        PDBG("ProbeUEnumNames: 没找到 UEnum 样本, 中止");
        PDBG("<<<<<<<<<< [ProbeUEnumNames] END <<<<<<<<<<");
        return;
    }
    PDBG("ProbeUEnumNames: 收集 {} 个 UEnum 样本, pairSize=0x{:X}", enums.size(), pairSize);

    auto validateArray = [&](uintptr_t e, int32_t off) -> bool {
        uintptr_t data = 0; int32_t num = 0, max = 0;
        if (!KMgrRead(e + off, &data, 8) || !IsValidPtr(data)) return false;
        if (!KMgrRead(e + off + 8, &num, 4)) return false;
        if (!KMgrRead(e + off + 12, &max, 4)) return false;
        if (num < 1 || num > 4096) return false;
        if (max < num || max > 65536) return false;
        // First up-to-2 elements must be {resolvable FName, small enum value}.
        const int check = num >= 2 ? 2 : 1;
        for (int k = 0; k < check; ++k) {
            uintptr_t pair = data + (uintptr_t)k * pairSize;
            std::string nm;
            if (!TryReadFName(pair, nm)) return false;
            uint64_t val = 0;
            if (!KMgrRead(pair + alignedFName, &val, 8)) return false;
            if (val >= 0x10000) return false;  // string-byte "values" are huge → excludes CppType FString
        }
        return true;
    };

    // UEnum::Names lives after UObject(0x28)+Next+CppType; scan a fixed window
    // covering standard (0x40) and padded (NiZhan 0x58) layouts.
    int bestOff = -1, bestScore = 0;
    for (int32_t off = 0x28; off <= 0xC0; off += 8) {
        int score = 0;
        for (uintptr_t e : enums) if (validateArray(e, off)) score++;
        if (score > bestScore) { bestScore = score; bestOff = off; }
    }

    const int need = (int)enums.size() >= 3 ? 3 : (int)enums.size();
    if (bestOff >= 0 && bestScore >= need) {
        auto& r = GetResult("UEnum::Names");
        r.offset = bestOff; r.size = 16; r.typeName = "TArray<TPair<FName,int64>>";
        r.evidence = "probed via UEnum cast-flag samples";
        r.autoDetected = true; r.confirmed = true;
        PDBG("ProbeUEnumNames: UEnum::Names = 0x{:X} ({}/{} enums)", bestOff, bestScore, enums.size());
    } else {
        PDBG("ProbeUEnumNames: 无一致候选 (best=0x{:X} score={}/{}), 回退派生公式",
             bestOff, bestScore, enums.size());
    }
    PDBG("<<<<<<<<<< [ProbeUEnumNames] END <<<<<<<<<<");
}

// ============================================================
// Engine-version invocation helper.
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

    // Find KismetSystemLibrary UClass.
    uintptr_t kismetClass = FindObjectInGObjects("KismetSystemLibrary", "Class");
    if (!kismetClass) {
        PDBG("GetEngVer: KismetSystemLibrary UClass 未找到");
        PDBG("<<<<<<<<<< [CallGetEngineVersion] END <<<<<<<<<<");
        return;
    }
    PDBG("GetEngVer: KismetSystemLibrary @ {}", kismetClass);

    // Read the class default object.
    uintptr_t cdo = 0;
    if (!KMgrRead(kismetClass + classDefaultObjOff, &cdo, 8) || !IsValidPtr(cdo)) {
        PDBG("GetEngVer: CDO 读取失败 @ offset 0x{:X}", classDefaultObjOff);
        PDBG("<<<<<<<<<< [CallGetEngineVersion] END <<<<<<<<<<");
        return;
    }
    PDBG("GetEngVer: CDO @ {}", cdo);

    // Find GetEngineVersion along Children->Next.
    uintptr_t func = WalkChildrenChain(kismetClass, "GetEngineVersion", childrenOff, nextOff, namePrivateOff);
    if (!func) {
        PDBG("GetEngVer: GetEngineVersion UFunction 未找到");
        PDBG("<<<<<<<<<< [CallGetEngineVersion] END <<<<<<<<<<");
        return;
    }
    PDBG("GetEngVer: GetEngineVersion @ {}", func);

    // Use the ProcessEvent virtual index stored in probe results.
    int32_t peIdx = 0;
    auto peIt = m_Results.find("ProcessEvent::VTableIdx");
    if (peIt != m_Results.end() && peIt->second.confirmed && peIt->second.offset > 0)
        peIdx = peIt->second.offset;
    if (peIdx == 0) {
        PDBG("GetEngVer: ProcessEvent::VTableIdx 未探测到，无法继续");
        PDBG("<<<<<<<<<< [CallGetEngineVersion] END <<<<<<<<<<");
        return;
    }
    PDBG("GetEngVer: ProcessEventIdx = 0x{:X}", peIdx);

    // Read the ProcessEvent pointer from the vtable.
    uintptr_t vtable = 0;
    if (!KMgrRead(cdo, &vtable, 8) || !IsValidPtr(vtable)) {
        PDBG("GetEngVer: VTable 读取失败");
        PDBG("<<<<<<<<<< [CallGetEngineVersion] END <<<<<<<<<<");
        return;
    }
    PDBG("GetEngVer: VTable @ {}", vtable);

    uintptr_t peFunc = 0;
    if (!KMgrRead(vtable + peIdx * 8, &peFunc, 8) || peFunc == 0) {
        PDBG("GetEngVer: ProcessEvent 函数指针读取失败 @ VTable[0x{:X}], peFunc=0x{:X}",
             peIdx, peFunc);
        PDBG("<<<<<<<<<< [CallGetEngineVersion] END <<<<<<<<<<");
        return;
    }

    // Compare against the current range heuristic; an out-of-range warning does not prevent invocation.
    uintptr_t textStart = GetTextSegStart();
    uintptr_t textEnd = GetTextSegEnd();
    PDBG("GetEngVer: ProcessEvent func @ 0x{:X}, .text=[0x{:X}, 0x{:X})",
         peFunc, textStart, textEnd);
    if (textStart && textEnd && (peFunc < textStart || peFunc >= textEnd)) {
        PDBG("GetEngVer: 警告 - ProcessEvent 不在 .text 段内, 但仍尝试调用");
    }

    // Invoke ProcessEvent(CDO, GetEngineVersion, &parms) without a verified game-thread executor.
    using ProcessEventFn = void(*)(const void*, void*, void*);
    auto pe = reinterpret_cast<ProcessEventFn>(peFunc);

    // GetEngineVersion writes an FString out-param (16B: {TCHAR* Data; int32 Num;
    // int32 Max}). Read it offset-wise instead of via a typed FString; UE Android
    // stores UTF-16, so narrow printable ASCII best-effort (diagnostic only).
    uint8_t parms[0x10] = {};
    pe(reinterpret_cast<const void*>(cdo), reinterpret_cast<void*>(func), parms);

    std::string version;
    uintptr_t dataPtr = 0;
    int32_t count = 0;
    memcpy(&dataPtr, parms, sizeof(dataPtr));
    memcpy(&count, parms + sizeof(void*), sizeof(count));
    if (dataPtr && count > 0 && count < 256) {
        std::vector<uint16_t> wbuf(static_cast<size_t>(count));
        if (KMgrRead(dataPtr, wbuf.data(), static_cast<size_t>(count) * sizeof(uint16_t))) {
            for (uint16_t c : wbuf) {
                if (!c) break;
                if (c >= 0x20 && c < 0x7F) version += static_cast<char>(c);
            }
        }
    }
    m_EngineVersion = version;
    PDBG("GetEngVer: EngineVersion = {}", version);
    PDBG("<<<<<<<<<< [CallGetEngineVersion] END <<<<<<<<<<");

    LogSuccess(std::format("Engine Version: {}", version));
}

// ============================================================
// Main ImGui entry.
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

    // Menu bar.
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("探测选项")) {
            if (ImGui::MenuItem("全部自动探测", nullptr, false, m_GameDetected)) {
                Phase1_AutoProbe();
                Phase2_AutoProbe();
                Phase3_AutoProbe();
                Phase4_AutoProbe();
                Phase5_AutoProbe();
                Phase6_AutoProbe();
            }
            if (ImGui::MenuItem("导出探测结果")) m_CurrentPhase = 99;
            if (ImGui::MenuItem("探测结果总览")) m_CurrentPhase = 100;
            if (ImGui::MenuItem("清空探测结果")) {
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
        case 0: DrawDumpPanel(); break;
        case 1: DrawPhase1(); break;
        case 2: DrawPhase2(); break;
        case 3: DrawPhase3(); break;
        case 4: DrawPhase4(); break;
        case 5: DrawPhase5(); break;
        case 6: DrawPhase6(); break;
        case 99: DrawExportPanel(); break;
        case 100: DrawResultsSummary(); break;
    }

    ImGui::End();
}

// ============================================================
// Phase selector.
// ============================================================

void UEProber::DrawPhaseSelector() {
    static const char* phaseNames[] = {
        "Dump", "1.UObject", "2.UField/UStruct", "3.UClass",
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
// Shared candidate table.
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
                    target.autoDetected = false; // Manual confirmation.
                    LogSuccess(std::format("{} 手动确认: 偏移 0x{:X}", target.name, c.offset));
                }
            }
            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    // Manual input.
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
// Memory dump view.
// ============================================================

void UEProber::DrawMemoryDump(uintptr_t address, int32_t size, const std::string& label) {
    if (!address || size <= 0) return;

    m_DumpBuffer.resize(size);
    if (!KMgrRead(address, m_DumpBuffer.data(), size)) {
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
// Phase 1 view.
// ============================================================

void UEProber::DrawPhase1() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "阶段 1: UObject 基础成员探测");
    ImGui::TextWrapped("探测 VTable, Index, Name, Class, Outer, Flags 六个成员的偏移。");
    ImGui::Spacing();

    // Prerequisite configuration.
    if (ImGui::TreeNodeEx("前置配置", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushItemWidth(80);
        ImGui::InputInt("探测范围", &m_ProbeRange, 0x10, 0x40);
        ImGui::PopItemWidth();
        if (m_ProbeRange < 0x20) m_ProbeRange = 0x20;
        if (m_ProbeRange > 0x200) m_ProbeRange = 0x200;

        // Display object-array entries zero through four.
        for (int i = 0; i <= 4; ++i) {
            uintptr_t obj = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(i));
            if (obj && IsValidPtr(obj)) {
                std::string fullName;
                if (TryGetFullName(obj, fullName)) {
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                        "  obj[%d] = %s  [%s]", i, FormatPtr(obj).c_str(), fullName.c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                        "  obj[%d] = %s", i, FormatPtr(obj).c_str());
                }
            } else {
                ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                    "  obj[%d] = 无效", i);
            }
        }
        ImGui::TreePop();
    }

    ImGui::Spacing();
    if (ImGui::Button("自动探测阶段 1")) {
        Phase1_AutoProbe();
    }
    ImGui::Spacing();

    // Field candidates.
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

    // Memory dump.
    if (ImGui::TreeNode("内存查看")) {
        uintptr_t obj0 = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(0));
        if (obj0 && IsValidPtr(obj0))
            DrawMemoryDump(obj0, 0x40, "obj[0]");
        uintptr_t obj1 = reinterpret_cast<uintptr_t>(BridgeGetObjectByIndex(1));
        if (obj1 && IsValidPtr(obj1))
            DrawMemoryDump(obj1, 0x40, "obj[1]");
        ImGui::TreePop();
    }
}

// ============================================================
// Phase 2 view.
// ============================================================

void UEProber::DrawPhase2() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "阶段 2: UField / UStruct 探测");
    ImGui::TextWrapped("探测 UField::Next, UStruct::Super, Size, MinAlignment, Children, ChildProperties");
    ImGui::Spacing();

    bool canAutoProbe = HasConfirmed("UObject::NamePrivate") && HasConfirmed("UObject::ClassPrivate");
    if (!canAutoProbe)
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.2f, 1.0f), "请先完成阶段 1 (确认 Name 和 Class)");

    if (ImGui::Button("自动探测阶段 2") && canAutoProbe) {
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

    // Cached UClass addresses.
    if (ImGui::TreeNode("关键地址缓存")) {
        ImGui::Text("ClassClass:    %s", FormatPtr(m_ClassClass).c_str());
        ImGui::Text("ClassStruct:   %s", FormatPtr(m_ClassStruct).c_str());
        ImGui::Text("ClassField:    %s", FormatPtr(m_ClassField).c_str());
        ImGui::Text("ClassObject:   %s", FormatPtr(m_ClassObject).c_str());

        // Structure sizes.
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
// Phase 3 view.
// ============================================================

void UEProber::DrawPhase3() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "阶段 3: UClass 成员探测");
    ImGui::TextWrapped("探测 UClass::CastFlags, DefaultObject");
    ImGui::Spacing();

    if (ImGui::Button("自动探测阶段 3")) {
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
// Phase 4 view.
// ============================================================

void UEProber::DrawPhase4() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "阶段 4: UFunction 探测");
    ImGui::TextWrapped("探测 FunctionFlags, NumParms, ParmsSize, ReturnValueOffset, Func");
    ImGui::Spacing();

    // Cached function-anchor addresses.
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
    if (ImGui::Button("自动探测阶段 4")) {
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

    // Memory dump.
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
// Phase 5 view.
// ============================================================

void UEProber::DrawPhase5() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "阶段 5: FField / FProperty 探测");
    ImGui::TextWrapped("探测 FField::VTable, Owner, Name, Next, ClassPrivate 及 FProperty 成员");
    ImGui::Spacing();

    if (ImGui::Button("自动探测阶段 5")) {
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
// Phase 6 view.
// ============================================================

void UEProber::DrawPhase6() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "阶段 6: ProcessEvent VTable 索引");
    ImGui::TextWrapped("确定 UObject::ProcessEvent 在虚函数表中的索引");
    ImGui::Spacing();

    if (ImGui::Button("扫描 VTable")) {
        Phase6_AutoProbe();
    }
    ImGui::Spacing();

    if (ImGui::TreeNode("ProcessEvent VTable 索引")) {
        DrawCandidateTable("ProcessEvent VTable Index", m_Phase6ProcessEventCandidates, GetResult("ProcessEvent::VTableIdx"));
        ImGui::TreePop();
    }
}

// ============================================================
// Result overview.
// ============================================================

void UEProber::DrawResultsSummary() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "探测结果总览");
    ImGui::Spacing();

    // Group by category.
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

    // Log region.
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
// Export panel.
// ============================================================

void UEProber::DrawExportPanel() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "导出探测结果");
    ImGui::Spacing();

    // Query engine version automatically from this UI path.
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

    // Field definitions.
    struct FieldInfo {
        std::string resultKey;   // Key in m_Results.
        std::string memberName;  // C++ member name.
        std::string typeName;    // C++ type.
        int32_t     size;        // Field width in bytes.
    };

    // Class definitions.
    struct ClassDef {
        std::string className;
        std::string parentClass;     // Empty means no base class.
        std::string sizeStructName;  // GetStructSize argument; empty selects sizeResultKey.
        std::string sizeResultKey;   // Result key such as sizeof(FProperty).
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

    // Code generation.
    std::string code;
    std::string verLabel = m_EngineVersion.empty() ? "Unknown" : m_EngineVersion;
    code += "// ============================================================\n";
    code += "//  Generated by AndUEProber\n";
    code += "//  https://github.com/DumpA1n/AndUEProber\n";
    code += std::format("//  Engine Version: \"{}\"\n", verLabel);
    code += "// ============================================================\n\n";

    // Track each class's last field end (offset + size)
    // for non-POD base tail-padding reuse under the assumed Itanium ABI.
    std::map<std::string, int32_t> classContentEnd;

    for (auto& cls : classes) {
        // Sort confirmed fields by offset.
        struct ResolvedField {
            std::string memberName;
            std::string typeName;
            int32_t offset;
            int32_t size;
        };
        std::vector<ResolvedField> resolved;

        for (auto& f : cls.fields) {
            // The vtable is assumed at zero and is not stored in m_Results.
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

        // Compute the class content end.
        int32_t contentEnd = 0;
        for (auto& f : resolved)
            contentEnd = std::max(contentEnd, f.offset + f.size);
        classContentEnd[cls.className] = contentEnd;

        // Read the structure size.
        int32_t structSize = 0;
        if (!cls.sizeStructName.empty())
            structSize = GetStructSize(cls.sizeStructName);
        if (structSize <= 0 && !cls.sizeResultKey.empty()) {
            auto it = m_Results.find(cls.sizeResultKey);
            if (it != m_Results.end() && it->second.offset > 0)
                structSize = it->second.offset; // The sizeof(FProperty) result stores the size in its offset field.
        }

        // Read the base size to place derived members.
        // Use contentEnd for tail-padding reuse in non-POD bases with vtables.
        int32_t parentSize = 0;
        if (!cls.parentClass.empty()) {
            auto ceIt = classContentEnd.find(cls.parentClass);
            if (ceIt != classContentEnd.end())
                parentSize = ceIt->second;
            else
                parentSize = GetStructSize(cls.parentClass);
        }

        // Identify a root with a vtable that needs a nontrivial destructor.
        bool isNonPodRoot = cls.parentClass.empty() &&
            std::any_of(resolved.begin(), resolved.end(),
                [](const ResolvedField& f) { return f.memberName == "VTable"; });

        // Emit the class definition.
        if (cls.parentClass.empty())
            code += std::format("class {} {{\npublic:\n", cls.className);
        else
            code += std::format("class {} : public {} {{\npublic:\n", cls.className, cls.parentClass);

        // Emit a destructor for the non-POD root to permit the assumed ABI's tail-padding reuse.
        if (isNonPodRoot)
            code += std::format("    ~{}() {{}}\n\n", cls.className);

        int32_t cursor = parentSize; // Current output offset.
        int padIdx = 0;

        for (auto& f : resolved) {
            if (f.offset < cursor) continue; // Skip fields overlapping the base.

            // Emit padding.
            if (f.offset > cursor) {
                int32_t gap = f.offset - cursor;
                code += std::format("    {:50s}Pad_{:02X}[0x{:X}];\n",
                    "uint8", cursor, gap);
                ++padIdx;
            }

            // Emit the field.
            code += std::format("    {:50s}{};\n", f.typeName, f.memberName);
            cursor = f.offset + f.size;
        }

        // Tail padding.
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
// Shared offset table helper.
// ============================================================

void UEProber::DrawOffsetTable(const std::string& category) {
    // Reserved for a detailed category table.
}

// ============================================================
// AndUEDumper integration panel.
// ============================================================

void UEProber::DrawDumpPanel() {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "AndUEDumper 集成 Dump");
    ImGui::Spacing();

    // Target-profile detection.
    if (!m_GameDetected) {
        ImGui::TextWrapped("点击下方按钮自动检测当前运行的游戏，获取 GObjects 和 FName 解析器。");
        ImGui::Spacing();
        if (ImGui::Button("检测游戏")) {
            DetectGame();
        }
        return;
    }

    // Detected profile.
    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "游戏: %s", m_GameDetection.GameName.c_str());
    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "包名: %s", m_GameDetection.PackageName.c_str());
    auto addrLine = [](const char* label, uintptr_t addr) {
        if (addr) ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s: 0x%lX", label, addr);
        else      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s: N/A", label);
    };
    addrLine("UE Base", m_GameDetection.UEBaseAddress);
    addrLine("GUObjectArrayPtr",  m_GameDetection.GUObjectArrayPtr);
    addrLine("Objects Field Addr", m_GameDetection.ObjectsFieldAddr);
    addrLine("DecryptFName", m_GameDetection.DecryptFNameAddr);
    ImGui::Spacing();

    if (m_GObjectsInitialized) {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "GObjects 和 FName 已初始化，探测功能已就绪。");
        ImGui::Spacing();

        if (ImGui::Button("全部自动探测 (阶段 1~6)")) {
            Phase1_AutoProbe();
            Phase2_AutoProbe();
            Phase3_AutoProbe();
            Phase4_AutoProbe();
            Phase5_AutoProbe();
            Phase6_AutoProbe();
            LogSuccess("全部自动探测完成");
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "一键执行所有阶段的自动探测");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Probe result counts.
    int confirmedProbeCount = 0;
    {
        struct OffsetGroup { const char* className; std::initializer_list<const char*> names; };
        static const OffsetGroup kGroups[] = {
            {"UObject",   {"UObject::ObjectFlags","UObject::InternalIndex","UObject::ClassPrivate","UObject::NamePrivate","UObject::OuterPrivate"}},
            {"UField",    {"UField::Next"}},
            {"UStruct",   {"UStruct::SuperStruct","UStruct::Children","UStruct::ChildProperties","UStruct::PropertiesSize"}},
            {"UFunction", {"UFunction::FunctionFlags","UFunction::NumParms","UFunction::ParmsSize","UFunction::Func"}},
            {"FField",    {"FField::ClassPrivate","FField::Next","FField::NamePrivate","FField::FlagsPrivate"}},
            {"FProperty", {"FProperty::ArrayDim","FProperty::ElementSize","FProperty::PropertyFlags","FProperty::Offset_Internal","sizeof(FProperty)"}},
        };

        int total = 0, confirmed = 0;
        for (auto& g : kGroups)
            for (auto* n : g.names) { ++total; if (HasConfirmed(n)) ++confirmed; }

        confirmedProbeCount = confirmed;
        if (confirmed > 0)
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "探测偏移: 已启用 (%d/%d)", confirmed, total);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "探测偏移: 未使用 (将使用 Profile 默认偏移)");

        if (confirmed < total) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "未确认:");
            for (auto& g : kGroups) {
                std::string missing;
                for (auto* n : g.names) {
                    if (!HasConfirmed(n)) {
                        if (!missing.empty()) missing += ", ";
                        const char* m = strstr(n, "::"); missing += m ? m + 2 : n;
                    }
                }
                if (!missing.empty())
                    ImGui::BulletText("%s: %s", g.className, missing.c_str());
            }
        }
    }

    ImGui::Spacing();

    // Dump controls using available probe results.
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

    const char* dumpBtnLabel = (confirmedProbeCount > 0) ? "开始 Dump (使用探测偏移)" : "开始 Dump (使用 Profile 默认偏移)";
    if (ImGui::Button(dumpBtnLabel)) {
        StartDump();
    }

    if (!canDump) ImGui::EndDisabled();

    ImGui::SameLine();
    if (confirmedProbeCount > 0)
        ImGui::TextWrapped("将当前探测到的偏移发送给 AndUEDumper 进行 SDK dump。");
    else
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "未探测到偏移, 将使用 AndUEDumper 预设偏移。");
}

void UEProber::RunAutoDumpFlow() {
    PDBG("[AutoDump] DetectGame...");
    DetectGame();
    if (!m_GameDetected) {
        PDBG("[AutoDump] DetectGame failed; aborting flow");
        m_DumpError = "DetectGame failed";
        m_DumpStatus.store(EDumpStatus::Failed);
        return;
    }

    PDBG("[AutoDump] Phase1~6 AutoProbe...");
    Phase1_AutoProbe();
    Phase2_AutoProbe();
    Phase3_AutoProbe();
    Phase4_AutoProbe();
    Phase5_AutoProbe();
    Phase6_AutoProbe();
    PDBG("[AutoDump] AutoProbe phases done; invoking StartDump");
    StartDump();
}

void UEProber::DetectGame() {
    if (m_GameDetected) return;

    GameDetectionResult result;
    if (!DetectAndPrepareGame(result)) {
        PDBG("游戏检测失败: 没有匹配的 Profile 或初始化失败");
        return;
    }

    m_GameDetection = result;
    m_GameDetected = true;
    // DetectAndPrepareGame -> InitUEVars already wired the dumper's offset-based
    // object array + FName resolution (UEWrappers). The prober iterates objects
    // via the bridge (BridgeGetObjectByIndex/Num) and reads names via
    // TryReadFName, so there is no local TUObjectArray / FName layout to set up.
    m_GObjectsInitialized = true;

    PDBG("检测到游戏: {} ({})", result.GameName, result.PackageName);
    PDBG("UE Base: 0x{:X}, GUObjectArray: 0x{:X}, ObjectsFieldAddr: 0x{:X}",
         result.UEBaseAddress, result.GUObjectArrayPtr, result.ObjectsFieldAddr);
    PDBG("GObjects ready (via UEVars): NumElementsPerChunk={} ({})",
         result.NumElementsPerChunk, result.NumElementsPerChunk > 0 ? "chunked" : "flat");
}

void UEProber::StartDump() {
    if (m_DumpStatus.load() == EDumpStatus::Running)
        return;

    if (!m_GameDetected) {
        m_DumpError = "请先检测游戏。";
        m_DumpStatus.store(EDumpStatus::Failed);
        return;
    }

    // Collect ProbedOffsets from the result map.
    ProbedOffsets offsets{};

    // UObject
    if (HasConfirmed("UObject::ObjectFlags"))          offsets.objFlags = GetConfirmedOffset("UObject::ObjectFlags");
    if (HasConfirmed("UObject::InternalIndex"))          offsets.objIndex = GetConfirmedOffset("UObject::InternalIndex");
    if (HasConfirmed("UObject::ClassPrivate"))          offsets.objClass = GetConfirmedOffset("UObject::ClassPrivate");
    if (HasConfirmed("UObject::NamePrivate"))           offsets.objName  = GetConfirmedOffset("UObject::NamePrivate");
    if (HasConfirmed("UObject::OuterPrivate"))          offsets.objOuter = GetConfirmedOffset("UObject::OuterPrivate");

    // UField
    if (HasConfirmed("UField::Next"))            offsets.fieldNext = GetConfirmedOffset("UField::Next");

    // UEnum
    if (HasConfirmed("UEnum::Names"))            offsets.uenumNames = GetConfirmedOffset("UEnum::Names");

    // UStruct
    if (HasConfirmed("UStruct::SuperStruct"))          offsets.structSuper     = GetConfirmedOffset("UStruct::SuperStruct");
    if (HasConfirmed("UStruct::Children"))       offsets.structChildren  = GetConfirmedOffset("UStruct::Children");
    if (HasConfirmed("UStruct::ChildProperties")) offsets.structChildProps = GetConfirmedOffset("UStruct::ChildProperties");
    if (HasConfirmed("UStruct::PropertiesSize")) offsets.structSize      = GetConfirmedOffset("UStruct::PropertiesSize");

    // UClass
    if (HasConfirmed("UClass::CastFlags"))           offsets.uclassCastFlags     = GetConfirmedOffset("UClass::CastFlags");
    if (HasConfirmed("UClass::ClassDefaultObject"))  offsets.uclassDefaultObject = GetConfirmedOffset("UClass::ClassDefaultObject");

    // UFunction
    if (HasConfirmed("UFunction::FunctionFlags")) offsets.funcFlags    = GetConfirmedOffset("UFunction::FunctionFlags");
    if (HasConfirmed("UFunction::NumParms"))       offsets.funcNumParams = GetConfirmedOffset("UFunction::NumParms");
    if (HasConfirmed("UFunction::ParmsSize"))      offsets.funcParamSize = GetConfirmedOffset("UFunction::ParmsSize");
    if (HasConfirmed("UFunction::Func"))           offsets.funcFunc      = GetConfirmedOffset("UFunction::Func");

    // FField
    if (HasConfirmed("FField::ClassPrivate"))     offsets.ffieldClass = GetConfirmedOffset("FField::ClassPrivate");
    if (HasConfirmed("FField::Owner"))            offsets.ffieldOwner = GetConfirmedOffset("FField::Owner");
    if (HasConfirmed("FField::Next"))             offsets.ffieldNext  = GetConfirmedOffset("FField::Next");
    if (HasConfirmed("FField::NamePrivate"))      offsets.ffieldName  = GetConfirmedOffset("FField::NamePrivate");
    if (HasConfirmed("FField::FlagsPrivate"))     offsets.ffieldFlags = GetConfirmedOffset("FField::FlagsPrivate");

    // FProperty
    if (HasConfirmed("FProperty::ArrayDim"))      offsets.fpropArrayDim = GetConfirmedOffset("FProperty::ArrayDim");
    if (HasConfirmed("FProperty::ElementSize"))   offsets.fpropElemSize = GetConfirmedOffset("FProperty::ElementSize");
    if (HasConfirmed("FProperty::PropertyFlags")) offsets.fpropFlags    = GetConfirmedOffset("FProperty::PropertyFlags");
    if (HasConfirmed("FProperty::Offset_Internal")) offsets.fpropOffset = GetConfirmedOffset("FProperty::Offset_Internal");
    if (HasConfirmed("sizeof(FProperty)"))        offsets.fpropSize    = GetConfirmedOffset("sizeof(FProperty)");
    if (HasConfirmed("FProperty::SubPropertyBase")) offsets.fpropSubBase = GetConfirmedOffset("FProperty::SubPropertyBase");
    if (HasConfirmed("FEnumProperty::UnderlyingType")) offsets.fenumUnderlying = GetConfirmedOffset("FEnumProperty::UnderlyingType");
    if (HasConfirmed("FEnumProperty::Enum"))           offsets.fenumEnum       = GetConfirmedOffset("FEnumProperty::Enum");

    // Per-subclass container tail offsets (Step 10.6). Only override when the
    // probe found a real instance; otherwise leave zero so the dumper falls
    // back to fpropSubBase / runtime probe.
    if (HasConfirmed("FArrayProperty::Inner"))      offsets.farrayInner = GetConfirmedOffset("FArrayProperty::Inner");
    if (HasConfirmed("FSetProperty::ElementProp"))  offsets.fsetElement = GetConfirmedOffset("FSetProperty::ElementProp");
    if (HasConfirmed("FMapProperty::KeyProp"))      offsets.fmapKey     = GetConfirmedOffset("FMapProperty::KeyProp");
    if (HasConfirmed("FMapProperty::ValueProp"))    offsets.fmapValue   = GetConfirmedOffset("FMapProperty::ValueProp");

    StartDumpWithProbedOffsets(offsets, m_DumpStatus, m_DumpError, m_DumpOutputDir);
}

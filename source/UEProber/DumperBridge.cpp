// DumperBridge.cpp — compiled in AndUEDumperLib context (KittyMemoryEx headers only).
// Provides game detection + dump entry point for UEProber.

#include "DumperBridge.h"

#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>
#include <unistd.h>

#include "Dumper.hpp"
#include "UE/UEMemory.hpp"
#include "UE/UEOffsets.hpp"

#include "GameProfiles/IGameProfileEx.hpp"

// All game profiles from AndUEDumper
#include "UE/UEGameProfiles/ArenaBreakout.hpp"
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
#include "UE/UEGameProfiles/PUBG.hpp"
#include "GameProfiles/DeltaForce.hpp"
#include "GameProfiles/NiZhan.hpp"
#include "GameProfiles/RocoKingdom.hpp"

#include "Utils/Logger.hpp"

using namespace UEMemory;

// ============================================================
//  Bridge: expose kMgr.readMem without leaking KittyMemoryEx headers
// ============================================================

ssize_t KMgrReadMem(uintptr_t address, void* buffer, size_t size)
{
    return kMgr.readMem(address, buffer, size);
}

size_t KMgrWriteMem(uintptr_t address, void* buffer, size_t size)
{
    return kMgr.writeMem(address, buffer, size);
}

bool KMgrRead(uintptr_t address, void* buffer, size_t size)
{
    return kMgr.readMem(address, buffer, size) == (ssize_t)size;
}

bool KMgrIsValidPtr(uintptr_t address)
{
    if (!address || address < 0x10000000)
        return false;
    // 高位只允许全 0 或 Android tagged pointer 前缀
    uintptr_t highBits = address & ~(uintptr_t)0x7fffffffff;
    return highBits == 0 || highBits == (uintptr_t)0xB400000000000000;
}

// ============================================================
//  Global state: matched profile for the running game
// ============================================================

static IGameProfileEx* g_ExProfile = nullptr;
static bool g_KMgrInitialized = false;

static std::vector<IGameProfileEx*>& GetExProfiles()
{
    static std::vector<IGameProfileEx*> profiles = {
        new GameProfileEx<PESProfile>(),
        new GameProfileEx<DislyteProfile>(),
        new GameProfileEx<MortalKombatProfile>(),
        new GameProfileEx<FarlightProfile>(),
        new GameProfileEx<TorchlightProfile>(),
        new GameProfileEx<ArenaBreakoutProfile>(),
        new GameProfileEx<BlackCloverProfile>(),
        new GameProfileEx<WutheringWavesProfile>(),
        new GameProfileEx<RealBoxing2Profile>(),
        new GameProfileEx<OdinValhallaProfile>(),
        new GameProfileEx<Injustice2Profile>(),
        new GameProfileEx<RooftopParkourProfile>(),
        new GameProfileEx<BabyYellowProfile>(),
        new GameProfileEx<TowerFantasyProfile>(),
        new GameProfileEx<BladeSoulProfile>(),
        new GameProfileEx<Lineage2Profile>(),
        new GameProfileEx<Case2Profile>(),
        new GameProfileEx<CenturyProfile>(),
        new GameProfileEx<KingArthurProfile>(),
        new GameProfileEx<NightCrowsProfile>(),
        new GameProfileEx<HelloNeighborProfile>(),
        new GameProfileEx<HelloNeighborNDProfile>(),
        new GameProfileEx<SFG2Profile>(),
        new GameProfileEx<ArkUltimateProfile>(),
        new GameProfileEx<AuroriaProfile>(),
        new GameProfileEx<LineageWProfile>(),
        new GameProfileEx<RLSideswipeProfile>(),
        new GameProfileEx<PUBGProfile>(),
        new GameProfileEx<DeltaForceProfile>(),
        new GameProfileEx<NiZhanProfile>(),
        new GameProfileEx<RocoKingdomProfile>(),
    };
    return profiles;
}

// ============================================================
//  Phase 1: Detect game and prepare
// ============================================================

bool DetectAndPrepareGame(GameDetectionResult& result)
{
    result = {};

    std::string sGamePackage = getprogname();
    pid_t gamePID = getpid();

    LOGI("DetectAndPrepareGame: package=%s pid=%d", sGamePackage.c_str(), gamePID);

    // Initialize KittyMemoryEx for self-process reading
    if (!g_KMgrInitialized) {
        if (!kMgr.initialize(gamePID, EK_MEM_OP_SYSCALL, false) &&
            !kMgr.initialize(gamePID, EK_MEM_OP_IO, false)) {
            LOGE("Failed to initialize KittyMemoryMgr.");
            return false;
        }
        g_KMgrInitialized = true;

        // 启用 kPtrValidator region 缓存：否则每次 isPtrReadable 都会重开 /proc/<pid>/maps 全文解析，
        // 在 FName 查询热路径上会被放大到几十万次 fopen，造成严重卡顿。
        kPtrValidator.setPID(kMgr.processID());
        kPtrValidator.setUseCache(true);
        kPtrValidator.refreshRegionCache();
        if (kPtrValidator.cachedRegions().empty()) {
            LOGE("kPtrValidator: refreshRegionCache 失败，cachedRegions 为空。");
        } else {
            LOGI("kPtrValidator: 缓存了 %zu 个 region。", kPtrValidator.cachedRegions().size());
        }
    }

    // Match game by AppID (like dump_thread)
    g_ExProfile = nullptr;
    for (auto* ex : GetExProfiles()) {
        auto* profile = ex->AsGameProfile();
        for (auto& pkg : profile->GetAppIDs()) {
            if (sGamePackage == pkg) {
                g_ExProfile = ex;
                LOGI("Matched profile: %s (AppID: %s)", profile->GetAppName().c_str(), pkg.c_str());
                break;
            }
        }
        if (g_ExProfile) break;
    }

    if (!g_ExProfile) {
        LOGE("No matching profile for package: %s", sGamePackage.c_str());
        return false;
    }

    // Get UE module base
    auto ue_elf = g_ExProfile->AsGameProfile()->GetUnrealELF();
    if (!ue_elf.isValid()) {
        LOGE("Couldn't find a valid UE ELF in process maps.");
        g_ExProfile = nullptr;
        return false;
    }

    result.UEBaseAddress = ue_elf.base();
    LOGI("UE Base: %p", (void*)result.UEBaseAddress);

    // Call the profile's GetGUObjectArrayPtr to get GObjects address
    result.GUObjectArrayPtr = g_ExProfile->PublicGetGUObjectArrayPtr();
    if (result.GUObjectArrayPtr == 0) {
        LOGE("GetGUObjectArrayPtr returned 0.");
        g_ExProfile = nullptr;
        return false;
    }
    LOGI("GUObjectArrayPtr: %p (offset: 0x%lX)",
         (void*)result.GUObjectArrayPtr,
         result.GUObjectArrayPtr - result.UEBaseAddress);

    // Read Objects pointer from FUObjectArray
    UE_Offsets* profileOffsets = g_ExProfile->AsGameProfile()->GetOffsets();
    uintptr_t objObjectsAddr = result.GUObjectArrayPtr + profileOffsets->FUObjectArray.ObjObjects;
    result.ObjectsFieldAddr = objObjectsAddr + profileOffsets->TUObjectArray.Objects;
    LOGI("Objects field addr: %p (offset: 0x%lX)",
         (void*)result.ObjectsFieldAddr, result.ObjectsFieldAddr - result.UEBaseAddress);

    result.NumElementsPerChunk = static_cast<int32_t>(profileOffsets->TUObjectArray.NumElementsPerChunk);
    LOGI("TUObjectArray: elementsPerChunk=%d", result.NumElementsPerChunk);

    result.DecryptFNameAddr = g_ExProfile->PublicGetDecryptFNameAddr();
    if (result.DecryptFNameAddr)
        LOGI("DecryptFName: %p", (void*)result.DecryptFNameAddr);

    result.GameName = g_ExProfile->AsGameProfile()->GetAppName();
    result.PackageName = sGamePackage;
    result.Success = true;

    return true;
}

// ============================================================
//  FName resolution via matched profile
// ============================================================

std::string ProfileGetNameByID(int32_t id)
{
    if (!g_ExProfile)
        return "";

    return g_ExProfile->PublicGetNameByID(id);
}

bool ProfileFindProcessEvent(uint8_t* uObject, uintptr_t* pe_address_out, int* pe_index_out)
{
    if (!g_ExProfile)
        return false;

    return g_ExProfile->AsGameProfile()->findProcessEvent(uObject, pe_address_out, pe_index_out);
}

// ============================================================
//  Phase 2: Dump with probed offsets
// ============================================================

void StartDumpWithProbedOffsets(
    const ProbedOffsets& offsets,
    std::atomic<EDumpStatus>& status,
    std::string& outError,
    std::string& outDir)
{
    if (status.load() == EDumpStatus::Running)
        return;

    if (!g_ExProfile) {
        outError = "No matched profile. Call DetectAndPrepareGame first.";
        status.store(EDumpStatus::Failed);
        return;
    }

    status.store(EDumpStatus::Running);
    outError.clear();
    outDir.clear();

    // Build UE_Offsets from probed values, starting from the profile's base offsets
    UE_Offsets probedUEOffsets = *g_ExProfile->AsGameProfile()->GetOffsets();

    // Override with probed values (only non-zero)
    if (offsets.objFlags)      probedUEOffsets.UObject.ObjectFlags = offsets.objFlags;
    if (offsets.objIndex)      probedUEOffsets.UObject.InternalIndex = offsets.objIndex;
    if (offsets.objClass)      probedUEOffsets.UObject.ClassPrivate = offsets.objClass;
    if (offsets.objName)       probedUEOffsets.UObject.NamePrivate = offsets.objName;
    if (offsets.objOuter)      probedUEOffsets.UObject.OuterPrivate = offsets.objOuter;

    if (offsets.fieldNext)     probedUEOffsets.UField.Next = offsets.fieldNext;

    // usually at sizeof(UField) + sizeof(FString)
    if (offsets.fieldNext)   probedUEOffsets.UEnum.Names = probedUEOffsets.UField.Next + (sizeof(void *) * 2) + (sizeof(int32_t) * 2);

    if (offsets.structSuper)     probedUEOffsets.UStruct.SuperStruct = offsets.structSuper;
    if (offsets.structChildren)  probedUEOffsets.UStruct.Children = offsets.structChildren;
    if (offsets.structChildProps) probedUEOffsets.UStruct.ChildProperties = offsets.structChildProps;
    if (offsets.structSize)      probedUEOffsets.UStruct.PropertiesSize = offsets.structSize;

    if (offsets.funcFlags)     probedUEOffsets.UFunction.EFunctionFlags = offsets.funcFlags;
    if (offsets.funcNumParams) probedUEOffsets.UFunction.NumParams = offsets.funcNumParams;
    if (offsets.funcParamSize) probedUEOffsets.UFunction.ParamSize = offsets.funcParamSize;
    if (offsets.funcFunc)      probedUEOffsets.UFunction.Func = offsets.funcFunc;

    if (offsets.ffieldClass) probedUEOffsets.FField.ClassPrivate = offsets.ffieldClass;
    if (offsets.ffieldNext)  probedUEOffsets.FField.Next = offsets.ffieldNext;
    if (offsets.ffieldName)  probedUEOffsets.FField.NamePrivate = offsets.ffieldName;
    if (offsets.ffieldFlags) probedUEOffsets.FField.FlagsPrivate = offsets.ffieldFlags;

    if (offsets.fpropArrayDim) probedUEOffsets.FProperty.ArrayDim = offsets.fpropArrayDim;
    if (offsets.fpropElemSize) probedUEOffsets.FProperty.ElementSize = offsets.fpropElemSize;
    if (offsets.fpropFlags)    probedUEOffsets.FProperty.PropertyFlags = offsets.fpropFlags;
    if (offsets.fpropOffset)   probedUEOffsets.FProperty.Offset_Internal = offsets.fpropOffset;
    if (offsets.fpropSize)     probedUEOffsets.FProperty.Size = offsets.fpropSize;

    // Apply probed offsets to the matched Ex profile
    g_ExProfile->SetProbedOffsets(probedUEOffsets);

    // Capture references
    auto& dumpStatus = status;
    auto& dumpError  = outError;
    auto& dumpOutDir = outDir;

    std::thread([&dumpStatus, &dumpError, &dumpOutDir]() {
        std::string sGamePackage = getprogname();

        std::string sOutDirectory = KittyUtils::getExternalStorage();
        sOutDirectory += "/Android/data/";
        sOutDirectory += sGamePackage;
        sOutDirectory += "/files";

        std::string sDumpDir = sOutDirectory + "/UEDump3r";
        std::string sDumpGameDir = sDumpDir + "/" + sGamePackage;
        IOUtils::delete_directory(sDumpGameDir);

        if (IOUtils::mkdir_recursive(sDumpGameDir, 0777) == -1) {
            dumpError = "Couldn't create output directory: " + sDumpGameDir;
            dumpStatus.store(EDumpStatus::Failed);
            return;
        }

        UEDumper uEDumper{};

        uEDumper.setDumpExeInfoNotify([](bool bFinished) {
            if (!bFinished) LOGI("Dumping Executable Info...");
        });
        uEDumper.setDumpNamesInfoNotify([](bool bFinished) {
            if (!bFinished) LOGI("Dumping Names Info...");
        });
        uEDumper.setDumpObjectsInfoNotify([](bool bFinished) {
            if (!bFinished) LOGI("Dumping Objects Info...");
        });
        uEDumper.setDumpOffsetsInfoNotify([](bool bFinished) {
            if (!bFinished) LOGI("Dumping Offsets Info...");
        });
        uEDumper.setObjectsProgressCallback([](const SimpleProgressBar&) {
            static bool once = false;
            if (!once) { once = true; LOGI("Gathering UObjects...."); }
        });
        uEDumper.setDumpProgressCallback([](const SimpleProgressBar& bar) {
            static bool once = false;
            if (!once) { once = true; LOGI("Dumping...."); }
            bar.print();
        });

        bool dumpSuccess = false;
        std::unordered_map<std::string, BufferFmt> dumpbuffersMap;
        auto dmpStart = std::chrono::steady_clock::now();

        LOGI("Initializing Dumper with probed offsets...");
        if (uEDumper.Init(g_ExProfile->AsGameProfile())) {
            dumpSuccess = uEDumper.Dump(&dumpbuffersMap);
        }

        if (!dumpSuccess || dumpbuffersMap.empty()) {
            std::string err = uEDumper.GetLastError();
            dumpError = err.empty() ? "Dump failed." : err;
            dumpStatus.store(EDumpStatus::Failed);
            return;
        }

        LOGI("Saving Files...");
        for (const auto& it : dumpbuffersMap) {
            if (!it.first.empty()) {
                std::string path = KittyUtils::String::fmt("%s/%s", sDumpGameDir.c_str(), it.first.c_str());
                it.second.writeBufferToFile(path);
            }
        }

        auto dmpEnd = std::chrono::steady_clock::now();
        std::chrono::duration<float, std::milli> dmpDurationMS = (dmpEnd - dmpStart);

        if (!uEDumper.GetLastError().empty())
            LOGI("Dump Status: %s", uEDumper.GetLastError().c_str());
        LOGI("Dump Duration: %.2fms", dmpDurationMS.count());
        LOGI("Dump Location: %s", sDumpGameDir.c_str());

        dumpOutDir = sDumpGameDir;
        dumpStatus.store(EDumpStatus::Success);
    }).detach();
}

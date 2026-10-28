#pragma once

#include <atomic>
#include <csetjmp>
#include <csignal>
#include <cstdint>
#include <string>
#include <unistd.h>
#include <vector>

#include "UE/UEGameProfile.hpp"
using namespace UEMemory;

// Discovery shared by AArch64 Android builds whose GUObjectArray has no stable
// byte pattern and whose name entries are encrypted.
class IGameProfileAndroid : public IGameProfile
{
public:
    bool ArchSupprted() const override
    {
        return GetUnrealELF().header().e_machine == EM_AARCH64;
    }

    static uintptr_t DecodeBranchTarget(uint32_t insn, uintptr_t pc)
    {
        int32_t imm26 = insn & 0x03FFFFFF;
        if (imm26 & 0x02000000) imm26 |= (int32_t)0xFC000000;
        return pc + (int64_t)imm26 * 4;
    }

    // True when the profile's object array layout reads a sane object count and
    // a readable object 1 at address.
    bool IsObjectArray(uintptr_t address) const
    {
        if (!address) return false;
        const UE_Offsets *o = GetOffsets();
        const uintptr_t objObjects = address + o->FUObjectArray.ObjObjects;
        const int32_t count = vm_rpm_ptr<int32_t>((void *)(objObjects + o->TUObjectArray.NumElements));
        uintptr_t items = vm_rpm_ptr<uintptr_t>((void *)(objObjects + o->TUObjectArray.Objects));
        if (count <= 1 || count > 0x1000000 || !items) return false;
        if (o->TUObjectArray.NumElementsPerChunk) items = vm_rpm_ptr<uintptr_t>((void *)items);
        const uintptr_t object = vm_rpm_ptr<uintptr_t>((void *)(items + o->FUObjectItem.Size + o->FUObjectItem.Object));
        return object && IsPtrReadable(object);
    }

    uintptr_t FindAdrpXrefToAddr(uintptr_t target) const
    {
        if (!target) return 0;
        const uintptr_t targetPage = target & ~uintptr_t(0xFFF);

        const size_t kChunkInsns = 0x40000;
        std::vector<uint32_t> buf(kChunkInsns);

        for (auto &seg : GetUnrealELF().segments())
        {
            if (!seg.readable || !seg.is_private) continue;
            if (!isEmulator() && !seg.executable) continue;

            for (uintptr_t base = seg.startAddress; base < seg.endAddress; base += kChunkInsns * 4)
            {
                size_t remain = (seg.endAddress - base) / 4;
                size_t n = remain < kChunkInsns ? remain : kChunkInsns;
                if (!vm_rpm_ptr((void *)base, buf.data(), n * sizeof(uint32_t)))
                    continue;

                for (size_t i = 0; i < n; i++)
                {
                    uint32_t insn = buf[i];
                    if ((insn & 0x9F000000) != 0x90000000) continue;  // ADRP

                    int64_t imm = (int64_t)((((insn >> 5) & 0x7FFFF) << 2) | ((insn >> 29) & 0x3));
                    imm = (imm << 43) >> 43;
                    imm <<= 12;

                    uintptr_t pc = base + i * 4;
                    if (((pc & ~uintptr_t(0xFFF)) + (uintptr_t)imm) != targetPage) continue;
                    if (Arm64::DecodeADRL(pc) == target) return pc;
                }
            }
        }
        return 0;
    }

    // UGameEngine::FinishDestroy logs "Game engine shut down" and tail-calls
    // UObject::FinishDestroy, whose first global load is GUObjectArray.
    uintptr_t FindGUObjectArrayViaFinishDestroy() const
    {
        static uintptr_t cached = 0;
        static bool searched = false;
        if (searched) return cached;
        searched = true;

        static const char *kPat =
            "47 00 61 00 6D 00 65 00 20 00 65 00 6E 00 67 00 69 00 6E 00 65 00 "
            "20 00 73 00 68 00 75 00 74 00 20 00 64 00 6F 00 77 00 6E 00 00 00";
        uintptr_t strAddr = findIdaPattern(PATTERN_MAP_TYPE::ANY_R, kPat, 0);
        if (!strAddr) { LOGE("FinishDestroy: \"Game engine shut down\" not found"); return 0; }

        uintptr_t xref = FindAdrpXrefToAddr(strAddr);
        if (!xref) { LOGE("FinishDestroy: no ADRP reference to %p", (void *)strAddr); return 0; }

        uintptr_t funcStart = 0;
        for (int i = 0; i < 0x100; i++)
        {
            uintptr_t p = xref - (uintptr_t)i * 4;
            if ((vm_rpm_ptr<uint32_t>((void *)p) & 0xFC407FFF) == 0xA8007BFD) { funcStart = p; break; }  // STP X29,X30,[SP,..]
        }
        if (!funcStart) { LOGE("FinishDestroy: no prologue above %p", (void *)xref); return 0; }

        // The super call is the far tail-call B, or the last BL before RET.
        uintptr_t target = 0;
        for (uintptr_t p = funcStart; p < funcStart + 0x200; p += 4)
        {
            uint32_t insn = vm_rpm_ptr<uint32_t>((void *)p);
            if (insn == 0xD65F03C0) break;  // RET
            if ((insn & 0xFC000000) == 0x14000000)
            {
                uintptr_t t = DecodeBranchTarget(insn, p);
                if (t < funcStart || t > funcStart + 0x400) { target = t; break; }
            }
            else if ((insn & 0xFC000000) == 0x94000000)
            {
                target = DecodeBranchTarget(insn, p);
            }
        }
        if (!target) { LOGE("FinishDestroy: no super call in %p", (void *)funcStart); return 0; }

        ElfScanner ue = GetUnrealELF();
        for (uintptr_t p = target; p < target + 0x80; p += 4)
        {
            uint32_t a = vm_rpm_ptr<uint32_t>((void *)p);
            if ((a & 0x9F000000) != 0x90000000) continue;  // ADRP
            uint32_t rd = a & 0x1F;
            int64_t imm = (int64_t)((((a >> 5) & 0x7FFFF) << 2) | ((a >> 29) & 0x3));
            imm = (imm << 43) >> 43;
            imm <<= 12;
            uintptr_t page = (p & ~uintptr_t(0xFFF)) + (uintptr_t)imm;
            for (uintptr_t q = p + 4; q < p + 0x20; q += 4)
            {
                uint32_t l = vm_rpm_ptr<uint32_t>((void *)q);
                if ((l & 0xFFC00000) == 0xF9400000 && ((l >> 5) & 0x1F) == rd)  // LDR Xt,[Xd,#imm12]
                {
                    uintptr_t arr = vm_rpm_ptr<uintptr_t>((void *)(page + (uintptr_t)((l >> 10) & 0xFFF) * 8));
                    if (arr >= ue.base() && arr < ue.end() && IsObjectArray(arr))
                    {
                        LOGI("FinishDestroy: GUObjectArray = <Base> + 0x%lX", (unsigned long)(arr - ue.base()));
                        cached = arr;
                        return arr;
                    }
                }
            }
        }
        LOGE("FinishDestroy: no global load in %p", (void *)target);
        return 0;
    }

    // FName::ToString is the first call in the function that formats "%s__Direction_%s".
    uintptr_t FindFNameToString() const
    {
        static uintptr_t cached = 0;
        static bool searched = false;
        if (searched) return cached;
        searched = true;

        static const char *kDirPat =
            "25 00 73 00 5F 00 5F 00 44 00 69 00 72 00 65 00 63 00 74 00 69 00 6F 00 6E 00 5F 00 25 00 73 00 00 00";
        uintptr_t strAddr = findIdaPattern(PATTERN_MAP_TYPE::ANY_R, kDirPat, 0);
        if (!strAddr) { LOGE("FName::ToString: \"%%s__Direction_%%s\" not found"); return 0; }

        uintptr_t xref = FindAdrpXrefToAddr(strAddr);
        if (!xref) { LOGE("FName::ToString: no ADRP reference to %p", (void *)strAddr); return 0; }

        uintptr_t funcStart = 0;
        for (int i = 0; i < 0x200; i++)
        {
            uintptr_t p = xref - (uintptr_t)i * 4;
            if ((vm_rpm_ptr<uint32_t>((void *)p) & 0xFF8003FF) == 0xD10003FF) { funcStart = p; break; }  // SUB SP, SP, #imm
        }
        if (!funcStart) return 0;

        ElfScanner ue = GetUnrealELF();
        for (uintptr_t p = funcStart; p < funcStart + 0x100; p += 4)
        {
            uint32_t insn = vm_rpm_ptr<uint32_t>((void *)p);
            if ((insn & 0xFC000000) == 0x94000000)  // BL
            {
                uintptr_t t = DecodeBranchTarget(insn, p);
                if (t >= ue.base() && t < ue.end())
                {
                    LOGI("FName::ToString = <Base> + 0x%lX", (unsigned long)(t - ue.base()));
                    cached = t;
                    return t;
                }
            }
        }
        LOGE("FName::ToString: no call in %p", (void *)funcStart);
        return 0;
    }

    virtual bool IsFNamePassedByValue() const
    {
        return false;
    }

    // Calls the engine's FName::ToString, so it resolves names only when the
    // dumper runs inside the game process.
    std::string GetNameByID_ViaToString(int32_t id) const
    {
        if (id < 0 || kMgr.processID() != getpid()) return "";

        uintptr_t ToString = FindFNameToString();
        if (!ToString) return "";

        struct FStr { char16_t *Data; int32_t Num; int32_t Max; };
        FStr out{};
        char buf[1024];
        size_t len = 0;

        // ToString indexes the name pool unchecked, and the prober passes ids read
        // from candidate offsets, so a fault inside it answers "not a name".
        InstallFaultHandler();
        if (sigsetjmp(FaultJump(), 1) != 0) return "";
        InCall() = true;
        if (IsFNamePassedByValue())
            out = ((FStr(*)(uint64_t))ToString)((uint64_t)(uint32_t)id);
        else
        {
            struct FNameKey { int32_t Comparison; int32_t Number; } key{id, 0};
            out = ((FStr(*)(const FNameKey *))ToString)(&key);
        }
        for (; out.Data && len + 1 < size_t(out.Num) && len < sizeof(buf) && out.Data[len]; len++)
            buf[len] = static_cast<char>(out.Data[len]);
        InCall() = false;
        return std::string(buf, len);
    }

    std::string GetNameByID_ViaPlainAnsi(int32_t id, uintptr_t function) const
    {
        if (id < 0 || !function || kMgr.processID() != getpid()) return {};
        char buf[1024]{};
        InstallFaultHandler();
        if (sigsetjmp(FaultJump(), 1) != 0) return {};
        InCall() = true;
        reinterpret_cast<void (*)(const int32_t *, char *)>(function)(&id, buf);
        InCall() = false;
        return std::string(buf, strnlen(buf, sizeof(buf)));
    }

private:
    static void InstallFaultHandler()
    {
        static std::atomic<bool> installed{false};
        if (!installed.exchange(true))
        {
            struct sigaction sa{};
            sa.sa_sigaction = OnFault;
            sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGSEGV, &sa, &PreviousHandler(SIGSEGV));
            sigaction(SIGBUS, &sa, &PreviousHandler(SIGBUS));
        }
    }
    static sigjmp_buf &FaultJump() { static thread_local sigjmp_buf jump; return jump; }
    static bool &InCall() { static thread_local bool inCall = false; return inCall; }
    static struct sigaction &PreviousHandler(int sig)
    {
        static struct sigaction segv{}, bus{};
        return sig == SIGSEGV ? segv : bus;
    }

    static void OnFault(int sig, siginfo_t *info, void *ctx)
    {
        if (InCall())
        {
            InCall() = false;
            siglongjmp(FaultJump(), 1);
        }
        const struct sigaction &prev = PreviousHandler(sig);
        if (prev.sa_flags & SA_SIGINFO)
            prev.sa_sigaction(sig, info, ctx);
        else if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN)
            prev.sa_handler(sig);
        else
        {
            signal(sig, SIG_DFL);
            raise(sig);
        }
    }
};

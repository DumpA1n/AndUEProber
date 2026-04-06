#pragma once

#include "UE/UEGameProfile.hpp"
using namespace UEMemory;

class NiZhanProfile : public IGameProfile
{
public:
    NiZhanProfile() = default;

    bool ArchSupprted() const override
    {
        auto e_machine = GetUnrealELF().header().e_machine;
        return e_machine == EM_AARCH64 || e_machine == EM_ARM;
    }

    std::string GetAppName() const override
    {
        return "NiZhan";
    }

    std::vector<std::string> GetAppIDs() const override
    {
        return {"com.tencent.tmgp.nz"};
    }

    bool isUsingCasePreservingName() const override
    {
        return false;
    }

    bool IsUsingFNamePool() const override
    {
        return true;
    }

    bool isUsingOutlineNumberName() const override
    {
        return false;
    }

    uintptr_t GetGUObjectArrayPtr() const override
    {
        uintptr_t guobjectarray = GetUnrealELF().findSymbol("GUObjectArray");
        if (guobjectarray != 0)
            return guobjectarray;

        std::vector<std::pair<std::string, int>> idaPatterns = {
            {"91 E1 03 ? AA E0 03 08 AA E2 03 1F 2A", -7},
            {"B4 21 0C 40 B9 ? ? ? ? ? ? ? 91", 5},
            {"9F E5 00 ? 00 E3 FF ? 40 E3 ? ? A0 E1", -2},
            {"96 df 02 17 ? ? ? ? 54 ? ? ? ? ? ? ? 91 e1 03 13 aa", 9},
            {"f4 03 01 2a ? 00 00 34 ? ? ? ? ? ? ? ? ? ? 00 54 ? 00 00 14 ? ? ? ? ? ? ? 91", 0x18},
            {"69 3e 40 b9 1f 01 09 6b ? ? ? 54 e1 03 13 aa ? ? ? ? f4 4f ? a9 ? ? ? ? ? ? ? 91", 0x18},
        };

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        for (const auto &it : idaPatterns)
        {
            std::string ida_pattern = it.first;
            const int step = it.second;

            uintptr_t adrl = Arm64::DecodeADRL(findIdaPattern(map_type, ida_pattern, step));
            if (adrl != 0) return adrl;
        }

        return 0;
    }

    uintptr_t GetNamesPtr() const override
    {
        return GetUnrealELF().base();
    }

    UE_Offsets *GetOffsets() const override
    {
        static UE_Offsets offsets = UE_DefaultOffsets::UE4_25_27(isUsingCasePreservingName());
        return &offsets;
    }

    // Decode ARM64 BL instruction at given address, return absolute target
    static uintptr_t DecodeBL(uintptr_t bl_address)
    {
        if (!bl_address) return 0;
        uint32_t insn = *reinterpret_cast<uint32_t*>(bl_address);
        // BL: opcode[31:26] == 100101
        if ((insn & 0xFC000000) != 0x94000000) return 0;
        // imm26, sign-extended, shifted left 2
        int32_t imm26 = insn & 0x03FFFFFF;
        if (imm26 & 0x02000000) imm26 |= (int32_t)0xFC000000; // sign extend
        return bl_address + (int64_t)imm26 * 4;
    }

    uintptr_t FindGetPlainANSIString() const
    {
        // Locate the BL to GetPlainANSIString in a JNI bridge that converts FName→Java String via "<init>".
        // Register allocation (X19/X20) varies across builds, so anchors use wildcards:
        //   STR X0,[Xn,#8]  → ? 06 00 F9    (Xn = X19 or X20)
        //   LDR X8,[Xm]     → ? 02 40 F9    (Xm = X19 or X20)
        //   MOV X0,Xn       → E0 03 ? AA    (Xn = X19 or X20)
        //   MOV X1,X0       → E1 03 00 AA   (fixed)
        //   LDR X8,[X8,#0x108] → 08 85 40 F9 (fixed)
        //   MOV X3,X21      → E3 03 15 AA   (fixed)
        //   BLR X8          → 00 01 3F D6   (fixed)
        std::vector<std::pair<std::string, int>> idaPatterns = {
            // P1: BL target → ADD → BL → STR [Xn,#8] → LDR [Xm] → ADRP → MOV X1,X0 → ADD → LDR [X8,#0x108]
            {"? ? ? 94 ? ? ? 91 ? ? ? 94 ? 06 00 F9 ? 02 40 F9 ? ? ? ? E1 03 00 AA ? ? ? 91 08 85 40 F9", 0},
            // P2: pre-BL + post: BL(1st) → MOV X{19/20},X0 → <2 insns> → BL target → ADD → BL → STR [Xn,#8]
            {"? ? ? 94 ? 03 00 AA ? ? ? ? ? ? ? ? ? ? ? 94 ? ? ? 91 ? ? ? 94 ? 06 00 F9", 0x10},
            // P3: STR → LDR → ADRP → MOV X1,X0 → ADD → LDR [X8,#0x108] → MOV X0,Xn
            {"? 06 00 F9 ? 02 40 F9 ? ? ? ? E1 03 00 AA ? ? ? 91 08 85 40 F9 E0 03 ? AA", -0xC},
            // P4: MOV X1,X0 → ADD → LDR [X8,#0x108] → MOV X0,Xn → MOV X3,X21 → BLR X8
            {"E1 03 00 AA ? ? ? 91 08 85 40 F9 E0 03 ? AA E3 03 15 AA 00 01 3F D6", -0x18},
        };

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        for (const auto &it : idaPatterns)
        {
            uintptr_t bl_addr = findIdaPattern(map_type, it.first, it.second);
            uintptr_t target = DecodeBL(bl_addr);
            if (target != 0) return target;
        }

        return 0;
    }

    std::string GetNameByID(int32_t id) const override
    {
        using GetPlainANSIString_t = void (*)(const int32_t*, char*);

        static uintptr_t funcAddr = FindGetPlainANSIString();
        if (!funcAddr) return "";

        char buf[1024]{};
        ((GetPlainANSIString_t)funcAddr)(&id, buf);

        return std::string(buf);
    }
};

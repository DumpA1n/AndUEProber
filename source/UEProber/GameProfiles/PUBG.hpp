#pragma once

#include "UE/UEGameProfile.hpp"
using namespace UEMemory;

class PUBGProfile : public IGameProfile
{
public:
    PUBGProfile() = default;

    bool ArchSupprted() const override
    {
        auto e_machine = GetUnrealELF().header().e_machine;
        return e_machine == EM_AARCH64;
    }

    std::string GetAppName() const override
    {
        return "PUBG";
    }

    std::vector<std::string> GetAppIDs() const override
    {
        return {
            "com.tencent.ig",
            "com.rekoo.pubgm",
            "com.pubg.imobile",
            "com.pubg.krmobile",
            "com.vng.pubgmobile",
        };
    }

    bool isUsingCasePreservingName() const override
    {
        return false;
    }

    bool IsUsingFNamePool() const override
    {
        return false;
    }

    bool isUsingOutlineNumberName() const override
    {
        return false;
    }

    uintptr_t GetGUObjectArrayPtr() const override
    {
        std::string ida_pattern = "12 40 B9 ? 3E 40 B9 ? ? ? 6B ? ? ? 54 ? ? ? ? ? ? ? 91";
        const int step = 0xf;

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        return Arm64::DecodeADRL(findIdaPattern(map_type, ida_pattern, step));
    }

    uintptr_t GetNamesPtr() const override
    {
        return GetUnrealELF().base();
    }

    UE_Offsets *GetOffsets() const override
    {
        static UE_Offsets offsets = UE_DefaultOffsets::UE4_18_19(isUsingCasePreservingName());

        static bool once = false;
        if (!once)
        {
            once = true;
            offsets.FNameEntry.Index = sizeof(void *);
            offsets.FNameEntry.Name = sizeof(void *) + sizeof(int32_t);
        }

        return &offsets;
    }

    static uintptr_t DecodeBL(uintptr_t bl_address)
    {
        if (!bl_address) return 0;
        uint32_t insn = *reinterpret_cast<uint32_t*>(bl_address);
        if ((insn & 0xFC000000) != 0x94000000) return 0;
        int32_t imm26 = insn & 0x03FFFFFF;
        if (imm26 & 0x02000000) imm26 |= (int32_t)0xFC000000;
        return bl_address + (int64_t)imm26 * 4;
    }

    uintptr_t FindGetPlainANSIString() const
    {
        std::vector<std::pair<std::string, int>> idaPatterns = {
            {"? ? ? 94 ? ? ? 91 ? ? ? 94 ? 06 00 F9 ? 02 40 F9 ? ? ? ? E1 03 00 AA ? ? ? 91 08 85 40 F9", 0},
            {"? ? ? 94 ? 03 00 AA ? ? ? ? ? ? ? ? ? ? ? 94 ? ? ? 91 ? ? ? 94 ? 06 00 F9", 0x10},
            {"? 06 00 F9 ? 02 40 F9 ? ? ? ? E1 03 00 AA ? ? ? 91 08 85 40 F9 E0 03 ? AA", -0xC},
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
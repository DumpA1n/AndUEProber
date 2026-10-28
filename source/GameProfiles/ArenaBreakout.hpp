#pragma once

#include "IGameProfileAndroid.hpp"

class ArenaBreakoutProfile : public IGameProfileAndroid
{
public:
    ArenaBreakoutProfile() = default;

    std::string GetAppName() const override
    {
        return "Arena Breakout";
    }

    std::vector<std::string> GetAppIDs() const override
    {
        return {"com.tencent.mf.uam"};
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
        if (uintptr_t a = FindGUObjectArrayViaFinishDestroy())
            return a;

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
            uintptr_t adrl = Arm64::DecodeADRL(findIdaPattern(map_type, it.first, it.second));
            if (IsObjectArray(adrl)) return adrl;
        }

        return 0;
    }

    uintptr_t GetNamesPtr() const override
    {
        const uintptr_t function = FindGetPlainANSIString();
        for (uintptr_t address = function; function && address < function + 0x80; address += 4)
        {
            const uint32_t adrp = vm_rpm_ptr<uint32_t>(reinterpret_cast<void *>(address));
            if ((adrp & 0x9F000000) != 0x90000000) continue;
            const uint32_t reg = adrp & 31;
            for (uintptr_t next = address + 4; next < address + 0x20; next += 4)
            {
                const uint32_t add = vm_rpm_ptr<uint32_t>(reinterpret_cast<void *>(next));
                if ((add & 0xFFC00000) != 0x91000000 || ((add >> 5) & 31) != reg || (add & 31) != reg) continue;
                const uintptr_t pool = Arm64::DecodeADRL(address, static_cast<uint32_t>(next - address));
                const uintptr_t block = vm_rpm_ptr<uintptr_t>(reinterpret_cast<void *>(pool + 0x48));
                uint16_t header = 0;
                char name[4]{};
                if (pool && block && vm_rpm_ptr(reinterpret_cast<void *>(block), &header, sizeof(header)) &&
                    header >> 6 == 4 && vm_rpm_ptr(reinterpret_cast<void *>(block + 2), name, sizeof(name)) &&
                    name[0] == 'N' && name[1] == 'o' && name[2] == 'n' && name[3] == 'e')
                {
                    LOGI("Arena Breakout FNamePool = <Base> + 0x%lX", (unsigned long)(pool - GetUnrealELF().base()));
                    return pool;
                }
            }
        }
        LOGE("Arena Breakout FNamePool not found or not initialized");
        return 0;
    }

    UE_Offsets *GetOffsets() const override
    {
        static UE_Offsets offsets = UE_DefaultOffsets::UE4_25_27(isUsingCasePreservingName());
        offsets.FNamePool.BlocksOff = 0x48;
        return &offsets;
    }

    uintptr_t FindGetPlainANSIString() const
    {
        const std::vector<std::pair<std::string, int>> patterns = {
            {"? ? ? 94 ? ? ? 91 ? ? ? 94 ? 06 00 F9 ? 02 40 F9 ? ? ? ? E1 03 00 AA ? ? ? 91 08 85 40 F9", 0},
            {"? ? ? 94 ? 03 00 AA ? ? ? ? ? ? ? ? ? ? ? 94 ? ? ? 91 ? ? ? 94 ? 06 00 F9", 0x10},
            {"? 06 00 F9 ? 02 40 F9 ? ? ? ? E1 03 00 AA ? ? ? 91 08 85 40 F9 E0 03 ? AA", -0xC},
            {"E1 03 00 AA ? ? ? 91 08 85 40 F9 E0 03 ? AA E3 03 15 AA 00 01 3F D6", -0x18},
            {"E1 03 00 AA ? ? ? 91 E0 03 ? AA E3 03 15 AA ? ? ? ? 00 01 3F D6", -0x18},
        };
        const auto map = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;
        for (const auto &[pattern, offset] : patterns)
        {
            const uintptr_t address = findIdaPattern(map, pattern, offset);
            if (!address) continue;
            const uint32_t instruction = vm_rpm_ptr<uint32_t>(reinterpret_cast<void *>(address));
            if ((instruction & 0xFC000000) != 0x94000000) continue;
            const uintptr_t target = DecodeBranchTarget(instruction, address);
            LOGI("Arena Breakout GetPlainANSIString = <Base> + 0x%lX", (unsigned long)(target - GetUnrealELF().base()));
            return target;
        }
        LOGE("Arena Breakout GetPlainANSIString not found");
        return 0;
    }

    std::string GetNameByID(int32_t id) const override
    {
        static const uintptr_t function = FindGetPlainANSIString();
        return GetNameByID_ViaPlainAnsi(id, function);
    }
};

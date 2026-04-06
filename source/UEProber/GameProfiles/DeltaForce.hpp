#pragma once

#include <cstdint>
#include <string>
#include "UE/UEGameProfile.hpp"
using namespace UEMemory;

class DeltaForceProfile : public IGameProfile
{
public:
    DeltaForceProfile() = default;

    bool ArchSupprted() const override
    {
        auto e_machine = GetUnrealELF().header().e_machine;
        return e_machine == EM_AARCH64;
    }

    std::string GetAppName() const override
    {
        return "Delta Force";
    }

    std::vector<std::string> GetAppIDs() const override
    {
        return {"com.proxima.dfm", "com.garena.game.df", "com.tencent.tmgp.dfm"};
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
        // TODO: replace GUObjectArray offset
        return GetUnrealELF().base() + 0x1A36A748;

        // std::vector<std::pair<std::string, int>> idaPatterns = {
        //     {"91 E1 03 ? AA E0 03 08 AA E2 03 1F 2A", -7},
        //     {"B4 21 0C 40 B9 ? ? ? ? ? ? ? 91", 5},
        //     {"9F E5 00 ? 00 E3 FF ? 40 E3 ? ? A0 E1", -2},
        //     {"96 df 02 17 ? ? ? ? 54 ? ? ? ? ? ? ? 91 e1 03 13 aa", 9},
        //     {"f4 03 01 2a ? 00 00 34 ? ? ? ? ? ? ? ? ? ? 00 54 ? 00 00 14 ? ? ? ? ? ? ? 91", 0x18},
        //     {"69 3e 40 b9 1f 01 09 6b ? ? ? 54 e1 03 13 aa ? ? ? ? f4 4f ? a9 ? ? ? ? ? ? ? 91", 0x18},
        // };

        // PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;

        // for (const auto &it : idaPatterns)
        // {
        //     std::string ida_pattern = it.first;
        //     const int step = it.second;

        //     uintptr_t adrl = Arm64::DecodeADRL(findIdaPattern(map_type, ida_pattern, step));
        //     if (adrl != 0) return adrl;
        // }

        // return 0;
    }

    uintptr_t GetNamesPtr() const override
    {
        return GetUnrealELF().base();
    }

    UE_Offsets *GetOffsets() const override
    {
        static UE_Offsets offsets = UE_DefaultOffsets::UE4_25_27(isUsingCasePreservingName());
        offsets.TUObjectArray.NumElements = sizeof(int32_t);
        offsets.TUObjectArray.Objects = offsets.TUObjectArray.NumElements + (sizeof(int32_t) * 3);
        return &offsets;
    }

    // TODO: implement proper GetNameByID
    std::string GetNameByID(int32_t id) const override
    {
		// using GetPlainANSIString_t = void (*)(const int32_t*, char*);

		// char buf[1024];
		// ((GetPlainANSIString_t)(GetUnrealELF().base() + 0xFFA2810))(&id, buf);

        // return std::string(buf);

		char buf[1024] = {0};
		uint32_t v10 = id;

        uint16_t *v16 = (uint16_t *)(*(uint64_t *)(GetUnrealELF().base() + 0x1A343A00 + ((v10 >> 15) & 0x1FFF8) + 56) + 2 * (v10 & 0x3FFFFLL));
        // uint16_t *v16 = (uint16_t *)(*((uint64_t *)(GetUnrealELF().base() + 0x1A343A00) + (v10 >> 18) + 7) + 2 * (v10 & 0x3FFFF));

        uint32_t len = *v16 >> 6;
		memcpy(buf, v16 + 1, len > 1024 ? 1024 : len);
		((void(*)(char*, uint32_t))(GetUnrealELF().base() + 0xD6C40FC))(buf, len);
		// *(buf + len) = 0;
		return std::string(buf);
    }
};

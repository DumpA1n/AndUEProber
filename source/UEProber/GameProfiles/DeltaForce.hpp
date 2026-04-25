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
        return GetUnrealELF().base() + 0x1B8B5788;

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
            if (adrl != 0 && kPtrValidator.isPtrReadable(adrl)) return adrl;
        }

        return 0;
    }

    uintptr_t GetNamesPtr() const override
    {
        return FindNamePoolDataPtr();
    }

    UE_Offsets *GetOffsets() const override
    {
        static UE_Offsets offsets = UE_DefaultOffsets::UE4_25_27(isUsingCasePreservingName());
        static bool once = false;
        if (!once)
        {
            once = true;
            offsets.FNamePool.BlocksBit = 18;
            offsets.FNamePool.BlocksOff -= sizeof(void *);
            offsets.TUObjectArray.NumElements = sizeof(int32_t);
            offsets.TUObjectArray.Objects = offsets.TUObjectArray.NumElements + (sizeof(int32_t) * 3);
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
            if (target != 0 && kPtrValidator.isPtrExecutable(target, sizeof(uint32_t))) return target;
        }

        return 0;
    }

    uintptr_t FindNamePoolDataPtr() const
    {
        static uintptr_t cached = 0;
        if (cached != 0) return cached;

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;
        std::vector<std::pair<std::string, int>> idaPatterns = {
            {"C8 ? ? D0 08 ? ? 91 ? ? ? 39 ? ? ? 36 C9 7E 52 D3 E0 E3 03 91 03 80 80 52 FB E3 03 91 08 0D 09 8B C9 46 7F D3 08 1D 40 F9", 0},
            {"C9 7E 52 D3 E0 E3 03 91 03 80 80 52 FB E3 03 91 08 0D 09 8B C9 46 7F D3 08 1D 40 F9 01 01 09 8B 28 24 40 78 16 FD 46 D3 E2 03 16 AA", -0x10},
            {"03 80 80 52 FB E3 03 91 08 0D 09 8B C9 46 7F D3 08 1D 40 F9 01 01 09 8B 28 24 40 78 16 FD 46 D3 E2 03 16 AA", -0x18},
        };

        for (const auto &it : idaPatterns)
        {
            uintptr_t candidate = Arm64::DecodeADRL(findIdaPattern(map_type, it.first, it.second));
            if (IsValidNamePoolData(candidate))
            {
                cached = candidate;
                return cached;
            }
        }

        cached = FindNamePoolDataFromWrapper(FindGetPlainANSIString());
        return cached;
    }

    uintptr_t FindDecryptFName() const
    {
        static uintptr_t cached = 0;
        if (cached != 0) return cached;

        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;
        std::vector<std::pair<std::string, int>> idaPatterns = {
            {"? ? ? ? E0 E3 03 91 E1 03 ? 2A ? ? ? ? E0 E3 03 91 ? ? ? 38", 0xC},
            {"E0 E3 03 91 E1 03 ? 2A ? ? ? ? E0 E3 03 91 ? ? ? 38 ? ? ? ? ? 06 00 F9 E1 03 00 AA", 0x8},
            {"28 C7 91 52 68 1C A7 72 28 7C A8 9B 08 FD 61 D3 08 0D 08 0B 29 00 08 4B 08 00 40 39 3F 0D 00 71", 0},
        };

        for (const auto &it : idaPatterns)
        {
            uintptr_t address = findIdaPattern(map_type, it.first, it.second);
            uintptr_t target = it.second == 0 ? address : DecodeBL(address);
            if (target != 0 && kPtrValidator.isPtrExecutable(target, sizeof(uint32_t)))
            {
                cached = target;
                return cached;
            }
        }

        cached = FindDecryptFromWrapper(FindGetPlainANSIString());
        return cached;
    }

protected:
    uint8_t *GetNameEntry(int32_t id) const override
    {
        if (id < 0) return nullptr;

        uintptr_t namesPtr = GetNamesPtr();
        if (!IsValidNamePoolData(namesPtr)) return nullptr;

        UE_Offsets *offsets = GetOffsets();
        uintptr_t blockBit = offsets->FNamePool.BlocksBit;
        uintptr_t blocksOff = offsets->FNamePool.BlocksOff;
        uintptr_t chunkMask = (uintptr_t(1) << blockBit) - 1;
        uintptr_t blockOffset = (static_cast<uintptr_t>(id) >> blockBit) * sizeof(void *);
        uintptr_t chunkOffset = (static_cast<uintptr_t>(id) & chunkMask) * offsets->FNamePool.Stride;

        uint8_t *chunk = vm_rpm_ptr<uint8_t *>((void *)(namesPtr + blocksOff + blockOffset));
        if (!kPtrValidator.isPtrReadable(chunk, sizeof(uint16_t))) return nullptr;

        return chunk + chunkOffset;
    }

    std::string GetNameEntryString(uint8_t *entry) const override
    {
        std::string name = IGameProfile::GetNameEntryString(entry);
        if (name.empty()) return "";

        uintptr_t decrypt = FindDecryptFName();
        if (decrypt != 0)
        {
            using DecryptFName_t = void (*)(char *, uint32_t);
            ((DecryptFName_t)decrypt)(name.data(), static_cast<uint32_t>(name.length()));
        }
        else
        {
            DecryptInline(name.data(), static_cast<uint32_t>(name.length()));
        }

        return name;
    }

private:
    static void DecryptInline(char *str, uint32_t len)
    {
        if (!str || !*str || len == 0) return;

        uint32_t key = 0;
        switch (len % 9)
        {
        case 0u:
            key = ((len & 0x1F) + len);
            break;
        case 1u:
            key = ((len ^ 0xDF) + len);
            break;
        case 2u:
            key = ((len | 0xCF) + len);
            break;
        case 3u:
            key = (33 * len);
            break;
        case 4u:
            key = (len + (len >> 2));
            break;
        case 5u:
            key = (3 * len + 5);
            break;
        case 6u:
            key = (((4 * len) | 5) + len);
            break;
        case 7u:
            key = (((len >> 4) | 7) + len);
            break;
        case 8u:
            key = ((len ^ 0xC) + len);
            break;
        default:
            key = ((len ^ 0x40) + len);
            break;
        }

        for (uint32_t i = 0; i < len; i++)
        {
            str[i] = (key & 0x80) ^ ~str[i];
        }
    }

    static bool IsAddX0Sp(uint32_t insn)
    {
        return (insn & 0xFF0003FF) == 0x910003E0;
    }

    static bool IsMovW1FromReg(uint32_t insn)
    {
        return (insn & 0xFFE0FFFF) == 0x2A0003E1;
    }

    bool IsValidNamePoolData(uintptr_t candidate) const
    {
        if (!kPtrValidator.isPtrReadable(candidate, sizeof(uintptr_t))) return false;

        uintptr_t blocksPtrAddr = candidate + GetOffsets()->FNamePool.BlocksOff;
        if (!kPtrValidator.isPtrReadable(blocksPtrAddr, sizeof(uintptr_t))) return false;

        uint8_t *chunk = vm_rpm_ptr<uint8_t *>((void *)blocksPtrAddr);
        return kPtrValidator.isPtrReadable(chunk, sizeof(uint16_t));
    }

    uintptr_t FindNamePoolDataFromWrapper(uintptr_t wrapper) const
    {
        if (!kPtrValidator.isPtrExecutable(wrapper, sizeof(uint32_t))) return 0;

        for (uintptr_t cursor = wrapper; cursor < wrapper + 0x180; cursor += sizeof(uint32_t))
        {
            uintptr_t candidate = Arm64::DecodeADRL(cursor);
            if (IsValidNamePoolData(candidate)) return candidate;
        }

        return 0;
    }

    uintptr_t FindDecryptFromWrapper(uintptr_t wrapper) const
    {
        if (!kPtrValidator.isPtrExecutable(wrapper, sizeof(uint32_t))) return 0;

        for (uintptr_t cursor = wrapper; cursor < wrapper + 0x180; cursor += sizeof(uint32_t))
        {
            uint32_t insn0 = vm_rpm_ptr<uint32_t>((void *)(cursor));
            uint32_t insn1 = vm_rpm_ptr<uint32_t>((void *)(cursor + 0x4));
            uint32_t insn2 = vm_rpm_ptr<uint32_t>((void *)(cursor + 0x8));
            uint32_t insn3 = vm_rpm_ptr<uint32_t>((void *)(cursor + 0xC));
            uint32_t insn4 = vm_rpm_ptr<uint32_t>((void *)(cursor + 0x10));

            if ((insn0 & 0xFC000000) != 0x94000000) continue;
            if (!IsAddX0Sp(insn1)) continue;
            if (!IsMovW1FromReg(insn2)) continue;
            if ((insn3 & 0xFC000000) != 0x94000000) continue;
            if (!IsAddX0Sp(insn4)) continue;

            uintptr_t target = DecodeBL(cursor + 0xC);
            if (target != 0 && kPtrValidator.isPtrExecutable(target, sizeof(uint32_t)))
                return target;
        }

        return 0;
    }
};

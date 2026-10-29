#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "IGameProfileAndroid.hpp"

class DeltaForceProfile : public IGameProfileAndroid
{
public:
    DeltaForceProfile() = default;

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
        if (uintptr_t a = FindGUObjectArrayViaFinishDestroy())
            return a;

        std::vector<std::pair<std::string, int>> idaPatterns = {
            {"? 00 A0 52 ? 00 A0 52 ? ? ? 1A ? ? ? 1B ? ? ? ? ? ? ? 91", 0x10},
            {"68 22 40 39 ? ? ? 34 ? ? ? ? ? ? ? 91 E1 03 13 AA 7F 22 00 39", 0x8},
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
        PATTERN_MAP_TYPE map_type = isEmulator() ? PATTERN_MAP_TYPE::ANY_R : PATTERN_MAP_TYPE::ANY_X;
        return Arm64::DecodeADRL(findIdaPattern(map_type, "91 ? 10 81 52 ? ? 21 8B", -7));
    }

    UE_Offsets *GetOffsets() const override
    {
        struct FChunkedFixedUObjectArray // = ObjObjects (reordered)
        {
            struct FUObjectItem;    // 0x18 item, Object@0 (only ever pointed-to)
            int32_t MaxChunks;      // 0x00  (=7)
            int32_t NumElements;    // 0x04  objects in use
            int32_t NumChunks;      // 0x08  (=2 live)
            int32_t _pad;           // 0x0C
            FUObjectItem** Objects; // 0x10  chunk table (separate alloc)
            uint8_t _lock[0x10];    // 0x18  ObjObjectsCritical / PreAllocatedObjects (0)
            int32_t MaxElements;    // 0x28 (abs 0x38)  458752 (= MaxChunks × PerChunk)
        };
        struct FUObjectArray
        {
            typedef FChunkedFixedUObjectArray TUObjectArray;
            uint8_t _gc[0x10];         // 0x00  ObjFirst/LastGCIndex, MaxObjectsNotConsideredByGC, OpenForDisregardForGC
            TUObjectArray ObjObjects;  // 0x10
        };

        static UE_Offsets offsets = UE_DefaultOffsets::UE4_25_27(isUsingCasePreservingName());
        static bool once = false;
        if (!once)
        {
            once = true;
            offsets.FNamePool.BlocksBit = 18;
            offsets.FNamePool.BlocksOff -= sizeof(void *);
            offsets.FUObjectArray.ObjObjects = offsetof(FUObjectArray, ObjObjects);
            offsets.TUObjectArray.Objects = offsetof(FChunkedFixedUObjectArray, Objects);
            offsets.TUObjectArray.NumElements = offsetof(FChunkedFixedUObjectArray, NumElements);
            offsets.TUObjectArray.MaxElements = offsetof(FChunkedFixedUObjectArray, MaxElements);
            offsets.TUObjectArray.MaxChunks = offsetof(FChunkedFixedUObjectArray, MaxChunks);
            offsets.TUObjectArray.NumChunks = offsetof(FChunkedFixedUObjectArray, NumChunks);
            // FUObjectItem keeps Flags at 0x8 but its SerialNumber at 0x14: the engine's
            // FWeakObjectPtr::Get/IsValid and AllocateSerialNumber all use item + 0x14.
            offsets.FUObjectItem.SerialNumber = 0x14;
            offsets.UObject.ClassPrivate = sizeof(void *);
            offsets.UObject.OuterPrivate = offsets.UObject.ClassPrivate + sizeof(void *);
            offsets.UObject.ObjectFlags = offsets.UObject.OuterPrivate + sizeof(void *);
            offsets.UObject.NamePrivate = offsets.UObject.ObjectFlags + sizeof(int32_t);
            offsets.UObject.InternalIndex = offsets.UObject.NamePrivate + offsets.FName.Size;
            offsets.UStruct.PropertiesSize = offsets.UField.Next + (sizeof(void *) * 2) + sizeof(int32_t);
            offsets.UStruct.SuperStruct = offsets.UStruct.PropertiesSize + sizeof(int32_t);
            offsets.UStruct.MinAlignment = offsets.UStruct.SuperStruct + sizeof(void *);
            offsets.UStruct.Children = offsets.UStruct.SuperStruct + (sizeof(void *) * 2);
            offsets.UStruct.ChildProperties = offsets.UStruct.Children + (sizeof(void *) * 3);
            // The engine's own IsA indexes the struct base chain at SuperStruct - 0x10.
            offsets.Config.isUsingStructBaseChain = true;
            offsets.UFunction.NumParams = offsets.UStruct.ChildProperties +
                ((sizeof(void *) + sizeof(int32_t) * 2) * 2) + (sizeof(void *) * 5);
            offsets.UFunction.ParamSize = offsets.UFunction.NumParams + sizeof(int16_t);
            offsets.UFunction.EFunctionFlags = offsets.UFunction.ParamSize + sizeof(int16_t) + sizeof(int32_t);
            offsets.UFunction.Func = offsets.UFunction.EFunctionFlags + (sizeof(int32_t) * 2) + (sizeof(void *) * 3);
            // Owner is a 16-byte FFieldVariant placed directly after the vtable.
            offsets.FField.Owner = sizeof(void *);
            offsets.FField.Next = offsets.FField.Owner + (sizeof(void *) * 2);
            offsets.FField.ClassPrivate = offsets.FField.Next + sizeof(void *);
            offsets.FField.NamePrivate = offsets.FField.ClassPrivate + sizeof(void *);
            offsets.FField.FlagsPrivate = offsets.FField.NamePrivate + offsets.FName.Size;
            offsets.FProperty.ArrayDim = offsets.FField.NamePrivate + GetPtrAlignedOf(offsets.FName.Size) + sizeof(void *);
            offsets.FProperty.ElementSize = offsets.FProperty.ArrayDim + sizeof(int32_t);
            offsets.FProperty.PropertyFlags = offsets.FProperty.ElementSize + sizeof(int32_t);
            offsets.FProperty.Offset_Internal = offsets.FProperty.PropertyFlags + sizeof(int64_t) + sizeof(int32_t);
            offsets.FProperty.Size = offsets.FProperty.Offset_Internal + (sizeof(int32_t) * 3) + (sizeof(void *) * 4);
            // ITextData's vtable leads with the two destructors and OwnsLocalizedString.
            offsets.FTextData.GetDisplayString = 3;
        }
        return &offsets;
    }

protected:
    std::string GetNameByID(int32_t id) const override
    {
        if (id < 0) return {};
        const auto *offsets = GetOffsets();
        const auto names = GetUEVars()->GetNamesPtr();
        const auto blockIndex = uint32_t(id) >> offsets->FNamePool.BlocksBit;
        uintptr_t block = 0;
        if (!vm_rpm_ptr((void *)(names + offsets->FNamePool.BlocksOff + blockIndex * sizeof(void *)), &block, sizeof(block)) || !block)
            return {};
        const auto mask = (uint32_t(1) << offsets->FNamePool.BlocksBit) - 1;
        return GetNameEntryString((uint8_t *)(block + (uint32_t(id) & mask) * offsets->FNamePool.Stride));
    }

    // Entries are stored as the bitwise complement of the name with bit 7
    // flipped by a key derived from the length.
    std::string GetNameEntryString(uint8_t *entry) const override
    {
        if (!entry) return {};
        const auto *offsets = GetOffsets();
        uint16_t header = 0;
        if (!vm_rpm_ptr(entry + offsets->FNamePoolEntry.Header, &header, sizeof(header)) ||
            offsets->FNamePoolEntry.GetIsWide(header)) return {};
        const uint32_t length = offsets->FNamePoolEntry.GetLength(header);
        if (!length || length > 1024) return {};
        std::string name(length, '\0');
        if (!vm_rpm_ptr(entry + offsets->FNamePoolEntry.Header + sizeof(header), name.data(), name.size()))
            return {};
        uint32_t key = 0;
        switch (length % 9)
        {
        case 0: key = (length & 0x1f) + length; break;
        case 1: key = (length ^ 0xdf) + length; break;
        case 2: key = (length | 0xcf) + length; break;
        case 3: key = 33 * length; break;
        case 4: key = length + (length >> 2); break;
        case 5: key = 3 * length + 5; break;
        case 6: key = ((4 * length) | 5) + length; break;
        case 7: key = ((length >> 4) | 7) + length; break;
        case 8: key = (length ^ 0x0c) + length; break;
        }
        for (auto &c : name)
            c = static_cast<char>((key & 0x80) ^ ~static_cast<unsigned char>(c));
        return name;
    }
};

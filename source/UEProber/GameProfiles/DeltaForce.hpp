#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <limits>
#include <span>
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

    andueprober::Status DiscoverObjectArray(andueprober::MemoryReader& reader,
        const andueprober::ModuleImage& module, andueprober::ReadBudget& budget,
        andueprober::DiscoveryValue& value) const override
    {
        auto status = FindGUObjectArrayViaFinishDestroy(reader, module, budget, value);
        if (status) status = ValidateObjectArrayCandidate(reader, module, budget, value);
        if (status || status.code != andueprober::Error::InvalidEvidence) return status;

        const std::array firstPattern{
            std::byte{0}, std::byte{0x00}, std::byte{0xa0}, std::byte{0x52},
            std::byte{0}, std::byte{0x00}, std::byte{0xa0}, std::byte{0x52},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x1a},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x1b},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x91}};
        const std::array firstMask{
            std::byte{0}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
            std::byte{0}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0xff},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0xff},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0xff}};
        status = andueprober::findAddressFromAarch64Pattern(
            reader, module, firstPattern, firstMask, 16, budget, value);
        if (status) status = ValidateObjectArrayCandidate(reader, module, budget, value);
        if (status || status.code != andueprober::Error::InvalidEvidence) return status;

        const std::array secondPattern{
            std::byte{0x68}, std::byte{0x22}, std::byte{0x40}, std::byte{0x39},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x34},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x91},
            std::byte{0xe1}, std::byte{0x03}, std::byte{0x13}, std::byte{0xaa},
            std::byte{0x7f}, std::byte{0x22}, std::byte{0x00}, std::byte{0x39}};
        const std::array secondMask{
            std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0xff},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
            std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0xff},
            std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
            std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}};
        status = andueprober::findAddressFromAarch64Pattern(
            reader, module, secondPattern, secondMask, 8, budget, value);
        if (!status) return status;
        return ValidateObjectArrayCandidate(reader, module, budget, value);
    }

    andueprober::Status DiscoverNamePool(andueprober::MemoryReader& reader,
        const andueprober::ModuleImage& module, andueprober::ReadBudget& budget,
        andueprober::DiscoveryValue& value) const override
    {
        const std::array pattern{std::byte{0x91}, std::byte{0}, std::byte{0x10}, std::byte{0x81},
            std::byte{0x52}, std::byte{0}, std::byte{0}, std::byte{0x21}, std::byte{0x8b}};
        const std::array mask{std::byte{0xff}, std::byte{0}, std::byte{0xff}, std::byte{0xff},
            std::byte{0xff}, std::byte{0}, std::byte{0}, std::byte{0xff}, std::byte{0xff}};
        return andueprober::findAddressFromAarch64Pattern(reader, module, pattern, mask, -7, budget, value);
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
            offsets.FNamePool.BlocksBit = 18; // Custom names require an explicit resolver contract.
            offsets.FNamePool.BlocksOff -= sizeof(void *); // Custom names require an explicit resolver contract.
            offsets.FUObjectArray.ObjObjects = offsetof(FUObjectArray, ObjObjects);
            offsets.TUObjectArray.Objects = offsetof(FChunkedFixedUObjectArray, Objects);
            offsets.TUObjectArray.NumElements = offsetof(FChunkedFixedUObjectArray, NumElements);
            offsets.TUObjectArray.MaxElements = offsetof(FChunkedFixedUObjectArray, MaxElements);
            offsets.TUObjectArray.MaxChunks = offsetof(FChunkedFixedUObjectArray, MaxChunks);
            offsets.TUObjectArray.NumChunks = offsetof(FChunkedFixedUObjectArray, NumChunks);
            offsets.UObject.ClassPrivate = sizeof(void *);
            offsets.UObject.OuterPrivate = offsets.UObject.ClassPrivate + sizeof(void *);
            offsets.UObject.ObjectFlags = offsets.UObject.OuterPrivate + sizeof(void *);
            offsets.UObject.NamePrivate = offsets.UObject.ObjectFlags + sizeof(int32_t);
            offsets.UObject.InternalIndex = offsets.UObject.NamePrivate + offsets.FName.Size;
            offsets.UStruct.PropertiesSize = offsets.UField.Next + (sizeof(void *) * 2) + sizeof(int32_t);
            offsets.UStruct.SuperStruct = offsets.UStruct.PropertiesSize + sizeof(int32_t);
            offsets.UStruct.Children = offsets.UStruct.SuperStruct + (sizeof(void *) * 2);
            offsets.UStruct.ChildProperties = offsets.UStruct.Children + (sizeof(void *) * 3);
            // The engine's own IsA indexes the struct base chain at SuperStruct - 0x10;
            // RunFullSdkDump corroborates the array against every class before emitting it.
            offsets.Config.isUsingStructBaseChain = true;
            offsets.UFunction.NumParams = offsets.UStruct.ChildProperties +
                ((sizeof(void *) + sizeof(int32_t) * 2) * 2) + (sizeof(void *) * 5);
            offsets.UFunction.ParamSize = offsets.UFunction.NumParams + sizeof(int16_t);
            offsets.UFunction.EFunctionFlags = offsets.UFunction.ParamSize + sizeof(int16_t) + sizeof(int32_t);
            offsets.UFunction.Func = offsets.UFunction.EFunctionFlags +
                (sizeof(int32_t) * 2) + (sizeof(void *) * 3);
            // Owner is a 16-byte FFieldVariant placed directly after the vtable,
            // so the rest of the chain starts at Owner + 2 pointers.
            offsets.FField.Owner = sizeof(void *);
            offsets.FField.Next = offsets.FField.Owner + (sizeof(void *) * 2);
            offsets.FField.ClassPrivate = offsets.FField.Next + sizeof(void *);
            offsets.FField.NamePrivate = offsets.FField.ClassPrivate + sizeof(void *);
            offsets.FField.FlagsPrivate = offsets.FField.NamePrivate + offsets.FName.Size;
            offsets.FProperty.ArrayDim = offsets.FField.NamePrivate +
                UEMemory::GetPtrAlignedOf(offsets.FName.Size) + sizeof(void *);
            offsets.FProperty.ElementSize = offsets.FProperty.ArrayDim + sizeof(int32_t);
            offsets.FProperty.PropertyFlags = offsets.FProperty.ElementSize + sizeof(int32_t);
            offsets.FProperty.Offset_Internal = offsets.FProperty.PropertyFlags + sizeof(int64_t) + sizeof(int32_t);
            offsets.FProperty.Size = offsets.FProperty.Offset_Internal +
                (sizeof(int32_t) * 3) + (sizeof(void *) * 4);
        }
        return &offsets;
    }

private:
    template<class T>
    static andueprober::Status ReadValue(andueprober::MemoryReader& reader, std::uintptr_t address,
        andueprober::ReadBudget& budget, T& value)
    {
        return andueprober::readExact(reader, address,
            std::as_writable_bytes(std::span(&value, std::size_t{1})), budget);
    }

    andueprober::Status ValidateObjectArrayCandidate(andueprober::MemoryReader& reader,
        const andueprober::ModuleImage& module, andueprober::ReadBudget& budget,
        andueprober::DiscoveryValue& value) const
    {
        using andueprober::Error;
        if (!value.address) return {Error::InvalidEvidence, "Delta Force object-array discovery produced no address"};
        const auto* offsets = GetOffsets();
        if (!offsets || offsets->FUObjectArray.ObjObjects > 4096 || offsets->TUObjectArray.Objects > 4096 ||
            offsets->TUObjectArray.NumElements > 4096 || offsets->TUObjectArray.MaxElements > 4096 ||
            offsets->TUObjectArray.NumChunks > 4096 || offsets->TUObjectArray.MaxChunks > 4096 ||
            offsets->FUObjectItem.Size < sizeof(void*) || offsets->FUObjectItem.Size > 256)
            return {Error::InvalidEvidence, "Delta Force object-array layout is outside bounded limits"};
        bool insideModule = false;
        for (const auto& range : module.ranges)
            if (range.readable && *value.address >= range.start && *value.address - range.start < range.size) {
                insideModule = true;
                break;
            }
        if (!insideModule || offsets->FUObjectArray.ObjObjects >
            std::numeric_limits<std::uintptr_t>::max() - *value.address)
            return {Error::InvalidEvidence, "Delta Force object-array candidate is outside the selected module"};
        const auto objectsArray = *value.address + offsets->FUObjectArray.ObjObjects;
        std::int32_t maxChunks = 0, count = 0, chunks = 0, capacity = 0;
        std::uintptr_t chunkTable = 0;
        if (auto status = ReadValue(reader, objectsArray + offsets->TUObjectArray.MaxChunks, budget, maxChunks); !status)
            return status;
        if (auto status = ReadValue(reader, objectsArray + offsets->TUObjectArray.NumElements, budget, count); !status)
            return status;
        if (auto status = ReadValue(reader, objectsArray + offsets->TUObjectArray.NumChunks, budget, chunks); !status)
            return status;
        if (auto status = ReadValue(reader, objectsArray + offsets->TUObjectArray.MaxElements, budget, capacity); !status)
            return status;
        if (auto status = ReadValue(reader, objectsArray + offsets->TUObjectArray.Objects, budget, chunkTable); !status)
            return status;
        constexpr std::int32_t maximumChunks = 4096;
        constexpr std::int32_t maximumObjects = 64 * 1024 * 1024;
        if (maxChunks <= 0 || maxChunks > maximumChunks || chunks <= 0 || chunks > maxChunks ||
            count <= 1 || capacity < count || capacity > maximumObjects || !chunkTable)
            return {Error::InvalidEvidence, "Delta Force object-array candidate has incoherent bounds"};
        std::uintptr_t firstChunk = 0;
        auto status = ReadValue(reader, chunkTable, budget, firstChunk);
        if (!status) return status;
        if (!firstChunk || offsets->FUObjectItem.Size >
            std::numeric_limits<std::uintptr_t>::max() - firstChunk || offsets->FUObjectItem.Object >
            std::numeric_limits<std::uintptr_t>::max() - firstChunk - offsets->FUObjectItem.Size)
            return {Error::InvalidEvidence, "Delta Force object-array candidate has no first chunk"};
        std::uintptr_t object = 0;
        status = ReadValue(reader, firstChunk + offsets->FUObjectItem.Size + offsets->FUObjectItem.Object,
            budget, object);
        if (!status) return status;
        std::uintptr_t vtable = 0;
        if (!object || !(status = ReadValue(reader, object, budget, vtable)) || !vtable)
            return status ? andueprober::Status{Error::InvalidEvidence,
                "Delta Force object-array candidate has no readable indexed object"} : status;
        value.evidence.sampleIdentities.push_back("validated-object-count:" + std::to_string(count));
        value.evidence.sampleIdentities.push_back("validated-object-capacity:" + std::to_string(capacity));
        value.evidence.samples = value.evidence.sampleIdentities.size();
        return {};
    }

protected:
    std::string GetNameByID(std::int32_t id) const override
    {
        if (id < 0) return {};
        const auto* offsets = GetOffsets();
        const auto names = GetNamesPtr();
        if (!names || offsets->FNamePool.BlocksBit == 0 || offsets->FNamePool.BlocksBit >= 31) return {};
        const auto unsignedId = static_cast<std::uint32_t>(id);
        const auto blockIndex = unsignedId >> offsets->FNamePool.BlocksBit;
        std::uintptr_t block = 0;
        if (!UEMemory::vm_rpm_ptr(reinterpret_cast<const void*>(names + offsets->FNamePool.BlocksOff +
            blockIndex * sizeof(void*)), &block, sizeof(block)) || !block) return {};
        const auto mask = (std::uint32_t{1} << offsets->FNamePool.BlocksBit) - 1;
        return GetNameEntryString(reinterpret_cast<std::uint8_t*>(
            block + (unsignedId & mask) * offsets->FNamePool.Stride));
    }

    std::string GetNameEntryString(uint8_t *entry) const override
    {
        if (!entry) return {};
        const auto* offsets = GetOffsets();
        std::uint16_t header = 0;
        if (!UEMemory::vm_rpm_ptr(entry + offsets->FNamePoolEntry.Header, &header, sizeof(header)) ||
            offsets->FNamePoolEntry.GetIsWide(header)) return {};
        const auto nameLength = offsets->FNamePoolEntry.GetLength(header);
        if (!nameLength || nameLength > 1024) return {};
        std::string name(nameLength, '\0');
        if (!UEMemory::vm_rpm_ptr(entry + offsets->FNamePoolEntry.Header + sizeof(header),
            name.data(), name.size())) return {};
        const auto length = static_cast<std::uint32_t>(name.size());
        std::uint32_t key = 0;
        switch (length % 9) {
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
        for (auto& character : name)
            character = static_cast<char>((key & 0x80) ^ ~static_cast<unsigned char>(character));
        return name;
    }

};

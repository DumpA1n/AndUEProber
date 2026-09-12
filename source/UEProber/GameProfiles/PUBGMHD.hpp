#pragma once

#include <cstddef>
#include <cstdint>

#include "IGameProfileAndroid.hpp"

class PUBGMHDProfile : public IGameProfileAndroid
{
public:
    PUBGMHDProfile() = default;

    std::string GetAppName() const override
    {
        return "PUBGMHD";
    }

    std::vector<std::string> GetAppIDs() const override
    {
        return {"com.tencent.tmgp.pubgmhd"};
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

    UE_Offsets *GetOffsets() const override
    {
        struct FFixedUObjectArray   // = ObjObjects (single inline chunk, flat-read)
        {
            struct FUObjectItem;    // 0x18 item, Object@0 (only ever pointed-to)
            int32_t NumElements;    // 0x00 (abs 0xB8)  objects in use (use this, not +0x04 = Num-1)
            int32_t _lastIndex;     // 0x04 (abs 0xBC)  == NumElements-1
            int32_t MaxElements;    // 0x08 (abs 0xC0)  single-chunk capacity (~400000)
            int32_t _pad;           // 0x0C (abs 0xC4)
            FUObjectItem* Objects;  // 0x10 (abs 0xC8)  inline chunk[0] -> FUObjectItem[] (stride 0x18, Object@0)
        };
        struct FUObjectArray
        {
            typedef FFixedUObjectArray TUObjectArray;
            uint8_t _front[0xB8];     // 0x00  reordered: GC indices + listener TArrays + locks
            TUObjectArray ObjObjects; // 0xB8
        };

        static UE_Offsets offsets = UE_DefaultOffsets::UE4_18_19(isUsingCasePreservingName());
        static bool once = false;
        if (!once)
        {
            once = true;
            offsets.Config.IsUsingFNamePool = true; // Custom names require an explicit resolver contract.
            offsets.FUObjectArray.ObjObjects = offsetof(FUObjectArray, ObjObjects);
            offsets.TUObjectArray.Objects = offsetof(FFixedUObjectArray, Objects);
            offsets.TUObjectArray.NumElements = offsetof(FFixedUObjectArray, NumElements);
            offsets.TUObjectArray.NumElementsPerChunk = 0;  // flat read of the single inline chunk
        }
        return &offsets;
    }

};

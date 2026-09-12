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
        }
        return &offsets;
    }

};

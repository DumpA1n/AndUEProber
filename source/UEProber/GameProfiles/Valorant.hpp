#pragma once

#include "IGameProfileAndroid.hpp"

class ValorantProfile : public IGameProfileAndroid
{
public:
    ValorantProfile() = default;

    std::string GetAppName() const override
    {
        return "Valorant";
    }

    std::vector<std::string> GetAppIDs() const override
    {
        return {"com.tencent.tmgp.codev"};
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
        static UE_Offsets offsets = UE_DefaultOffsets::UE4_25_27(isUsingCasePreservingName());
        return &offsets;
    }

};

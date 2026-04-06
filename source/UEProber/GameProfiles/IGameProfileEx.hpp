#pragma once

#include <cstdint>
#include <string>

#include "UE/UEGameProfile.hpp"
#include "UE/UEOffsets.hpp"

// IGameProfileEx — polymorphic interface for Ex profiles.
// Provides public access to protected IGameProfile methods + runtime offset override.

class IGameProfileEx
{
public:
    virtual ~IGameProfileEx() = default;

    virtual uintptr_t PublicGetGUObjectArrayPtr() const = 0;
    virtual std::string PublicGetNameByID(int32_t id) const = 0;

    virtual void SetProbedOffsets(const UE_Offsets& offsets) = 0;
    virtual bool HasProbedOffsets() const = 0;

    // Get the underlying IGameProfile* (for Init/Dump/GetAppIDs etc.)
    virtual IGameProfile* AsGameProfile() = 0;
};

// GameProfileEx<T> — generic wrapper for any IGameProfile subclass.
// Inherits from T (concrete profile) + IGameProfileEx (polymorphic interface).

template<typename TProfile>
class GameProfileEx : public TProfile, public IGameProfileEx
{
    UE_Offsets m_ProbedOffsets{};
    bool m_HasProbedOffsets = false;

public:
    GameProfileEx() = default;

    // IGameProfileEx
    uintptr_t PublicGetGUObjectArrayPtr() const override { return this->GetGUObjectArrayPtr(); }
    std::string PublicGetNameByID(int32_t id) const override { return this->GetNameByID(id); }

    void SetProbedOffsets(const UE_Offsets& offsets) override
    {
        m_ProbedOffsets = offsets;
        m_HasProbedOffsets = true;
    }

    bool HasProbedOffsets() const override { return m_HasProbedOffsets; }

    IGameProfile* AsGameProfile() override { return static_cast<IGameProfile*>(this); }

    // Override GetOffsets: return probed offsets when available
    UE_Offsets* GetOffsets() const override
    {
        if (m_HasProbedOffsets)
            return const_cast<UE_Offsets*>(&m_ProbedOffsets);

        return TProfile::GetOffsets();
    }
};

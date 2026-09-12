#pragma once
#include <type_traits>
#include "IGameProfileAndroid.hpp"

class IGameProfileEx {
public:
    virtual ~IGameProfileEx() = default;
    virtual bool SupportsBoundedDiscovery() const = 0;
    virtual andueprober::Status DiscoverObjectArray(andueprober::MemoryReader&, const andueprober::ModuleImage&,
        andueprober::ReadBudget&, andueprober::DiscoveryValue&) const = 0;
    virtual andueprober::Status DiscoverNamePool(andueprober::MemoryReader&, const andueprober::ModuleImage&,
        andueprober::ReadBudget&, andueprober::DiscoveryValue&) const = 0;
    virtual void BindRuntime(std::uintptr_t objectArray, std::uintptr_t namePool) = 0;
    virtual void SetProbedOffsets(const UE_Offsets&) = 0;
    virtual std::string ResolveName(std::int32_t) const = 0;
    virtual IGameProfile* AsGameProfile() = 0;
};
template <typename TProfile>
class GameProfileEx : public TProfile, public IGameProfileEx {
public:
    bool SupportsBoundedDiscovery() const override { return true; }
    andueprober::Status DiscoverObjectArray(andueprober::MemoryReader& reader,
        const andueprober::ModuleImage& module, andueprober::ReadBudget& budget,
        andueprober::DiscoveryValue& value) const override {
        if constexpr (std::is_base_of_v<IGameProfileAndroid, TProfile>)
            return TProfile::DiscoverObjectArray(reader, module, budget, value);
        return andueprober::findObjectArrayCandidate(reader, module, budget, value);
    }
    andueprober::Status DiscoverNamePool(andueprober::MemoryReader& reader,
        const andueprober::ModuleImage& module, andueprober::ReadBudget& budget,
        andueprober::DiscoveryValue& value) const override {
        if constexpr (std::is_base_of_v<IGameProfileAndroid, TProfile>)
            return TProfile::DiscoverNamePool(reader, module, budget, value);
        return andueprober::findNamePoolCandidate(reader, module, budget, value);
    }
    void BindRuntime(std::uintptr_t objectArray, std::uintptr_t namePool) override {
        objectArray_ = objectArray;
        namePool_ = namePool;
    }
    void SetProbedOffsets(const UE_Offsets& offsets) override {
        offsets_ = offsets;
        hasOffsets_ = true;
    }
    std::string ResolveName(std::int32_t id) const override { return TProfile::GetNameByID(id); }
    bool ArchSupprted() const override { return sizeof(std::uintptr_t) == 8; }
    UE_Offsets* GetOffsets() const override {
        return hasOffsets_ ? const_cast<UE_Offsets*>(&offsets_) : TProfile::GetOffsets();
    }
    IGameProfile* AsGameProfile() override { return static_cast<IGameProfile*>(this); }
    std::uintptr_t GetGUObjectArrayPtr() const override { return objectArray_; }
    std::uintptr_t GetNamesPtr() const override { return namePool_; }
    std::string GetNameByID(std::int32_t id) const override {
        return TProfile::GetNameByID(id);
    }
private:
    std::uintptr_t objectArray_ = 0, namePool_ = 0;
    mutable UE_Offsets offsets_{};
    bool hasOffsets_ = false;
};

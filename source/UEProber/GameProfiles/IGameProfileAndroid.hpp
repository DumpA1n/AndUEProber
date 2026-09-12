#pragma once
#include "andueprober/Discovery.hpp"
#include "UE/UEGameProfile.hpp"

// Discovery receives an operation-owned module lease, reader and budget explicitly.
// The upstream parameterless interface cannot supply these contracts and is disabled.
class IGameProfileAndroid : public IGameProfile {
public:
    virtual andueprober::Status DiscoverObjectArray(andueprober::MemoryReader& reader,
        const andueprober::ModuleImage& module, andueprober::ReadBudget& budget,
        andueprober::DiscoveryValue& value) const {
        return FindGUObjectArrayViaFinishDestroy(reader, module, budget, value);
    }
    virtual andueprober::Status DiscoverNamePool(andueprober::MemoryReader& reader,
        const andueprober::ModuleImage& module, andueprober::ReadBudget& budget,
        andueprober::DiscoveryValue& value) const {
        return andueprober::findNamePoolCandidate(reader, module, budget, value);
    }
    andueprober::Status FindAdrpXrefToAddr(andueprober::MemoryReader& reader,
        const andueprober::ModuleImage& module, uintptr_t target,
        andueprober::ReadBudget& budget, andueprober::DiscoveryValue& value) const {
        return andueprober::findAdrpReference(reader, module, target, budget, value);
    }
    andueprober::Status FindGUObjectArrayViaFinishDestroy(andueprober::MemoryReader& reader,
        const andueprober::ModuleImage& module, andueprober::ReadBudget& budget,
        andueprober::DiscoveryValue& value) const {
        return andueprober::findObjectArrayCandidate(reader, module, budget, value);
    }
    andueprober::Status FindFNameToString(andueprober::MemoryReader& reader,
        const andueprober::ModuleImage& module, andueprober::ReadBudget& budget,
        andueprober::DiscoveryValue& value) const {
        return andueprober::findNameToStringCandidate(reader, module, budget, value);
    }
    bool ArchSupprted() const override { return false; }
    uintptr_t GetGUObjectArrayPtr() const override { return 0; }
    uintptr_t GetNamesPtr() const override { return 0; }
    std::string GetNameByID(int32_t id) const override { return IGameProfile::GetNameByID(id); }
};

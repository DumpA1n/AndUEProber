#pragma once
#include <type_traits>
#include "IGameProfileAndroid.hpp"

class IGameProfileEx {
public:
    virtual ~IGameProfileEx() = default;
    virtual bool SupportsBoundedDiscovery() const = 0;
    virtual andueprober::Status DiscoverObjectArray(andueprober::MemoryReader&, const andueprober::ModuleImage&,
        andueprober::ReadBudget&, andueprober::DiscoveryValue&) const = 0;
    virtual IGameProfile* AsGameProfile() = 0;
};
template <typename TProfile>
class GameProfileEx : public TProfile, public IGameProfileEx {
public:
    bool SupportsBoundedDiscovery() const override { return std::is_base_of_v<IGameProfileAndroid, TProfile>; }
    andueprober::Status DiscoverObjectArray(andueprober::MemoryReader& reader,
        const andueprober::ModuleImage& module, andueprober::ReadBudget& budget,
        andueprober::DiscoveryValue& value) const override {
        if constexpr (std::is_base_of_v<IGameProfileAndroid, TProfile>)
            return this->FindGUObjectArrayViaFinishDestroy(reader, module, budget, value);
        value = {};
        return {andueprober::Error::Unsupported, "The profile has no bounded discovery provider"};
    }
    IGameProfile* AsGameProfile() override { return static_cast<IGameProfile*>(this); }
};

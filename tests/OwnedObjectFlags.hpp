#pragma once
#include "OwnedRelations.hpp"
#include "andueprober/ObjectFlags.hpp"

struct OwnedObjectFlags {
    OwnedRelations memory;
    bool zeroOffset = false;
    static constexpr std::array<std::uint32_t, 4> flags{0x20u, 0u, 0x80000004u, 0xffffffffu};
    explicit OwnedObjectFlags(bool zero = false) : zeroOffset(zero) {
        for (std::uint32_t i = 0; i < 4; ++i) {
            memory.put<std::uint32_t>(2048 + i * 64 + (zero ? 4 : 0), i);
            memory.put<std::uint32_t>(2048 + i * 64 + (zero ? 0 : 4), flags[i]);
        }
    }
    andueprober::ObjectFlagProbeProfile profile(andueprober::Layout layout = andueprober::Layout::FField) const {
        return {"owned-object-flags-v1", "owned-object-flags-image", memory.epoch, layout, 64};
    }
    std::array<andueprober::ObjectFlagSample, 3> samples() const {
        std::array<andueprober::ObjectFlagSample, 3> result;
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = {memory.object(i + 1), "owned-object:" + std::to_string(i + 1), flags[i + 1]};
        return result;
    }
    andueprober::Snapshot initial(andueprober::Layout layout = andueprober::Layout::FField) {
        andueprober::Snapshot snapshot;
        snapshot.sessionId = "owned-object-flags"; snapshot.moduleIdentity = "owned-object-flags-image";
        snapshot.generation = memory.epoch; snapshot.layout = layout;
        andueprober::ReadBudget budget; budget.generation = memory.epoch;
        if (!observeOwnedRelations(memory, memory, budget, snapshot)) std::abort();
        return snapshot;
    }
};

#include "andueprober/Evidence.hpp"
#include "Core/Budget.hpp"

#include <map>
#include <string_view>

namespace andueprober {
namespace {
// Closure text is bounded by the caller's own limit and a violation is evidence
// failure, not a caller mistake: the offsets come from an observed snapshot.
void text(Budget& budget, const EvidenceLimits& limits, const std::string& value) {
    budget.text(value, limits.maximumTextBytes, Error::InvalidEvidence);
}
void proof(Budget& budget, const EvidenceLimits& limits, const std::string& name, const Offset& offset) {
    text(budget, limits, name);
    budget.charge(1, sizeof(Offset));
    budget.item(offset.dependencies.size());
    budget.charge(offset.dependencies.size(), sizeof(std::pair<std::string, std::uint64_t>));
    budget.charge(offset.evidence.size(), sizeof(Evidence));
    if (!offset.value || !offset.version || offset.validation != Validation::Validated || offset.evidence.empty())
        fail(Error::InvalidEvidence, "Evidence closure requires validated values and observation evidence");
    for (const auto& [dependency, version] : offset.dependencies) {
        (void)version;
        text(budget, limits, dependency);
    }
    for (const auto& evidence : offset.evidence) {
        text(budget, limits, evidence.check); text(budget, limits, evidence.source);
        budget.charge(evidence.relativeAddresses.size(), sizeof(std::uintptr_t));
        budget.charge(evidence.sampleIdentities.size(), sizeof(std::string));
        if (!evidence.passed || !evidence.samples || evidence.samples != evidence.sampleIdentities.size())
            fail(Error::InvalidEvidence, "Evidence closure requires matching named observation samples");
        for (const auto& identity : evidence.sampleIdentities) text(budget, limits, identity);
    }
}
} // namespace

Status validateEvidenceClosure(const Snapshot& snapshot, std::span<const std::string> roots,
    const EvidenceLimits& limits) {
    return guard([&]() -> Status {
        if (!limits.maximumNodes || !limits.maximumMetadataBytes || !limits.maximumTextBytes || roots.empty())
            fail(Error::InvalidArgument, "Evidence closure requires nonzero limits and at least one root");
        Budget budget{"Evidence closure validation", limits.cancelled, limits.deadline,
            limits.maximumMetadataBytes, limits.maximumDependencies};
        budget.check();
        if (roots.size() > limits.maximumNodes) fail(Error::BudgetExceeded, "Evidence root count exceeds its node budget");
        budget.charge(roots.size(), sizeof(std::string_view));
        if (snapshot.schemaVersion != 1 || snapshot.state == TaskState::Failed || snapshot.state == TaskState::Cancelled ||
            !snapshot.result || (snapshot.layout != Layout::UProperty && snapshot.layout != Layout::FField))
            fail(Error::InvalidEvidence, "Evidence closure requires a usable analysis identity and state");
        text(budget, limits, snapshot.sessionId); text(budget, limits, snapshot.moduleIdentity);
        text(budget, limits, snapshot.layoutIdentity);
        struct Frame { const Offset* offset; decltype(Offset{}.dependencies)::const_iterator next; unsigned* state; };
        std::map<std::string_view, unsigned> visited;
        std::vector<Frame> stack;
        const auto enter = [&](const auto& found) {
            budget.check();
            if (visited.size() >= limits.maximumNodes) fail(Error::BudgetExceeded, "Evidence closure node budget exceeded");
            proof(budget, limits, found->first, found->second);
            auto [state, inserted] = visited.emplace(found->first, 1);
            (void)inserted;
            stack.push_back({&found->second, found->second.dependencies.begin(), &state->second});
        };
        for (const auto& root : roots) {
            text(budget, limits, root);
            const auto found = snapshot.offsets.find(root);
            if (found == snapshot.offsets.end()) fail(Error::InvalidEvidence, "Evidence closure root is missing");
            if (visited.contains(found->first)) continue;
            enter(found);
            while (!stack.empty()) {
                budget.check();
                auto& frame = stack.back();
                if (frame.next == frame.offset->dependencies.end()) {
                    *frame.state = 2; stack.pop_back(); continue;
                }
                const auto& [name, version] = *frame.next++;
                const auto dependency = snapshot.offsets.find(name);
                if (dependency == snapshot.offsets.end() || dependency->second.version != version)
                    fail(Error::InvalidEvidence, "Evidence closure contains a missing or stale dependency");
                const auto state = visited.find(dependency->first);
                if (state != visited.end()) {
                    if (state->second == 1) fail(Error::InvalidEvidence, "Evidence closure contains a cycle");
                    continue;
                }
                enter(dependency);
            }
        }
        budget.check();
        return {};
    });
}

} // namespace andueprober

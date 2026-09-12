#include "andueprober/Evidence.hpp"

#include <map>
#include <string_view>

namespace andueprober {
namespace {
struct Failure { Error code; const char* message; };
[[noreturn]] void fail(Error code, const char* message) { throw Failure{code, message}; }
struct Budget {
    const EvidenceLimits& limits;
    std::size_t bytes;
    std::size_t dependencies;
    void check() const {
        if (limits.cancelled && limits.cancelled->load()) fail(Error::Cancelled, "Evidence closure validation cancelled");
        if (std::chrono::steady_clock::now() >= limits.deadline)
            fail(Error::DeadlineExceeded, "Evidence closure validation deadline exceeded");
    }
    void charge(std::size_t count, std::size_t width = 1) {
        check();
        if (count > bytes / width) fail(Error::BudgetExceeded, "Evidence closure metadata budget exceeded");
        bytes -= count * width;
    }
    void text(const std::string& value) {
        charge(value.size());
        if (value.empty() || value.size() > limits.maximumTextBytes || value.find('\0') != std::string::npos || !validateUtf8(value))
            fail(Error::InvalidEvidence, "Evidence closure requires bounded nonempty UTF-8 text");
        check();
    }
    void proof(const std::string& name, const Offset& offset) {
        text(name);
        charge(1, sizeof(Offset));
        if (offset.dependencies.size() > dependencies)
            fail(Error::BudgetExceeded, "Evidence closure dependency budget exceeded");
        dependencies -= offset.dependencies.size();
        charge(offset.dependencies.size(), sizeof(std::pair<std::string, std::uint64_t>));
        charge(offset.evidence.size(), sizeof(Evidence));
        if (!offset.value || !offset.version || offset.validation != Validation::Validated || offset.evidence.empty())
            fail(Error::InvalidEvidence, "Evidence closure requires validated values and observation evidence");
        for (const auto& [dependency, version] : offset.dependencies) {
            (void)version;
            text(dependency);
        }
        for (const auto& evidence : offset.evidence) {
            text(evidence.check); text(evidence.source);
            charge(evidence.relativeAddresses.size(), sizeof(std::uintptr_t));
            charge(evidence.sampleIdentities.size(), sizeof(std::string));
            if (!evidence.passed || !evidence.samples || evidence.samples != evidence.sampleIdentities.size())
                fail(Error::InvalidEvidence, "Evidence closure requires matching named observation samples");
            for (const auto& identity : evidence.sampleIdentities) text(identity);
        }
    }
};
} // namespace

Status validateEvidenceClosure(const Snapshot& snapshot, std::span<const std::string> roots,
    const EvidenceLimits& limits) {
    try {
        if (!limits.maximumNodes || !limits.maximumMetadataBytes || !limits.maximumTextBytes || roots.empty())
            fail(Error::InvalidArgument, "Evidence closure requires nonzero limits and at least one root");
        Budget budget{limits, limits.maximumMetadataBytes, limits.maximumDependencies};
        budget.check();
        if (roots.size() > limits.maximumNodes) fail(Error::BudgetExceeded, "Evidence root count exceeds its node budget");
        budget.charge(roots.size(), sizeof(std::string_view));
        if (snapshot.schemaVersion != 1 || snapshot.state == TaskState::Failed || snapshot.state == TaskState::Cancelled ||
            !snapshot.result || (snapshot.layout != Layout::UProperty && snapshot.layout != Layout::FField))
            fail(Error::InvalidEvidence, "Evidence closure requires a usable analysis identity and state");
        budget.text(snapshot.sessionId); budget.text(snapshot.moduleIdentity); budget.text(snapshot.layoutIdentity);
        struct Frame { const Offset* offset; decltype(Offset{}.dependencies)::const_iterator next; unsigned* state; };
        std::map<std::string_view, unsigned> visited;
        std::vector<Frame> stack;
        const auto enter = [&](const auto& found) {
            budget.check();
            if (visited.size() >= limits.maximumNodes) fail(Error::BudgetExceeded, "Evidence closure node budget exceeded");
            budget.proof(found->first, found->second);
            auto [state, inserted] = visited.emplace(found->first, 1);
            (void)inserted;
            stack.push_back({&found->second, found->second.dependencies.begin(), &state->second});
        };
        for (const auto& root : roots) {
            budget.text(root);
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
    } catch (const Failure& error) {
        try { return {error.code, error.message}; } catch (...) { return {error.code, {}}; }
    } catch (...) { return {Error::Internal, {}}; }
}

} // namespace andueprober

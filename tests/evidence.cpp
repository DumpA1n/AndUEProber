#include <andueprober/Evidence.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <new>

static std::size_t allocationsUntilFailure = SIZE_MAX;
void* operator new(std::size_t size) {
    if (allocationsUntilFailure == 0) throw std::bad_alloc();
    if (allocationsUntilFailure != SIZE_MAX) --allocationsUntilFailure;
    if (auto* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

using namespace andueprober;
namespace {
unsigned checks{};
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
Snapshot graph(std::size_t count) {
    Snapshot snapshot;
    snapshot.sessionId = "owned-evidence-graph";
    snapshot.moduleIdentity = "owned-metadata";
    snapshot.layoutIdentity = "explicit-field-graph";
    snapshot.layout = Layout::FField;
    snapshot.generation = 1;
    for (std::size_t index = 0; index < count; ++index) {
        Offset value;
        value.value = 0;
        value.version = 1;
        value.validation = Validation::Validated;
        value.evidence.push_back({"owned declared observation", true, 2, {0}, "owned fixture metadata", {"first", "second"}});
        if (index + 1 != count) value.dependencies["node" + std::to_string(index + 1)] = 1;
        snapshot.offsets["node" + std::to_string(index)] = std::move(value);
    }
    return snapshot;
}
const std::array<std::string, 1> roots{"node0"};
void validation() {
    auto snapshot = graph(8);
    CHECK(validateEvidenceClosure(snapshot, roots));
    for (unsigned mode = 0; mode < 13; ++mode) {
        auto invalid = snapshot;
        auto& leaf = invalid.offsets.at("node7");
        switch (mode) {
        case 0: leaf.value.reset(); break;
        case 1: leaf.version = 2; break;
        case 2: leaf.validation = Validation::Candidate; break;
        case 3: leaf.validation = Validation::Stale; break;
        case 4: leaf.evidence.clear(); break;
        case 5: leaf.evidence[0].source.clear(); break;
        case 6: leaf.evidence[0].check = std::string("\xc0\xaf", 2); break;
        case 7: leaf.evidence[0].sampleIdentities[0] = std::string("a\0b", 3); break;
        case 8: leaf.evidence[0].sampleIdentities.pop_back(); break;
        case 9: leaf.evidence[0].passed = false; break;
        case 10: leaf.dependencies["node0"] = 1; break;
        case 11: leaf.dependencies["node7"] = 1; break;
        case 12: leaf.dependencies["missing"] = 1; break;
        }
        CHECK(validateEvidenceClosure(invalid, roots).code == Error::InvalidEvidence);
    }
    auto unrelated = snapshot.offsets.at("node7");
    unrelated.validation = Validation::Stale;
    unrelated.evidence.clear();
    unrelated.dependencies["unrelated"] = 1;
    snapshot.offsets["unrelated"] = unrelated;
    CHECK(validateEvidenceClosure(snapshot, roots));
    CHECK(snapshot.offsets.at("unrelated").validation == Validation::Stale);
    std::array<std::string, 2> sharedRoots{"node0", "node2"};
    CHECK(validateEvidenceClosure(snapshot, sharedRoots));
    CHECK(validateEvidenceClosure(snapshot, std::span<const std::string>{}).code == Error::InvalidArgument);
    std::array<std::string, 1> absent{"missing"};
    CHECK(validateEvidenceClosure(snapshot, absent).code == Error::InvalidEvidence);
    snapshot.state = TaskState::Failed;
    CHECK(validateEvidenceClosure(snapshot, roots).code == Error::InvalidEvidence);
}
void bounds() {
    auto snapshot = graph(8);
    EvidenceLimits limits;
    limits.maximumNodes = 8;
    limits.maximumDependencies = 7;
    CHECK(validateEvidenceClosure(snapshot, roots, limits));
    limits.maximumNodes = 7;
    CHECK(validateEvidenceClosure(snapshot, roots, limits).code == Error::BudgetExceeded);
    limits.maximumNodes = 8; limits.maximumDependencies = 6;
    CHECK(validateEvidenceClosure(snapshot, roots, limits).code == Error::BudgetExceeded);
    limits = {}; limits.maximumMetadataBytes = 256;
    CHECK(validateEvidenceClosure(snapshot, roots, limits).code == Error::BudgetExceeded);
    limits = {}; limits.maximumNodes = 0;
    CHECK(validateEvidenceClosure(snapshot, roots, limits).code == Error::InvalidArgument);
    limits = {}; limits.maximumMetadataBytes = 0;
    CHECK(validateEvidenceClosure(snapshot, roots, limits).code == Error::InvalidArgument);
    limits = {}; limits.maximumTextBytes = 0;
    CHECK(validateEvidenceClosure(snapshot, roots, limits).code == Error::InvalidArgument);
    limits = {}; limits.maximumNodes = 1; limits.maximumDependencies = 0;
    auto leaf = graph(1);
    CHECK(validateEvidenceClosure(leaf, roots, limits));
    std::array<std::string, 2> repeated{"node0", "node0"};
    CHECK(validateEvidenceClosure(leaf, repeated, limits).code == Error::BudgetExceeded);
    limits = {};
    leaf.offsets.at("node0").evidence[0].source.assign(limits.maximumTextBytes, 'x');
    CHECK(validateEvidenceClosure(leaf, roots, limits));
    leaf.offsets.at("node0").evidence[0].source.push_back('x');
    CHECK(validateEvidenceClosure(leaf, roots, limits).code == Error::InvalidEvidence);
    std::atomic<bool> cancelled{true}; limits.cancelled = &cancelled;
    CHECK(validateEvidenceClosure(snapshot, roots, limits).code == Error::Cancelled);
    limits = {}; limits.deadline = std::chrono::steady_clock::time_point::min();
    CHECK(validateEvidenceClosure(snapshot, roots, limits).code == Error::DeadlineExceeded);
    auto deep = graph(4096);
    CHECK(validateEvidenceClosure(deep, roots));
    deep.offsets.at("node4095").dependencies["node0"] = 1;
    CHECK(validateEvidenceClosure(deep, roots).code == Error::InvalidEvidence);
    deep = graph(4097);
    CHECK(validateEvidenceClosure(deep, roots).code == Error::BudgetExceeded);
}
void allocationFailures() {
    const auto snapshot = graph(8);
    unsigned rejected = 0;
    bool completed = false;
    for (std::size_t allocation = 0; allocation < 64; ++allocation) {
        allocationsUntilFailure = allocation;
        const auto status = validateEvidenceClosure(snapshot, roots);
        allocationsUntilFailure = SIZE_MAX;
        if (status) { completed = true; break; }
        CHECK(status.code == Error::Internal && status.message.empty());
        ++rejected;
    }
    CHECK(completed && rejected >= 8);
    EvidenceLimits limits; limits.maximumNodes = 0;
    allocationsUntilFailure = 0;
    const auto invalid = validateEvidenceClosure(snapshot, roots, limits);
    allocationsUntilFailure = SIZE_MAX;
    CHECK(invalid.code == Error::InvalidArgument && invalid.message.empty());
    std::printf("PASS: %u successive evidence traversal allocation failures\n", rejected);
}
}
int main() {
    validation(); bounds(); allocationFailures();
    std::printf("PASS: %u bounded evidence closure checks\n", checks);
}

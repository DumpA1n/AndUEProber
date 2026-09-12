#include "andueprober/Core.hpp"
#include "andueprober/Reflection.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>

// The worker denies allocation after its callback has populated the working snapshot.
static thread_local bool denyAllocations = false;
static thread_local std::size_t allocationsUntilFailure = SIZE_MAX;
static std::atomic<unsigned> denied = 0;
void* operator new(std::size_t size) {
    if (denyAllocations || allocationsUntilFailure == 0) { ++denied; throw std::bad_alloc(); }
    if (allocationsUntilFailure != SIZE_MAX) --allocationsUntilFailure;
    if (auto* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
int main() {
    using namespace andueprober;
    for (bool throwCallback : {false, true}) {
        Session owner({});
        if (!owner.start([=](auto& working, const auto&) -> Status {
            working.messages = {std::string(256, 'A')};
            denyAllocations = true;
            if (throwCallback) throw std::bad_alloc();
            return {};
        })) return 1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (owner.snapshot()->state == TaskState::Running && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        if (!owner.stop()) return 2;
        const auto result = owner.snapshot();
        if (result->state != TaskState::Failed || result->result.code != Error::Internal || !result->result.message.empty()) return 3;
    }
    if (denied < 2) return 4;
    Snapshot analysis;
    analysis.sessionId = "owned-allocation-fixture";
    analysis.moduleIdentity = "owned-compiled-layout";
    analysis.layout = Layout::FField;
    analysis.layoutIdentity = "owned-allocation-layout";
    Offset offset;
    offset.value = 0; offset.validation = Validation::Validated;
    offset.evidence.push_back({"compiled layout", true, 1, {0}, "owned fixture", {"value"}});
    if (!publishOffset(analysis, "Record::value", std::move(offset))) return 5;
    ReflectionSchema schema{"owned-reflection", {{1, "Record", 8, 8, "compiled sizeof and alignof", {
        {"value", {ReflectionTypeKind::Scalar, ReflectionScalar::UInt64, 0, 1}, "Record::value"}}}}, {}};
    unsigned rejected = 0;
    bool completed = false;
    for (std::size_t allocation = 0; allocation < 512; ++allocation) {
        allocationsUntilFailure = allocation;
        const auto result = freezeReflection(analysis, schema);
        allocationsUntilFailure = SIZE_MAX;
        if (result.status) {
            if (!result.snapshot || !validateSnapshot(result.snapshot->analysis())) return 6;
            completed = true;
            break;
        }
        if (result.status.code != Error::Internal || result.snapshot) return 7;
        ++rejected;
    }
    if (!completed || rejected < 10) return 8;
    std::printf("PASS: worker allocation failures and %u successive reflection allocation failures publish Internal without a partial snapshot\n", rejected);
}

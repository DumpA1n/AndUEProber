#include "OwnedObjectFlags.hpp"
#include <cstdio>
#include <cstdlib>

using namespace andueprober;
namespace {
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
const std::string field = "UObject::ObjectFlags";
Status run(OwnedObjectFlags& fixture, Snapshot& snapshot, ReadBudget* supplied = nullptr) {
    ReadBudget budget; budget.generation = fixture.memory.epoch;
    const auto samples = fixture.samples();
    return probeObjectFlags(fixture.memory, fixture.profile(snapshot.layout), samples, fixture.memory.nameLayout, fixture.memory.address(64), fixture.memory.pool, supplied ? *supplied : budget, snapshot);
}
void validLayouts() {
    for (const auto layout : {Layout::FField, Layout::UProperty}) for (const bool zero : {false, true}) {
        OwnedObjectFlags fixture(zero); auto snapshot = fixture.initial(layout);
        CHECK(run(fixture, snapshot)); CHECK(validateSnapshot(snapshot));
        const auto& offset = snapshot.offsets.at(field);
        CHECK(offset.value == (zero ? 0 : 4) && offset.validation == Validation::Validated);
        CHECK(offset.dependencies.size() == 4 && offset.evidence.size() == 2);
        CHECK(offset.evidence.front().samples == 3 && offset.evidence.front().sampleIdentities.size() == 3);
        CHECK(snapshot.fieldReports.at(field).candidates.size() == 1 && snapshot.fieldReports.at(field).examinedOffsets == 16);
        CHECK(offset.evidence.front().source.find("memory-generation:1") != std::string::npos);
        auto changed = snapshot.offsets.at("UObject::InternalIndex");
        CHECK(publishOffset(snapshot, "UObject::InternalIndex", std::move(changed)));
        CHECK(snapshot.offsets.at(field).validation == Validation::Stale);
    }
}
void invalidMetadata() {
    for (unsigned mode = 0; mode < 15; ++mode) {
        OwnedObjectFlags fixture; auto snapshot = fixture.initial();
        auto profile = fixture.profile(); auto samples = fixture.samples(); auto names = fixture.memory.nameLayout;
        auto expected = Error::InvalidArgument;
        switch (mode) {
        case 0: profile.identity.clear(); break;
        case 1: profile.moduleIdentity = "wrong"; expected = Error::StaleIdentity; break;
        case 2: profile.generation++; expected = Error::StaleIdentity; break;
        case 3: profile.layout = Layout::Unknown; break;
        case 4: profile.extent = 3; break;
        case 5: profile.extent = 4097; break;
        case 6: names.size = 0; break;
        case 7: names.number = 0; break;
        case 8: samples[0].object = 0; break;
        case 9: samples[0].identity = samples[1].identity; break;
        case 10: samples[0].object = samples[1].object + 4; break;
        case 11: samples[0].object = UINTPTR_MAX - 2; expected = Error::Overflow; break;
        case 12: samples[0].identity = std::string("\xed\xa0\x80", 3); break;
        case 13: for (auto& sample : samples) sample.expectedFlags = 0; expected = Error::InvalidEvidence; break;
        case 14: profile.extent = 16; expected = Error::InvalidEvidence; break;
        }
        ReadBudget budget; budget.generation = 1;
        CHECK(probeObjectFlags(fixture.memory, profile, samples, names, fixture.memory.address(64), fixture.memory.pool, budget, snapshot).code == expected);
        CHECK(!snapshot.offsets.contains(field));
    }
    OwnedObjectFlags fixture; auto snapshot = fixture.initial(); auto samples = fixture.samples();
    ReadBudget budget; budget.generation = 1;
    CHECK(probeObjectFlags(fixture.memory, fixture.profile(), std::span(samples).first(2), fixture.memory.nameLayout, fixture.memory.address(64), fixture.memory.pool, budget, snapshot).code == Error::InvalidArgument);
    for (unsigned mode = 0; mode < 6; ++mode) {
        auto invalid = snapshot; auto& dependency = invalid.offsets.at("UObject::NamePrivate");
        switch (mode) {
        case 0: dependency.validation = Validation::Candidate; break;
        case 1: dependency.evidence.clear(); break;
        case 2: dependency.evidence.front().sampleIdentities.pop_back(); break;
        case 3: dependency.dependencies.begin()->second++; break;
        case 4: dependency.value = 8; break;
        case 5: dependency.value = UINT32_MAX; break;
        }
        CHECK(run(fixture, invalid).code == Error::InvalidEvidence);
    }
}
void readFailures() {
    for (const bool user : {false, true}) for (const bool expired : {false, true}) {
        OwnedObjectFlags fixture; auto snapshot = fixture.initial();
        CHECK(run(fixture, snapshot));
        if (user) snapshot.offsets.at(field).origin = Origin::User;
        const auto prior = snapshot.offsets.at(field);
        ReadBudget budget; budget.generation = 1;
        std::atomic<bool> cancelled{!expired}; budget.cancelled = &cancelled;
        if (expired) budget.deadline = std::chrono::steady_clock::time_point::min();
        CHECK(run(fixture, snapshot, &budget).code == (expired ? Error::DeadlineExceeded : Error::Cancelled));
        const auto& retained = snapshot.offsets.at(field);
        CHECK(retained.validation == (user ? Validation::Validated : Validation::Stale));
        CHECK(retained.value == prior.value && retained.version == prior.version + (user ? 0 : 1) && retained.origin == prior.origin);
    }
    for (const auto error : {Error::PermissionDenied, Error::Unmapped, Error::ShortRead}) {
        OwnedObjectFlags fixture; auto snapshot = fixture.initial();
        fixture.memory.failure = error;
        CHECK(run(fixture, snapshot).code == error); CHECK(!snapshot.offsets.contains(field));
    }
    for (unsigned mode = 0; mode < 6; ++mode) {
        OwnedObjectFlags fixture; auto snapshot = fixture.initial();
        ReadBudget budget; budget.generation = 1;
        std::atomic<bool> cancelled{false}; budget.cancelled = &cancelled;
        auto expected = Error::Cancelled;
        switch (mode) {
        case 0: cancelled = true; break;
        case 1: budget.deadline = std::chrono::steady_clock::time_point::min(); expected = Error::DeadlineExceeded; break;
        case 2: budget.remainingBytes = 3; expected = Error::BudgetExceeded; break;
        case 3: fixture.memory.beforeRead = [&](auto&, auto) { cancelled = true; }; break;
        case 4: fixture.memory.beforeRead = [&](auto& reader, auto) { ++reader.epoch; }; expected = Error::StaleIdentity; break;
        case 5: fixture.memory.shortRead = true; expected = Error::ShortRead; break;
        }
        CHECK(run(fixture, snapshot, &budget).code == expected); CHECK(!snapshot.offsets.contains(field));
    }
}
void ambiguityAndReadback() {
    {
        OwnedObjectFlags fixture; auto snapshot = fixture.initial(); CHECK(run(fixture, snapshot));
        for (std::uint32_t i = 1; i < 4; ++i) fixture.memory.put<std::uint32_t>(2048 + i * 64 + 32, fixture.flags[i]);
        CHECK(run(fixture, snapshot).code == Error::InvalidEvidence);
        CHECK(snapshot.fieldReports.at(field).candidates.size() == 2);
        CHECK(snapshot.offsets.at(field).validation == Validation::Stale);
    }
    for (unsigned target = 1; target <= 3; ++target) {
        OwnedObjectFlags fixture; auto snapshot = fixture.initial(); unsigned reads = 0;
        fixture.memory.beforeRead = [&](auto& reader, std::size_t offset) {
            if (offset == 2048 + target * 64 + 4 && ++reads == 2)
                reader.template put<std::uint32_t>(offset, 9);
        };
        CHECK(run(fixture, snapshot).code == Error::InvalidEvidence);
        CHECK(!snapshot.offsets.contains(field));
        CHECK(snapshot.fieldReports.at(field).candidates.empty());
    }
    {
        OwnedObjectFlags fixture; auto snapshot = fixture.initial(); unsigned reads = 0;
        fixture.memory.beforeRead = [&](auto&, std::size_t offset) {
            if (offset == 2048 + 64 + 4 && ++reads == 2) ++snapshot.offsets.at("UObject::InternalIndex").version;
        };
        CHECK(run(fixture, snapshot).code == Error::InvalidEvidence); CHECK(!snapshot.offsets.contains(field));
    }
    {
        OwnedObjectFlags fixture; auto snapshot = fixture.initial();
        auto samples = fixture.samples();
        for (std::size_t i = 0; i < samples.size(); ++i) samples[i].expectedFlags = i + 1;
        ReadBudget budget; budget.generation = 1;
        CHECK(probeObjectFlags(fixture.memory, fixture.profile(), samples, fixture.memory.nameLayout, fixture.memory.address(64), fixture.memory.pool, budget, snapshot).code == Error::InvalidEvidence);
        CHECK(snapshot.fieldReports.at(field).candidates.empty());
    }
}
void userOverrides() {
    for (unsigned mode = 0; mode < 9; ++mode) {
        OwnedObjectFlags fixture; auto snapshot = fixture.initial(); CHECK(run(fixture, snapshot));
        auto value = snapshot.offsets.at(field); value.origin = Origin::User;
        CHECK(publishOffset(snapshot, field, std::move(value)));
        auto& override = snapshot.offsets.at(field);
        switch (mode) {
        case 1: override.value = 32; break;
        case 2: override.validation = Validation::Candidate; break;
        case 3: override.evidence.clear(); break;
        case 4: override.evidence.front().passed = false; break;
        case 5: override.dependencies.clear(); break;
        case 6: override.dependencies.begin()->second++; break;
        case 7: override.evidence.front().sampleIdentities.clear(); break;
        case 8: override.dependencies["Missing"] = 1; break;
        }
        const auto version = override.version; const auto prior = override.value; const auto validation = override.validation;
        const auto status = run(fixture, snapshot);
        CHECK(status.code == (mode ? Error::InvalidEvidence : Error::None));
        const auto& retained = snapshot.offsets.at(field);
        CHECK(retained.origin == Origin::User && retained.value == prior && retained.version == version && retained.validation == validation);
    }
}
void evidenceClosureAndIdentity() {
    for (unsigned mode = 0; mode < 6; ++mode) {
        OwnedObjectFlags fixture; auto snapshot = fixture.initial();
        auto leaf = snapshot.offsets.at("UObject::InternalIndex");
        leaf.dependencies.clear(); leaf.version = mode == 0 ? 2 : 1;
        auto middle = leaf; middle.version = 1; middle.dependencies["ReviewLeaf"] = 1;
        if (mode == 1) leaf.dependencies["ReviewMiddle"] = 1;
        if (mode == 2) leaf.validation = Validation::Stale;
        if (mode == 3) leaf.evidence.front().passed = false;
        if (mode == 4) leaf.evidence.front().sampleIdentities.clear();
        snapshot.offsets["ReviewLeaf"] = leaf; snapshot.offsets["ReviewMiddle"] = middle;
        snapshot.offsets.at("UObject::InternalIndex").dependencies["ReviewMiddle"] = 1;
        unsigned reads = 0;
        fixture.memory.beforeRead = [&](auto&, std::size_t offset) {
            if (mode == 5 && offset == 2048 + 64 + 4 && ++reads == 2)
                ++snapshot.offsets.at("ReviewLeaf").version;
        };
        CHECK(run(fixture, snapshot).code == Error::InvalidEvidence); CHECK(!snapshot.offsets.contains(field));
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
        OwnedObjectFlags fixture; auto snapshot = fixture.initial();
        auto names = fixture.memory.nameLayout; auto poolProfile = fixture.memory.pool; auto pool = fixture.memory.address(64);
        switch (mode) {
        case 0: names.size = 4; names.number.reset(); break;
        case 1: pool += 8; break;
        case 2: poolProfile.maximumUnits = 128; break;
        case 3: names.comparison = 4; names.number = 0; break;
        }
        ReadBudget budget; budget.generation = 1;
        CHECK(probeObjectFlags(fixture.memory, fixture.profile(), fixture.samples(), names, pool, poolProfile, budget, snapshot).code == Error::InvalidEvidence);
        CHECK(!snapshot.offsets.contains(field));
    }
    {
        OwnedObjectFlags fixture; auto snapshot = fixture.initial();
        auto unrelated = snapshot.offsets.at("UObject::InternalIndex"); unrelated.validation = Validation::Stale;
        snapshot.offsets["Unrelated"] = unrelated;
        CHECK(run(fixture, snapshot)); CHECK(snapshot.offsets.at(field).validation == Validation::Validated);
        CHECK(snapshot.offsets.at("Unrelated").validation == Validation::Stale);
    }
    for (unsigned mode = 0; mode < 3; ++mode) {
        OwnedObjectFlags fixture; auto snapshot = fixture.initial();
        auto& offset = snapshot.offsets.at("UObject::InternalIndex");
        if (mode == 0) offset.evidence.front().source.assign(4 * 1024 * 1024 + 1, 'A');
        if (mode == 1) for (unsigned i = 0; i <= 16384; ++i) offset.dependencies["Missing" + std::to_string(i)] = 1;
        if (mode == 2) {
            auto node = offset; node.dependencies.clear();
            for (unsigned i = 0; i < 4097; ++i) {
                auto copy = node;
                if (i + 1 < 4097) copy.dependencies["Node" + std::to_string(i + 1)] = 1;
                snapshot.offsets["Node" + std::to_string(i)] = std::move(copy);
            }
            offset.dependencies["Node0"] = 1;
        }
        CHECK(run(fixture, snapshot).code == Error::BudgetExceeded); CHECK(!snapshot.offsets.contains(field));
    }
    {
        OwnedObjectFlags fixture; auto snapshot = fixture.initial();
        fixture.memory.beforeRead = [](auto&, auto) { throw std::bad_alloc(); };
        CHECK(run(fixture, snapshot).code == Error::Internal); CHECK(!snapshot.offsets.contains(field));
    }
}
}
int main() {
    validLayouts(); invalidMetadata(); readFailures(); ambiguityAndReadback(); userOverrides(); evidenceClosureAndIdentity();
    std::printf("PASS: %u object flag checks; explicit uint32 metadata, actual Phase 1 evidence, unique bounded reads, prefix exclusion and strict overrides\n", checks);
}

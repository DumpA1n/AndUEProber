#include "OwnedPropertyTails.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace andueprober;
using namespace std::chrono_literals;
namespace {
unsigned checks = 0;
#define REQUIRE(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
std::vector<std::string> fields(PropertyTailKind kind) {
    switch (kind) {
    case PropertyTailKind::Enum: return {"FEnumProperty::UnderlyingType", "FEnumProperty::Enum"};
    case PropertyTailKind::Array: return {"FArrayProperty::Inner"};
    case PropertyTailKind::Set: return {"FSetProperty::ElementProp"};
    case PropertyTailKind::Map: return {"FMapProperty::KeyProp", "FMapProperty::ValueProp"};
    case PropertyTailKind::Struct: return {"FStructProperty::Struct"};
    case PropertyTailKind::Byte: return {"FByteProperty::Enum"};
    case PropertyTailKind::Class: return {"FObjectPropertyBase::PropertyClass", "FClassProperty::MetaClass"};
    case PropertyTailKind::Interface: return {"FInterfaceProperty::InterfaceClass"};
    default: return {"FObjectPropertyBase::PropertyClass"};
    }
}
Status probe(OwnedPropertyTails& fixture, const PropertyTailProfile& profile, std::span<const PropertyTailSample> samples,
    ReadBudget& budget, Snapshot& snapshot) {
    const auto& source = fixture.phase5.phase5.nameMemory();
    return probePropertyTails(fixture, profile, samples, source.nameLayout, source.address(64), source.pool, budget, snapshot);
}
Status observe(OwnedPropertyTails& fixture, PropertyTailKind kind, Snapshot& snapshot) {
    ReadBudget budget; budget.generation = fixture.epoch;
    return probe(fixture, fixture.profile(kind), fixture.samples(kind), budget, snapshot);
}
void success() {
    for (const auto kind : OwnedPropertyTails::kinds) {
        OwnedPropertyTails fixture; auto snapshot = fixture.initial(); const auto outputs = fields(kind);
        REQUIRE(observe(fixture, kind, snapshot)); REQUIRE(validateSnapshot(snapshot));
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const auto& result = snapshot.offsets.at(outputs[i]); const auto& report = snapshot.fieldReports.at(outputs[i]);
            REQUIRE(result.value == fixture.offset(kind, i) && result.validation == Validation::Validated);
            REQUIRE(result.dependencies.size() == 9 && result.evidence.size() == 2);
            REQUIRE(report.candidates.size() == 1 && report.examinedOffsets > 0);
            for (const auto& proof : result.evidence) REQUIRE(proof.samples == 3 && proof.sampleIdentities.size() == 3);
        }
        if (fixture.dual(kind)) REQUIRE(std::max(fixture.offset(kind, 0), fixture.offset(kind, 1)) - std::min(fixture.offset(kind, 0), fixture.offset(kind, 1)) > sizeof(std::uintptr_t));
        if (kind == PropertyTailKind::Class) REQUIRE(fixture.offset(kind, 0) == fixture.offset(PropertyTailKind::Object, 0));
        REQUIRE(fixture.targetReads == 0 && ownedNativeFunctionCalls == 0);
        for (const auto* dependency : {"FField::Owner", "FProperty::PropertyFlags"}) {
            auto changed = snapshot; auto prior = changed.offsets.at(dependency); REQUIRE(publishOffset(changed, dependency, std::move(prior)));
            for (const auto& field : outputs) REQUIRE(changed.offsets.at(field).validation == Validation::Stale);
        }
        auto samples = fixture.samples(kind);
        samples[2].expectedFirst = 0; samples[2].firstIdentity = "declared-null-first"; fixture.setPointer(kind, 2, 0, 0);
        if (fixture.dual(kind)) {
            samples[2].expectedSecond = 0; samples[2].secondIdentity = "declared-null-second"; fixture.setPointer(kind, 2, 1, 0);
        }
        ReadBudget budget; budget.generation = fixture.epoch;
        REQUIRE(probe(fixture, fixture.profile(kind), samples, budget, snapshot)); REQUIRE(fixture.targetReads == 0);
    }
}
void metadata() {
    for (unsigned mode = 0; mode < 23; ++mode) {
        OwnedPropertyTails fixture; auto snapshot = fixture.initial(); auto profile = fixture.profile(PropertyTailKind::Enum);
        auto samples = fixture.samples(profile.kind); ReadBudget budget; budget.generation = 1;
        Error expected = Error::InvalidArgument;
        switch (mode) {
        case 0: profile.kind = PropertyTailKind::Bool; expected = Error::Unsupported; break;
        case 1: profile.kind = PropertyTailKind::Unknown; expected = Error::Unsupported; break;
        case 2: profile.property.layout = Layout::UProperty; expected = Error::Unsupported; break;
        case 3: ++profile.property.generation; expected = Error::StaleIdentity; break;
        case 4: profile.extent = 4097; break;
        case 5: profile.propertyBaseExtent = profile.extent; break;
        case 6: profile.propertyBaseExtent = offsetof(OwnedNativeProperty, offsetInternal); expected = Error::InvalidEvidence; break;
        case 7: profile.property.fieldBaseProfileIdentity = "another-ffield-profile"; expected = Error::InvalidEvidence; break;
        case 8: profile.property.identity = "another-property-profile"; expected = Error::InvalidEvidence; break;
        case 9: profile.property.ownerLayout.size = 24; expected = Error::InvalidEvidence; break;
        case 10: samples[0].expectedSecond.reset(); break;
        case 11: samples[0].object = 0; break;
        case 12: samples[0].object = samples[1].object + 8; break;
        case 13: samples[0].object = UINTPTR_MAX - 2; expected = Error::Overflow; break;
        case 14: samples[0].identity = samples[1].identity; break;
        case 15: samples[0].firstIdentity = samples[1].firstIdentity; expected = Error::InvalidEvidence; break;
        case 16: samples[0].firstIdentity = "\xC0\xAF"; break;
        case 17: for (auto& sample : samples) { sample.expectedFirst = samples[0].expectedFirst; sample.firstIdentity = "same-target"; } expected = Error::InvalidEvidence; break;
        case 18: snapshot.offsets.erase("FProperty::ArrayDim"); expected = Error::InvalidEvidence; break;
        case 19: snapshot.offsets.at("FField::NamePrivate").value = snapshot.offsets.at("FField::Next").value; expected = Error::InvalidEvidence; break;
        case 20: snapshot.offsets.at("FProperty::ArrayDim").value = snapshot.offsets.at("FField::NamePrivate").value; expected = Error::InvalidEvidence; break;
        case 21: profile.identity.clear(); break;
        case 22: profile.property.fieldBaseExtent = 0; break;
        }
        REQUIRE(probe(fixture, profile, samples, budget, snapshot).code == expected);
        REQUIRE(fixture.reads == 0);
        for (const auto& field : fields(PropertyTailKind::Enum)) REQUIRE(!snapshot.offsets.contains(field));
    }
    for (const auto kind : {PropertyTailKind::Array, PropertyTailKind::Set, PropertyTailKind::Object, PropertyTailKind::Struct, PropertyTailKind::Byte, PropertyTailKind::Interface}) {
        OwnedPropertyTails fixture; auto snapshot = fixture.initial(); auto samples = fixture.samples(kind);
        samples[0].expectedSecond = 0; samples[0].secondIdentity = "not-part-of-this-kind";
        ReadBudget budget; budget.generation = 1;
        REQUIRE(probe(fixture, fixture.profile(kind), samples, budget, snapshot).code == Error::InvalidArgument); REQUIRE(fixture.reads == 0);
    }
    for (unsigned mode = 0; mode < 3; ++mode) {
        OwnedPropertyTails fixture; auto snapshot = fixture.initial(); const auto& source = fixture.phase5.phase5.nameMemory();
        auto names = source.nameLayout; auto pool = source.address(64); auto poolProfile = source.pool;
        if (mode == 0) names = {0, {}, {}, 4};
        if (mode == 1) ++pool;
        if (mode == 2) --poolProfile.maximumUnits;
        ReadBudget budget; budget.generation = 1;
        REQUIRE(probePropertyTails(fixture, fixture.profile(PropertyTailKind::Array), fixture.samples(PropertyTailKind::Array),
            names, pool, poolProfile, budget, snapshot).code == Error::InvalidEvidence); REQUIRE(fixture.reads == 0);
    }
    {
        OwnedPropertyTails fixture; auto snapshot = fixture.initial(); const auto& source = fixture.phase5.phase5.nameMemory();
        auto prior = fixture.phase5.profile(); auto differentExtent = prior; differentExtent.extent += 64;
        REQUIRE(propertyObservationIdentity(prior, source.nameLayout, source.address(64), source.pool) ==
            propertyObservationIdentity(differentExtent, source.nameLayout, source.address(64), source.pool));
        const auto samples = fixture.samples(PropertyTailKind::Array); ReadBudget budget; budget.generation = 1;
        REQUIRE(probe(fixture, fixture.profile(PropertyTailKind::Array), std::span(samples).first(2), budget, snapshot).code == Error::InvalidArgument);
        const std::vector<PropertyTailSample> many(17, samples[0]);
        REQUIRE(probe(fixture, fixture.profile(PropertyTailKind::Array), many, budget, snapshot).code == Error::InvalidArgument);
    }
}
void failures() {
    for (const auto kind : OwnedPropertyTails::kinds) for (const auto error : {Error::PermissionDenied, Error::Unmapped, Error::ShortRead,
        Error::Unsupported, Error::BudgetExceeded, Error::Cancelled, Error::DeadlineExceeded, Error::StaleIdentity, Error::Internal}) {
        OwnedPropertyTails fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, kind, snapshot));
        ReadBudget budget; budget.generation = 1; std::atomic<bool> cancelled{false};
        if (error == Error::PermissionDenied || error == Error::Unmapped || error == Error::Unsupported) fixture.failure = error;
        if (error == Error::ShortRead) fixture.shortRead = true;
        if (error == Error::BudgetExceeded) budget.remainingBytes = 1;
        if (error == Error::Cancelled) { cancelled = true; budget.cancelled = &cancelled; }
        if (error == Error::DeadlineExceeded) budget.deadline = std::chrono::steady_clock::time_point::min();
        if (error == Error::StaleIdentity) fixture.beforeRead = [](auto& memory, auto, auto) { ++memory.epoch; };
        if (error == Error::Internal) fixture.beforeRead = [](auto&, auto, auto) { throw std::bad_alloc(); };
        REQUIRE(probe(fixture, fixture.profile(kind), fixture.samples(kind), budget, snapshot).code == error);
        for (const auto& field : fields(kind)) REQUIRE(snapshot.offsets.at(field).validation == Validation::Stale);
        REQUIRE(fixture.targetReads == 0);
    }
    for (const auto kind : OwnedPropertyTails::kinds) for (std::size_t column = 0; column < fields(kind).size(); ++column) {
        OwnedPropertyTails fixture; auto snapshot = fixture.initial();
        for (std::size_t i = 0; i < 3; ++i) fixture.setPointer(kind, i, column, fixture.target(kind, i, column), true);
        REQUIRE(observe(fixture, kind, snapshot).code == Error::InvalidEvidence);
        REQUIRE(snapshot.fieldReports.at(fields(kind)[column]).candidates.size() == 2);
        for (const auto& field : fields(kind)) REQUIRE(!snapshot.offsets.contains(field));
    }
    for (const auto kind : OwnedPropertyTails::kinds) for (std::size_t column = 0; column < fields(kind).size(); ++column) {
        OwnedPropertyTails fixture; auto snapshot = fixture.initial(); const auto address = fixture.address(kind, 0) + fixture.offset(kind, column);
        std::size_t visits = 0;
        fixture.beforeRead = [&](auto&, auto current, auto) { if (current == address) ++visits; };
        REQUIRE(observe(fixture, kind, snapshot)); REQUIRE(visits >= 2); const auto last = visits; visits = 0;
        fixture.beforeRead = [&](auto& memory, auto current, auto) { if (current == address && ++visits == last) memory.setPointer(kind, 0, column, 0); };
        REQUIRE(observe(fixture, kind, snapshot).code == Error::InvalidEvidence);
        const auto& report = snapshot.fieldReports.at(fields(kind)[column]);
        REQUIRE(report.candidates.empty() && report.rejected.back().reason.find("changed") != std::string::npos);
        for (const auto& field : fields(kind)) REQUIRE(snapshot.offsets.at(field).validation == Validation::Stale);
    }
    for (const auto kind : {PropertyTailKind::Enum, PropertyTailKind::Map, PropertyTailKind::Class}) {
        OwnedPropertyTails fixture; auto snapshot = fixture.initial(); auto samples = fixture.samples(kind);
        for (std::size_t i = 0; i < 3; ++i) {
            samples[i].expectedSecond = samples[i].expectedFirst; samples[i].secondIdentity = samples[i].firstIdentity;
            fixture.setPointer(kind, i, 1, 0);
        }
        ReadBudget budget; budget.generation = 1;
        REQUIRE(probe(fixture, fixture.profile(kind), samples, budget, snapshot).code == Error::InvalidEvidence);
        REQUIRE(snapshot.fieldReports.at(fields(kind)[0]).candidates.size() == 1 && snapshot.fieldReports.at(fields(kind)[1]).candidates.size() == 1);
        for (const auto& field : fields(kind)) REQUIRE(!snapshot.offsets.contains(field));
    }
}
void overridesAndClosure() {
    for (const auto kind : OwnedPropertyTails::kinds) for (const auto& field : fields(kind)) for (unsigned mode = 0; mode < 9; ++mode) {
        OwnedPropertyTails fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, kind, snapshot));
        auto value = snapshot.offsets.at(field); value.origin = Origin::User;
        switch (mode) {
        case 1: value.value = 0; break;
        case 2: value.validation = Validation::Candidate; break;
        case 3: value.validation = Validation::Stale; break;
        case 4: value.evidence.clear(); break;
        case 5: value.evidence[0].passed = false; break;
        case 6: value.dependencies.erase("FProperty::ArrayDim"); break;
        case 7: ++value.dependencies.at("FField::NamePrivate"); break;
        case 8: value.dependencies["missing"] = 1; break;
        }
        REQUIRE(publishOffset(snapshot, field, std::move(value))); const auto retained = snapshot.offsets.at(field);
        REQUIRE(observe(fixture, kind, snapshot).code == (mode ? Error::InvalidEvidence : Error::None));
        const auto& current = snapshot.offsets.at(field);
        REQUIRE(current.value == retained.value && current.origin == Origin::User && current.version == retained.version);
        REQUIRE(current.validation == retained.validation && current.dependencies == retained.dependencies);
    }
    for (unsigned mode = 0; mode < 6; ++mode) {
        OwnedPropertyTails fixture; auto snapshot = fixture.initial();
        auto leaf = snapshot.offsets.at("UObject::InternalIndex"); leaf.dependencies.clear(); leaf.version = 1;
        auto middle = leaf; middle.dependencies["owned-leaf"] = 1;
        snapshot.offsets["owned-leaf"] = leaf; snapshot.offsets["owned-middle"] = middle;
        snapshot.offsets.at("UObject::InternalIndex").dependencies["owned-middle"] = 1;
        if (mode == 0) snapshot.offsets.at("owned-leaf").validation = Validation::Candidate;
        if (mode == 1) snapshot.offsets.at("owned-leaf").validation = Validation::Stale;
        if (mode == 2) snapshot.offsets.at("owned-leaf").dependencies["owned-middle"] = 1;
        if (mode == 3) ++snapshot.offsets.at("owned-leaf").version;
        if (mode == 4) snapshot.offsets.at("owned-leaf").evidence.clear();
        if (mode == 5) fixture.beforeRead = [&](auto&, auto, auto) { snapshot.offsets.at("owned-leaf").validation = Validation::Candidate; };
        REQUIRE(observe(fixture, PropertyTailKind::Array, snapshot).code == Error::InvalidEvidence);
        if (mode != 5) REQUIRE(fixture.reads == 0);
        REQUIRE(!snapshot.offsets.contains("FArrayProperty::Inner"));
    }
    {
        OwnedPropertyTails fixture; auto snapshot = fixture.initial();
        Offset leaf; leaf.value = 0; leaf.version = 1; leaf.validation = Validation::Validated;
        leaf.evidence.push_back({"owned declared dependency", true, 1, {0}, "owned-metadata", {"anchor"}});
        for (unsigned i = 0; i < 4100; ++i) {
            auto node = leaf;
            if (i + 1 < 4100) node.dependencies["deep-tail:" + std::to_string(i + 1)] = 1;
            snapshot.offsets["deep-tail:" + std::to_string(i)] = std::move(node);
        }
        snapshot.offsets.at("UObject::InternalIndex").dependencies["deep-tail:0"] = 1;
        REQUIRE(observe(fixture, PropertyTailKind::Set, snapshot).code == Error::BudgetExceeded);
        REQUIRE(fixture.reads == 0 && !snapshot.offsets.contains("FSetProperty::ElementProp"));
    }
    {
        OwnedPropertyTails fixture; auto snapshot = fixture.initial(); Offset stale; stale.validation = Validation::Stale;
        snapshot.offsets["unrelated-stale"] = stale;
        REQUIRE(observe(fixture, PropertyTailKind::Object, snapshot));
    }
    {
        OwnedPropertyTails fixture; auto snapshot = fixture.initial(); REQUIRE(observe(fixture, PropertyTailKind::Map, snapshot));
        auto& key = snapshot.offsets.at("FMapProperty::KeyProp"); auto& value = snapshot.offsets.at("FMapProperty::ValueProp");
        key.origin = value.origin = Origin::User; key.dependencies["FMapProperty::ValueProp"] = value.version;
        value.dependencies["FMapProperty::KeyProp"] = key.version;
        REQUIRE(observe(fixture, PropertyTailKind::Map, snapshot).code == Error::InvalidEvidence);
    }
}
void reportBudget() {
    struct NativeRepeatedTail { OwnedNativeProperty property; std::array<OwnedTailTarget*, 480> pointers; };
    static_assert(sizeof(NativeRepeatedTail) <= 4096);
    struct Memory final : MemoryReader {
        OwnedPropertyTails predecessor;
        std::array<NativeRepeatedTail, 16> records;
        std::size_t reads = 0;
        Memory() {
            for (std::size_t i = 0; i < records.size(); ++i) {
                records[i].property = predecessor.phase5.properties[i % 3];
                records[i].pointers.fill(reinterpret_cast<OwnedTailTarget*>(predecessor.target(PropertyTailKind::Array, i, 0)));
            }
        }
        ReadResult read(std::uintptr_t address, std::span<std::byte> output) override {
            ++reads;
            for (const auto& record : records) {
                const auto begin = reinterpret_cast<std::uintptr_t>(&record);
                if (address < begin || address - begin > sizeof(record) || output.size() > sizeof(record) - (address - begin)) continue;
                std::memcpy(output.data(), reinterpret_cast<const std::byte*>(&record) + address - begin, output.size());
                return {output.size(), Error::None};
            }
            return predecessor.read(address, output);
        }
        std::uint64_t generation() const override { return 1; }
    } fixture;
    auto snapshot = fixture.predecessor.initial();
    auto profile = fixture.predecessor.profile(PropertyTailKind::Array); profile.extent = sizeof(NativeRepeatedTail);
    std::vector<PropertyTailSample> samples;
    for (std::size_t i = 0; i < fixture.records.size(); ++i)
        samples.push_back({reinterpret_cast<std::uintptr_t>(&fixture.records[i]), "repeated:" + std::to_string(i) + std::string(400, 'A'),
            fixture.predecessor.target(PropertyTailKind::Array, i, 0), "target:" + std::to_string(i % 2) + std::string(400, 'B'), {}, {}});
    const auto& source = fixture.predecessor.phase5.phase5.nameMemory(); ReadBudget budget; budget.generation = 1;
    REQUIRE(probePropertyTails(fixture, profile, samples, source.nameLayout, source.address(64), source.pool, budget, snapshot).code == Error::BudgetExceeded);
    REQUIRE(fixture.reads > 0 && !snapshot.offsets.contains("FArrayProperty::Inner"));
    REQUIRE(!snapshot.fieldReports.at("FArrayProperty::Inner").candidates.empty());
    REQUIRE(fixture.predecessor.targetReads == 0);
}
void sessions() {
    OwnedPropertyTails fixture; const auto initial = fixture.initial(); Session session(initial); Snapshot* working = nullptr;
    REQUIRE(session.start([&](Snapshot& value, const auto& cancelled) {
        working = &value; ReadBudget budget; budget.generation = 1; budget.cancelled = &cancelled;
        return probe(fixture, fixture.profile(PropertyTailKind::Enum), fixture.samples(PropertyTailKind::Enum), budget, value);
    }));
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (session.snapshot()->state == TaskState::Running && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(session.stop()); const auto frozen = session.snapshot(); REQUIRE(frozen->state == TaskState::Succeeded);
    working->offsets.at("FEnumProperty::Enum").value = 0;
    REQUIRE(frozen->offsets.at("FEnumProperty::Enum").value == fixture.offset(PropertyTailKind::Enum, 1));
    OwnedPropertyTails cancelledFixture; Session active(cancelledFixture.initial()); std::atomic<bool> entered{false}, resume{false};
    cancelledFixture.beforeRead = [&](auto&, auto, auto) { if (!entered.exchange(true)) while (!resume) std::this_thread::yield(); };
    REQUIRE(active.start([&](auto& value, const auto& cancelled) {
        ReadBudget budget; budget.generation = 1; budget.cancelled = &cancelled;
        return probe(cancelledFixture, cancelledFixture.profile(PropertyTailKind::Map), cancelledFixture.samples(PropertyTailKind::Map), budget, value);
    }));
    const auto activeDeadline = std::chrono::steady_clock::now() + 10s;
    while (!entered && std::chrono::steady_clock::now() < activeDeadline) std::this_thread::yield(); REQUIRE(entered);
    active.cancel(); resume = true; REQUIRE(active.stop());
    REQUIRE(active.snapshot()->state == TaskState::Cancelled && active.snapshot()->result.code == Error::Cancelled);
    REQUIRE(!active.snapshot()->offsets.contains("FMapProperty::KeyProp"));
}
}
int main() {
    success(); metadata(); failures(); overridesAndClosure(); reportBudget(); sessions(); REQUIRE(ownedNativeFunctionCalls == 0);
    std::printf("PASS: %u property-tail checks; eleven opaque pointer offsets across nine kinds, independent occupied prefixes, actual preceding phases and atomic closure validation; bool and complete containers excluded\n", checks);
}

#include "OwnedEnums.hpp"
#include <cstdio>
#include <cstdlib>

using namespace andueprober;
namespace {
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
const std::string field = "UEnum::Names";
Status run(OwnedEnums& fixture, Snapshot& snapshot, ReadBudget* supplied = nullptr) {
    ReadBudget budget; budget.generation = fixture.epoch;
    return probeEnumNames(fixture, fixture.profile(snapshot.layout), fixture.samples(), fixture.phase2.phase1.nameLayout,
        fixture.phase2.phase1.address(64), fixture.phase2.phase1.pool, supplied ? *supplied : budget, snapshot);
}
void validLayouts() {
    for (const auto layout : {Layout::FField, Layout::UProperty})
        for (const bool countFirst : {false, true}) for (const bool valueFirst : {false, true}) {
            OwnedEnums fixture(countFirst, valueFirst); auto snapshot = fixture.initial(layout);
            const auto status = run(fixture, snapshot);
            if (!status) std::fprintf(stderr, "initial enum error %d: %s\n", int(status.code), status.message.c_str());
            CHECK(status); CHECK(validateSnapshot(snapshot));
            const auto& observed = snapshot.offsets.at(field);
            CHECK(observed.value == fixture.namesOffset() && observed.validation == Validation::Validated);
            CHECK(observed.dependencies.size() == 5 && observed.evidence.size() >= 2);
            CHECK(snapshot.fieldReports.at(field).candidates.size() == 1);
            for (const auto& evidence : observed.evidence) CHECK(evidence.samples == evidence.sampleIdentities.size());
            auto prior = snapshot.offsets.at("UField::Next");
            CHECK(publishOffset(snapshot, "UField::Next", std::move(prior)));
            CHECK(snapshot.offsets.at(field).validation == Validation::Stale);
        }
}
void invalidConfiguration() {
    for (unsigned mode = 0; mode < 16; ++mode) {
        OwnedEnums fixture; auto snapshot = fixture.initial(); auto profile = fixture.profile();
        auto expected = Error::InvalidArgument;
        switch (mode) {
        case 0: profile.identity.clear(); break;
        case 1: profile.moduleIdentity = "wrong"; expected = Error::StaleIdentity; break;
        case 2: profile.generation++; expected = Error::StaleIdentity; break;
        case 3: profile.extent = 4097; break;
        case 4: profile.extent = 0; break;
        case 5: profile.array.data.reset(); break;
        case 6: profile.array.capacity = profile.array.count; break;
        case 7: profile.array.data = 1; break;
        case 8: profile.array.size = 65; break;
        case 9: profile.entry.value.reset(); break;
        case 10: profile.entry.value = profile.entry.name; break;
        case 11: profile.entry.stride = 129; break;
        case 12: profile.entry.stride = 0; break;
        case 13: profile.maximumValues = 0; break;
        case 14: profile.maximumCapacity = 0; break;
        case 15: profile.fieldBaseExtent = profile.extent; break;
        }
        ReadBudget budget; budget.generation = 1;
        CHECK(probeEnumNames(fixture, profile, fixture.samples(), fixture.phase2.phase1.nameLayout,
            fixture.phase2.phase1.address(64), fixture.phase2.phase1.pool, budget, snapshot).code == expected);
        CHECK(!snapshot.offsets.contains(field));
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
        OwnedEnums fixture; auto snapshot = fixture.initial(); auto profile = fixture.profile();
        auto names = fixture.phase2.phase1.nameLayout; auto pool = fixture.phase2.phase1.address(64); auto poolProfile = fixture.phase2.phase1.pool;
        if (mode == 0) { names.size = 4; names.number.reset(); }
        if (mode == 1) pool += 8;
        if (mode == 2) poolProfile.maximumUnits = 128;
        if (mode == 3) profile.fieldBaseExtent = 32;
        ReadBudget budget; budget.generation = 1;
        CHECK(probeEnumNames(fixture, profile, fixture.samples(), names, pool, poolProfile, budget, snapshot).code == Error::InvalidEvidence);
        CHECK(!snapshot.offsets.contains(field));
    }
}
void metadataFailures() {
    for (unsigned mode = 0; mode < 9; ++mode) {
        OwnedEnums fixture; auto snapshot = fixture.initial(); auto samples = fixture.samples();
        auto expected = Error::InvalidArgument;
        switch (mode) {
        case 0: samples[0].object = 0; break;
        case 1: samples[0].object = samples[1].object + 8; break;
        case 2: samples[0].identity = samples[1].identity; break;
        case 3: samples[0].object = UINTPTR_MAX - 2; expected = Error::Overflow; break;
        case 4: samples[0].values.clear(); break;
        case 5: samples[1].values[1].expectedName = samples[1].values[0].expectedName; break;
        case 6: samples[1].values[1].identity = samples[0].values[0].identity; break;
        case 7: samples[0].values[0].expectedName = std::string("\xc0\xaf", 2); break;
        case 8: samples[0].values[0].expectedName.clear(); break;
        }
        ReadBudget budget; budget.generation = 1;
        CHECK(probeEnumNames(fixture, fixture.profile(), samples, fixture.phase2.phase1.nameLayout,
            fixture.phase2.phase1.address(64), fixture.phase2.phase1.pool, budget, snapshot).code == expected);
        CHECK(!snapshot.offsets.contains(field));
    }
}
void corruptArrays() {
    for (unsigned mode = 0; mode < 8; ++mode) {
        OwnedEnums fixture; auto snapshot = fixture.initial();
        switch (mode) {
        case 0: fixture.setValue(1, 1, 100); break;
        case 1: fixture.setName(2, 2, 0xffffffff); break;
        case 2: fixture.enums[0].names.count = -1; break;
        case 3: fixture.enums[0].names.capacity = 0; break;
        case 4: fixture.enums[0].names.data = nullptr; break;
        case 5: fixture.enums[0].names.data = reinterpret_cast<void*>(UINTPTR_MAX - 7); break;
        case 6: fixture.enums[0].names.data = reinterpret_cast<void*>(0x1000); break;
        case 7: fixture.enums[0].names.capacity = 65537; break;
        }
        CHECK(run(fixture, snapshot).code == Error::InvalidEvidence);
        CHECK(!snapshot.offsets.contains(field)); CHECK(!snapshot.fieldReports.at(field).rejected.empty());
    }
    {
        OwnedEnums fixture; auto snapshot = fixture.initial(); CHECK(run(fixture, snapshot)); fixture.duplicate();
        CHECK(run(fixture, snapshot).code == Error::InvalidEvidence);
        CHECK(snapshot.fieldReports.at(field).candidates.size() == 2);
        CHECK(snapshot.offsets.at(field).validation == Validation::Stale);
    }
    {
        OwnedEnums fixture; auto snapshot = fixture.initial(); auto samples = fixture.samples();
        for (unsigned i = 0; i < 3; ++i) {
            samples[i].values = samples[0].values; samples[i].values[0].identity = "alias-array:" + std::to_string(i);
            fixture.setHeader(i, fixture.entries[0].data(), 1, 3);
        }
        ReadBudget budget; budget.generation = 1;
        CHECK(probeEnumNames(fixture, fixture.profile(), samples, fixture.phase2.phase1.nameLayout,
            fixture.phase2.phase1.address(64), fixture.phase2.phase1.pool, budget, snapshot).code == Error::InvalidEvidence);
        CHECK(!snapshot.offsets.contains(field));
    }
}
void providerFailures() {
    for (const auto error : {Error::PermissionDenied, Error::ShortRead, Error::Internal, Error::Unsupported}) {
        OwnedEnums fixture; auto snapshot = fixture.initial(); fixture.failure = error;
        CHECK(run(fixture, snapshot).code == error); CHECK(!snapshot.offsets.contains(field));
    }
    for (unsigned mode = 0; mode < 6; ++mode) {
        OwnedEnums fixture; auto snapshot = fixture.initial(); ReadBudget budget; budget.generation = 1;
        std::atomic<bool> cancelled{false}; budget.cancelled = &cancelled;
        auto expected = Error::Cancelled;
        switch (mode) {
        case 0: cancelled = true; break;
        case 1: budget.deadline = std::chrono::steady_clock::time_point::min(); expected = Error::DeadlineExceeded; break;
        case 2: budget.remainingBytes = 1; expected = Error::BudgetExceeded; break;
        case 3: fixture.beforeRead = [&](auto&, auto, auto) { cancelled = true; }; break;
        case 4: fixture.beforeRead = [&](auto& value, auto, auto) { ++value.epoch; }; expected = Error::StaleIdentity; break;
        case 5: fixture.shortRead = true; expected = Error::ShortRead; break;
        }
        CHECK(run(fixture, snapshot, &budget).code == expected); CHECK(!snapshot.offsets.contains(field));
    }
}
void userOverrides() {
    for (unsigned mode = 0; mode < 8; ++mode) {
        OwnedEnums fixture; auto snapshot = fixture.initial(); CHECK(run(fixture, snapshot));
        auto value = snapshot.offsets.at(field); value.origin = Origin::User;
        CHECK(publishOffset(snapshot, field, std::move(value))); auto& override = snapshot.offsets.at(field);
        switch (mode) {
        case 1: override.value = fixture.duplicateOffset(); break;
        case 2: override.validation = Validation::Candidate; break;
        case 3: override.evidence.clear(); break;
        case 4: override.evidence.front().passed = false; break;
        case 5: override.dependencies.clear(); break;
        case 6: override.dependencies.begin()->second++; break;
        case 7: override.evidence.front().sampleIdentities.clear(); break;
        }
        const auto version = override.version; const auto offset = override.value; const auto validation = override.validation;
        CHECK(run(fixture, snapshot).code == (mode ? Error::InvalidEvidence : Error::None));
        const auto& retained = snapshot.offsets.at(field);
        CHECK(retained.origin == Origin::User && retained.value == offset && retained.validation == validation && retained.version == version);
    }
}
void finalReadback() {
    for (unsigned mode = 0; mode < 8; ++mode) {
        OwnedEnums fixture; auto snapshot = fixture.initial(); ReadBudget budget; budget.generation = 1;
        std::atomic<bool> cancelled{false}; budget.cancelled = &cancelled;
        unsigned rawReads = 0, headerReads = 0, nameReads = 0;
        bool injected = false;
        fixture.beforeRead = [&](auto& reader, std::uintptr_t address, std::size_t size) {
            if (address == reader.entryAddress(0, 0) && size == sizeof(OwnedEnumEntry)) {
                ++rawReads;
                if ((mode == 0 && rawReads == 2) || (mode == 1 && rawReads == 3)) {
                    reader.setValue(0, 0, 8); injected = true;
                }
                if (mode == 6 && rawReads == 2) {
                    ++snapshot.offsets.at("UField::Next").version; injected = true;
                }
                if (mode == 7 && rawReads == 2) { ++reader.epoch; injected = true; }
            }
            if (address == reader.object(0) + reader.namesOffset() && size == sizeof(OwnedEnumArray)) {
                ++headerReads;
                if ((mode == 2 && headerReads == 2) || (mode == 3 && headerReads == 3)) {
                    reader.enums[0].names.capacity = 2; injected = true;
                }
                if (mode == 5 && headerReads == 3) { cancelled = true; injected = true; }
            }
            if (address == reader.phase2.phase1.address(512 + 64 * 2 + 2) && ++nameReads == 3 && mode == 4) {
                reader.phase2.phase1.template put<char>(512 + 64 * 2 + 2, 'X'); injected = true;
            }
        };
        const auto status = run(fixture, snapshot, &budget);
        if (!injected) std::fprintf(stderr, "Enum readback mode %u: raw=%u header=%u name=%u status=%d\n", mode, rawReads, headerReads, nameReads, int(status.code));
        CHECK(injected);
        CHECK(status.code == (mode == 5 ? Error::Cancelled : mode == 7 ? Error::StaleIdentity : Error::InvalidEvidence));
        CHECK(!snapshot.offsets.contains(field));
    }
}
void evidenceAndMetadataBudgets() {
    for (unsigned mode = 0; mode < 5; ++mode) {
        OwnedEnums fixture; auto snapshot = fixture.initial();
        auto leaf = snapshot.offsets.at("UObject::InternalIndex"); leaf.dependencies.clear(); leaf.version = mode == 0 ? 2 : 1;
        auto middle = leaf; middle.version = 1; middle.dependencies["Leaf"] = 1;
        if (mode == 1) leaf.dependencies["Middle"] = 1;
        if (mode == 2) leaf.validation = Validation::Candidate;
        if (mode == 3) leaf.evidence.front().sampleIdentities.clear();
        snapshot.offsets["Leaf"] = leaf; snapshot.offsets["Middle"] = middle;
        if (mode == 4) snapshot.offsets["Leaf"].validation = Validation::Stale;
        else snapshot.offsets.at("UField::Next").dependencies["Middle"] = 1;
        const auto status = run(fixture, snapshot);
        CHECK(status.code == (mode == 4 ? Error::None : Error::InvalidEvidence));
        CHECK(snapshot.offsets.contains(field) == (mode == 4));
    }
    {
        OwnedEnums fixture; auto snapshot = fixture.initial(); auto samples = fixture.samples();
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i].values.clear();
            for (unsigned j = 0; j < 4096; ++j) samples[i].values.push_back({
                "Name" + std::to_string(j) + std::string(180, 'N'),
                "Identity" + std::to_string(i) + ":" + std::to_string(j) + std::string(180, 'I'), j});
        }
        ReadBudget budget; budget.generation = 1;
        CHECK(probeEnumNames(fixture, fixture.profile(), samples, fixture.phase2.phase1.nameLayout,
            fixture.phase2.phase1.address(64), fixture.phase2.phase1.pool, budget, snapshot).code == Error::BudgetExceeded);
        CHECK(fixture.reads == 0 && !snapshot.offsets.contains(field));
    }
    {
        OwnedEnums fixture; auto snapshot = fixture.initial();
        fixture.beforeRead = [](auto&, auto, auto) { throw std::bad_alloc(); };
        CHECK(run(fixture, snapshot).code == Error::Internal); CHECK(!snapshot.offsets.contains(field));
    }
}
void reportBudget() {
    struct RepeatedHeaders final : MemoryReader {
        OwnedEnums source;
        struct alignas(8) Storage { std::array<std::byte, 4096> bytes; };
        std::vector<Storage> objects{16};
        std::vector<OwnedEnumEntry> values{16};
        std::size_t reads = 0;
        RepeatedHeaders() {
            for (std::size_t i = 0; i < objects.size(); ++i) {
                objects[i].bytes.fill(std::byte{0xff});
                std::memcpy(objects[i].bytes.data(), &source.phase2.fields[i % 3], sizeof(OwnedNativeField));
                values[i] = source.entries[0][0];
                const OwnedEnumArray header{&values[i], 1, 1};
                for (std::size_t offset = sizeof(OwnedNativeField); offset + sizeof(header) <= 4096; offset += sizeof(header))
                    std::memcpy(objects[i].bytes.data() + offset, &header, sizeof(header));
            }
        }
        std::uint64_t generation() const override { return 1; }
        ReadResult read(std::uintptr_t address, std::span<std::byte> destination) override {
            ++reads;
            const auto copy = [&](const auto& object) {
                const auto start = reinterpret_cast<std::uintptr_t>(&object);
                if (address < start || address - start > sizeof(object) || destination.size() > sizeof(object) - (address - start)) return false;
                std::memcpy(destination.data(), reinterpret_cast<const std::byte*>(&object) + address - start, destination.size()); return true;
            };
            for (const auto& object : objects) if (copy(object)) return {destination.size(), Error::None};
            for (const auto& value : values) if (copy(value)) return {destination.size(), Error::None};
            return source.read(address, destination);
        }
    } fixture;
    auto snapshot = fixture.source.initial(); auto profile = fixture.source.profile(); profile.extent = 4096;
    std::vector<EnumSample> samples;
    for (std::size_t i = 0; i < fixture.objects.size(); ++i) samples.push_back({
        reinterpret_cast<std::uintptr_t>(&fixture.objects[i]), "repeated-enum:" + std::to_string(i), {
            {OwnedEnums::entryName(0, 0), "entry:" + std::to_string(i) + std::string(900, 'I'), OwnedEnums::values[0][0]}}});
    ReadBudget budget; budget.generation = 1; budget.remainingBytes = 16 * 1024 * 1024;
    CHECK(probeEnumNames(fixture, profile, samples, fixture.source.phase2.phase1.nameLayout,
        fixture.source.phase2.phase1.address(64), fixture.source.phase2.phase1.pool, budget, snapshot).code == Error::BudgetExceeded);
    CHECK(!snapshot.offsets.contains(field));
    CHECK(snapshot.fieldReports.at(field).candidates.size() > 100);
    CHECK(fixture.reads > 1000 && budget.remainingBytes > 0);
}
void cancelledRepeat() {
    for (bool deadline : {false, true}) {
        OwnedEnums fixture; auto snapshot = fixture.initial(); CHECK(run(fixture, snapshot));
        ReadBudget budget; budget.generation = 1; std::atomic<bool> cancelled{!deadline}; budget.cancelled = &cancelled;
        if (deadline) budget.deadline = std::chrono::steady_clock::time_point::min();
        CHECK(run(fixture, snapshot, &budget).code == (deadline ? Error::DeadlineExceeded : Error::Cancelled));
        CHECK(snapshot.offsets.at(field).validation == Validation::Stale);
    }
}
}
int main() {
    validLayouts(); invalidConfiguration(); metadataFailures(); corruptArrays(); providerFailures(); userOverrides(); finalReadback(); evidenceAndMetadataBudgets(); reportBudget(); cancelledRepeat();
    std::printf("PASS: %u enum checks; explicit array/FName/int64 metadata, actual Phase 1/2 evidence, aliases and signed boundaries\n", checks);
}

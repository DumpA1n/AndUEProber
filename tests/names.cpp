#include "andueprober/Names.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>

#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
using namespace andueprober;
struct Names final : MemoryReader {
    std::array<std::byte, 2048> bytes;
    NamePoolProfile profile;
    NameLayout layout{0, {}, 4, 8};
    std::array<NameSample, 2> samples{{{1536, "Object", "owned-class"}, {1600, "/Script/CoreUObject", "owned-package"}}};
    ReadBudget budget;
    std::uint64_t epoch = 1;
    std::function<void(Names&, std::uintptr_t)> beforeRead;
    Error failure = Error::None;
    bool shortRead = false;
    Names() {
        bytes.fill(std::byte{0xff});
        profile.identity = "owned-name-pool-v1"; profile.blocks = 0; profile.header = 0; profile.string = 2;
        profile.blockBits = 8; profile.maximumBlocks = 2;
        put<std::uintptr_t>(32, 256); put<std::uintptr_t>(40, 768);
        text(0, "Object"); text(16, "/Script/CoreUObject"); text(64, "OBJECT");
        put<std::uint32_t>(1544, 0); put<std::uint32_t>(1548, 0);
        put<std::uint32_t>(1608, 16); put<std::uint32_t>(1612, 0);
        budget.generation = epoch;
    }
    template<class T> void put(std::uintptr_t address, T value) {
        REQUIRE(address <= bytes.size() && sizeof(value) <= bytes.size() - address);
        std::memcpy(bytes.data() + address, &value, sizeof(value));
    }
    void text(std::uint32_t id, const std::string& name) {
        put<std::uint16_t>(256 + id * 2, static_cast<std::uint16_t>(name.size() << 6));
        std::memcpy(bytes.data() + 258 + id * 2, name.data(), name.size());
    }
    ReadResult read(std::uintptr_t address, std::span<std::byte> output) override {
        if (beforeRead) beforeRead(*this, address);
        if (failure != Error::None) return {0, failure, 13};
        if (address > bytes.size() || output.size() > bytes.size() - address) return {0, Error::Unmapped};
        std::memcpy(output.data(), bytes.data() + address, output.size());
        return {shortRead && !output.empty() ? output.size() - 1 : output.size(), Error::None};
    }
    std::uint64_t generation() const override { return epoch; }
    Status name(std::uint32_t id, std::string& out) { return readPoolName(*this, 32, id, profile, budget, out); }
};
int main() {
    {
        Names fixture; FieldProbeReport report;
        auto& candidates = report.candidates;
        REQUIRE(probeNameField(fixture, fixture.samples, 24, fixture.layout, 32, fixture.profile, fixture.budget, report));
        REQUIRE(candidates.size() == 1 && candidates.front().value == 8 && candidates.front().validation == Validation::Validated);
        REQUIRE(candidates.front().evidence[0].source == nameObservationIdentity(fixture.layout, fixture.profile, 32, fixture.epoch));
        REQUIRE(candidates.front().evidence[0].sampleIdentities.size() == 2);
        Snapshot initial; initial.sessionId = "owned-names"; initial.moduleIdentity = "owned-module"; initial.layout = Layout::FField;
        initial.layoutIdentity = nameLayoutIdentity(fixture.layout, fixture.profile);
        Session session(initial);
        REQUIRE(session.start([&](Snapshot& snapshot, const std::atomic<bool>& cancelled) {
            fixture.budget.cancelled = &cancelled;
            FieldProbeReport result;
            if (auto status = probeNameField(fixture, fixture.samples, 24, fixture.layout, 32, fixture.profile, fixture.budget, result); !status) return status;
            return publishOffset(snapshot, "UObject::NamePrivate", result.candidates.front());
        }));
        while (session.snapshot()->state == TaskState::Running) std::this_thread::yield();
        REQUIRE(session.stop());
        auto frozen = session.snapshot(); REQUIRE(frozen->state == TaskState::Succeeded); REQUIRE(validateSnapshot(*frozen));
        auto edited = *frozen;
        Offset dependent = candidates.front(); dependent.dependencies["UObject::NamePrivate"] = 1;
        REQUIRE(publishOffset(edited, "UClass::ClassPrivate", dependent));
        Offset user; user.value = 0; user.origin = Origin::User;
        REQUIRE(publishOffset(edited, "UObject::NamePrivate", user));
        REQUIRE(edited.offsets.at("UClass::ClassPrivate").validation == Validation::Stale);
        REQUIRE(frozen->offsets.at("UObject::NamePrivate").value == 8);
    }
    {
        Names fixture; FieldProbeReport first, second, reloaded;
        const auto layout = nameLayoutIdentity(fixture.layout, fixture.profile);
        REQUIRE(probeNameField(fixture, fixture.samples, 24, fixture.layout, 32, fixture.profile, fixture.budget, first));
        fixture.put<std::uintptr_t>(64, 256); fixture.put<std::uintptr_t>(72, 768);
        REQUIRE(probeNameField(fixture, fixture.samples, 24, fixture.layout, 64, fixture.profile, fixture.budget, second));
        REQUIRE(first.candidates.front().value == second.candidates.front().value);
        REQUIRE(first.candidates.front().evidence[0].source != second.candidates.front().evidence[0].source);
        REQUIRE(second.candidates.front().evidence[0].source == nameObservationIdentity(fixture.layout, fixture.profile, 64, 1));
        fixture.budget.generation = ++fixture.epoch;
        REQUIRE(probeNameField(fixture, fixture.samples, 24, fixture.layout, 64, fixture.profile, fixture.budget, reloaded));
        REQUIRE(second.candidates.front().evidence[0].source != reloaded.candidates.front().evidence[0].source);
        REQUIRE(reloaded.candidates.front().evidence[0].source == nameObservationIdentity(fixture.layout, fixture.profile, 64, 2));
        REQUIRE(nameLayoutIdentity(fixture.layout, fixture.profile) == layout);
        auto bounded = fixture.profile; bounded.maximumUnits = 128;
        REQUIRE(nameLayoutIdentity(fixture.layout, bounded) != layout);
    }
    {
        Names fixture; std::string name;
        fixture.put<std::uint32_t>(1548, 65537);
        REQUIRE(readFName(fixture, 1544, fixture.layout, 32, fixture.profile, fixture.budget, name));
        REQUIRE(name == "Object_65536");
        fixture.put<std::uint32_t>(1548, std::numeric_limits<std::int32_t>::max());
        REQUIRE(readFName(fixture, 1544, fixture.layout, 32, fixture.profile, fixture.budget, name));
        REQUIRE(name == "Object_2147483646");
        for (const auto invalid : {std::uint32_t{0x80000000}, std::numeric_limits<std::uint32_t>::max()}) {
            fixture.put<std::uint32_t>(1548, invalid);
            REQUIRE(readFName(fixture, 1544, fixture.layout, 32, fixture.profile, fixture.budget, name).code == Error::InvalidEvidence);
            REQUIRE(name.empty());
        }
        fixture.layout.display = 4; fixture.layout.number = 8; fixture.layout.size = 12;
        fixture.put<std::uint32_t>(1548, 64); fixture.put<std::uint32_t>(1552, 2);
        REQUIRE(readFName(fixture, 1544, fixture.layout, 32, fixture.profile, fixture.budget, name));
        REQUIRE(name == "OBJECT_1");
        fixture.put<std::uint32_t>(1544, 512);
        REQUIRE(readFName(fixture, 1544, fixture.layout, 32, fixture.profile, fixture.budget, name).code == Error::InvalidEvidence);
        REQUIRE(name.empty());
        fixture.put<std::uint32_t>(1544, 0);
        fixture.layout.display = 0;
        REQUIRE(readFName(fixture, 1544, fixture.layout, 32, fixture.profile, fixture.budget, name).code == Error::InvalidArgument);
        REQUIRE(name.empty());
    }
    {
        Names fixture; std::string name;
        fixture.put<std::uint16_t>(320, (3 << 6) | 1);
        fixture.put<char16_t>(322, u'\u4e2d'); fixture.put<char16_t>(324, 0xd83d); fixture.put<char16_t>(326, 0xde00);
        REQUIRE(fixture.name(32, name)); REQUIRE(name == "\xe4\xb8\xad\xf0\x9f\x98\x80");
        fixture.put<char16_t>(326, u'A'); REQUIRE(fixture.name(32, name).code == Error::InvalidEvidence); REQUIRE(name.empty());
        fixture.text(32, "\xe4\xb8\xad");
        REQUIRE(fixture.name(32, name).code == Error::Unsupported);
        fixture.profile.narrowEncoding = NarrowEncoding::Utf8;
        REQUIRE(fixture.name(32, name) && name == "\xe4\xb8\xad");
        for (const auto invalid : {"\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe4\xb8"}) {
            fixture.text(32, invalid); REQUIRE(fixture.name(32, name).code == Error::InvalidEvidence);
        }
        fixture.text(32, std::string("A\0B", 3)); REQUIRE(fixture.name(32, name).code == Error::InvalidEvidence);
    }
    {
        Names fixture; std::string name;
        REQUIRE(fixture.name(512, name).code == Error::InvalidEvidence);
        fixture.put<std::uint16_t>(766, (4 << 6));
        REQUIRE(fixture.name(255, name).code == Error::InvalidEvidence);
        fixture.profile.header = 4; fixture.profile.string = 6;
        REQUIRE(fixture.name(255, name).code == Error::InvalidEvidence);
        fixture.profile.outlineNumbers = true;
        REQUIRE(fixture.name(0, name).code == Error::Unsupported);
    }
    {
        Names fixture; std::string name;
        fixture.failure = Error::PermissionDenied;
        auto status = fixture.name(0, name); REQUIRE(status.code == Error::PermissionDenied && status.message.find("13") != std::string::npos);
        fixture.failure = Error::None; fixture.shortRead = true;
        REQUIRE(fixture.name(0, name).code == Error::ShortRead);
        fixture.shortRead = false; fixture.budget.remainingBytes = 1;
        REQUIRE(fixture.name(0, name).code == Error::BudgetExceeded);
        fixture.budget.remainingBytes = 4096;
        std::atomic<bool> cancelled{true}; fixture.budget.cancelled = &cancelled;
        REQUIRE(fixture.name(0, name).code == Error::Cancelled);
        cancelled = false; fixture.budget.deadline = std::chrono::steady_clock::now();
        REQUIRE(fixture.name(0, name).code == Error::DeadlineExceeded);
    }
    {
        Names fixture; std::string name;
        fixture.put<std::uintptr_t>(32, std::numeric_limits<std::uintptr_t>::max() - 1);
        REQUIRE(fixture.name(16, name).code == Error::Overflow);
    }
    for (int mutation = 0; mutation < 4; ++mutation) {
        Names fixture; std::string name; int payloads = 0;
        fixture.beforeRead = [&](Names& memory, auto address) {
            if (address != 258 || ++payloads != (mutation >= 2 ? 1 : 2)) return;
            if (mutation == 0) memory.put<char>(258, 'X');
            if (mutation == 1) ++memory.epoch;
            if (mutation == 2) memory.put<std::uint16_t>(256, 0);
            if (mutation == 3) memory.put<std::uintptr_t>(32, 768);
        };
        const auto status = fixture.name(0, name);
        REQUIRE(!status && name.empty());
    }
    {
        Names fixture; FieldProbeReport report;
        auto& candidates = report.candidates;
        fixture.put<std::uint32_t>(1552, 0); fixture.put<std::uint32_t>(1556, 0);
        fixture.put<std::uint32_t>(1616, 16); fixture.put<std::uint32_t>(1620, 0);
        REQUIRE(probeNameField(fixture, fixture.samples, 24, fixture.layout, 32, fixture.profile, fixture.budget, report));
        REQUIRE(candidates.size() == 2 && candidates[0].validation == Validation::Candidate);
        fixture.samples[1].object = fixture.samples[0].object;
        REQUIRE(probeNameField(fixture, fixture.samples, 24, fixture.layout, 32, fixture.profile, fixture.budget, report).code == Error::InvalidArgument);
    }
    std::puts("PASS: bounded names, Unicode, FName layouts, declared anchors, sessions and override invalidation; no engine calls");
}

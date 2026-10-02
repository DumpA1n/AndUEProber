#include "OwnedPropertyBases.hpp"
#include "andueprober/Evidence.hpp"
#include <cstdlib>
#include <iostream>
using namespace andueprober;
#define CHECK(value) do { if (!(value)) { std::cerr << "Property-base check failed at " << __LINE__ << ": " #value "\n"; std::abort(); } } while (0)
int main() {
    OwnedPropertyBases fixture;
    auto snapshot = fixture.initial();
    auto pointers = fixture.pointerSamples();
    auto booleans = fixture.boolSamples();
    ReadBudget budget; budget.generation = fixture.epoch;
    const auto status = probePropertyBases(fixture, fixture.profile(), pointers, booleans, budget, snapshot);
    CHECK(status);
    CHECK(snapshot.offsets.at("sizeof(FProperty)").value == fixture.size);
    CHECK(snapshot.offsets.at("FProperty::SubPropertyBase").value == fixture.subPropertyBase);
    CHECK(snapshot.offsets.at("sizeof(FProperty)").validation == Validation::Validated);
    CHECK(snapshot.offsets.at("FProperty::SubPropertyBase").validation == Validation::Validated);
    CHECK(validateSnapshot(snapshot));

    auto adjacent = fixture.initial();
    auto broadProfile = fixture.profile();
    broadProfile.extent = 4096;
    budget = {}; budget.generation = fixture.epoch;
    CHECK(probePropertyBases(fixture, broadProfile, pointers, booleans, budget, adjacent));
    CHECK(adjacent.offsets.at("FProperty::SubPropertyBase").value == fixture.subPropertyBase);

    OwnedPropertyBases ambiguous;
    std::memcpy(ambiguous.tails[0].data() + fixture.subPropertyBase + 8, &ambiguous.expected[0], 8);
    std::memcpy(ambiguous.tails[1].data() + fixture.subPropertyBase + 8, &ambiguous.expected[1], 8);
    auto failed = ambiguous.initial();
    auto ambiguousPointers = ambiguous.pointerSamples();
    auto ambiguousBools = ambiguous.boolSamples();
    budget = {}; budget.generation = ambiguous.epoch;
    CHECK(probePropertyBases(ambiguous, ambiguous.profile(), ambiguousPointers, ambiguousBools, budget, failed).code == Error::InvalidEvidence);
    CHECK(!failed.offsets.contains("sizeof(FProperty)"));
    CHECK(!failed.offsets.contains("FProperty::SubPropertyBase"));

    // DeltaForce stores one byte before FieldSize that reads 1 on every Bool; the
    // quartet starts at sizeof(FProperty) + 1 and the base stays aligned below it.
    OwnedPropertyBases shifted;
    constexpr std::array<std::uint8_t, 5> leadingByteBool{1, 1, 0, 1, 0xff};
    for (std::size_t index = 2; index < 4; ++index) {
        shifted.tails[index].fill(std::byte{0});
        std::memcpy(shifted.tails[index].data() + shifted.size, leadingByteBool.data(), leadingByteBool.size());
    }
    auto shiftedSnapshot = shifted.initial();
    auto shiftedPointers = shifted.pointerSamples();
    auto shiftedBools = shifted.boolSamples();
    budget = {}; budget.generation = shifted.epoch;
    CHECK(probePropertyBases(shifted, shifted.profile(), shiftedPointers, shiftedBools, budget, shiftedSnapshot));
    const auto& shiftedSize = shiftedSnapshot.offsets.at("sizeof(FProperty)");
    CHECK(shiftedSize.value == shifted.size);
    CHECK(shiftedSize.evidence.front().relativeAddresses.front() == shifted.size + 1);

    // Every NativeBool anchor must place the quartet at the same offset.
    OwnedPropertyBases disagreeing;
    disagreeing.tails[3].fill(std::byte{0});
    std::memcpy(disagreeing.tails[3].data() + disagreeing.size, leadingByteBool.data(), leadingByteBool.size());
    auto disagreement = disagreeing.initial();
    auto disagreeingPointers = disagreeing.pointerSamples();
    auto disagreeingBools = disagreeing.boolSamples();
    budget = {}; budget.generation = disagreeing.epoch;
    CHECK(probePropertyBases(disagreeing, disagreeing.profile(), disagreeingPointers, disagreeingBools, budget, disagreement).code == Error::InvalidEvidence);
    CHECK(!disagreement.offsets.contains("sizeof(FProperty)"));

    auto denied = fixture.initial();
    fixture.failure = Error::PermissionDenied;
    budget = {}; budget.generation = fixture.epoch;
    CHECK(probePropertyBases(fixture, fixture.profile(), pointers, booleans, budget, denied).code == Error::PermissionDenied);

    OwnedPropertyBases mutableFixture;
    auto changed = mutableFixture.initial();
    auto mutablePointers = mutableFixture.pointerSamples();
    auto mutableBools = mutableFixture.boolSamples();
    std::size_t targetReads = 0;
    const auto target = mutablePointers[0].object + mutableFixture.subPropertyBase;
    mutableFixture.beforeRead = [&](auto& source, auto address, auto width) {
        if (address == target && width == 8 && ++targetReads == 2) source.tails[0][source.subPropertyBase] ^= std::byte{1};
    };
    budget = {}; budget.generation = mutableFixture.epoch;
    CHECK(probePropertyBases(mutableFixture, mutableFixture.profile(), mutablePointers, mutableBools, budget, changed).code == Error::InvalidEvidence);
    CHECK(!changed.offsets.contains("sizeof(FProperty)"));
    CHECK(!changed.offsets.contains("FProperty::SubPropertyBase"));
    std::cout << "PASS: FProperty size and SubPropertyBase discovery, shifted and disagreeing NativeBool anchors, "
        "ambiguity, read failure and final mutation rejection\n";
}

#include "OwnedFunctions.hpp"
#include <andueprober/Evidence.hpp>

#include <cstdio>
#include <cstdlib>

using namespace andueprober;
namespace {
unsigned checks{};
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
void addClosure(Snapshot& snapshot) {
    auto leaf = snapshot.offsets.at("UObject::InternalIndex");
    leaf.dependencies.clear(); leaf.version = 1;
    snapshot.offsets["ReviewLeaf"] = leaf;
    auto middle = leaf; middle.dependencies["ReviewLeaf"] = 1;
    snapshot.offsets["ReviewMiddle"] = middle;
    snapshot.offsets.at("UObject::InternalIndex").dependencies["ReviewMiddle"] = 1;
}
template<class Fixture, class Observe>
void exercise(Observe observe, std::array<std::string, 2> outputs) {
    for (const auto layout : {Layout::FField, Layout::UProperty}) {
        for (unsigned mode = 0; mode < 7; ++mode) {
            Fixture fixture; auto snapshot = fixture.initial(layout); addClosure(snapshot);
            auto& leaf = snapshot.offsets.at("ReviewLeaf");
            switch (mode) {
            case 0: leaf.validation = Validation::Candidate; break;
            case 1: leaf.validation = Validation::Stale; break;
            case 2: leaf.evidence.clear(); break;
            case 3: leaf.dependencies["ReviewMiddle"] = 1; break;
            case 4: leaf.dependencies["Missing"] = 1; break;
            case 5: ++leaf.version; break;
            case 6: leaf.evidence[0].sampleIdentities.pop_back(); break;
            }
            CHECK(observe(fixture, snapshot).code == Error::InvalidEvidence);
            CHECK(!snapshot.offsets.contains(outputs[0]));
        }
        {
            Fixture fixture; auto snapshot = fixture.initial(layout); addClosure(snapshot);
            bool changed = false;
            fixture.beforeRead = [&](auto&, auto, auto) {
                changed = true;
                snapshot.offsets.at("ReviewLeaf").validation = Validation::Candidate;
            };
            CHECK(observe(fixture, snapshot).code == Error::InvalidEvidence);
            CHECK(changed && !snapshot.offsets.contains(outputs[0]));
        }
        {
            Fixture fixture; auto snapshot = fixture.initial(layout);
            auto unrelated = snapshot.offsets.at("UObject::InternalIndex");
            unrelated.validation = Validation::Stale;
            unrelated.evidence.clear();
            snapshot.offsets["UnrelatedStale"] = unrelated;
            CHECK(observe(fixture, snapshot));
            CHECK(snapshot.offsets.at(outputs[0]).validation == Validation::Validated);
            CHECK(snapshot.offsets.at("UnrelatedStale").validation == Validation::Stale);
        }
        {
            Fixture fixture; auto snapshot = fixture.initial(layout);
            CHECK(observe(fixture, snapshot));
            auto& first = snapshot.offsets.at(outputs[0]);
            auto& second = snapshot.offsets.at(outputs[1]);
            first.origin = second.origin = Origin::User;
            first.dependencies[outputs[1]] = second.version;
            second.dependencies[outputs[0]] = first.version;
            const auto version = first.version;
            CHECK(observe(fixture, snapshot).code == Error::InvalidEvidence);
            CHECK(snapshot.offsets.at(outputs[0]).version == version);
            CHECK(snapshot.offsets.at(outputs[0]).origin == Origin::User);
        }
    }
}
}
int main() {
    exercise<OwnedStructs>([](auto& fixture, auto& snapshot) {
        ReadBudget budget; budget.generation = fixture.epoch;
        return probeStructFields(fixture, fixture.profile(snapshot.layout), fixture.structSamples(), fixture.fieldSamples(), budget, snapshot);
    }, {"UField::Next", "UStruct::PropertiesSize"});
    exercise<OwnedClasses>([](auto& fixture, auto& snapshot) {
        ReadBudget budget; budget.generation = fixture.epoch;
        return probeClassFields(fixture, fixture.profile(snapshot.layout), fixture.samples(), budget, snapshot);
    }, {"UClass::CastFlags", "UClass::ClassDefaultObject"});
    exercise<OwnedFunctions>([](auto& fixture, auto& snapshot) {
        ReadBudget budget; budget.generation = fixture.epoch;
        return probeFunctionFields(fixture, fixture.profile(snapshot.layout), fixture.samples(), budget, snapshot);
    }, {"UFunction::FunctionFlags", "UFunction::NumParms"});
    CHECK(ownedNativeFunctionCalls.load() == 0);
    std::printf("PASS: %u actual phase evidence closure checks\n", checks);
}

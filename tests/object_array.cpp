#include "OwnedObjectArray.hpp"
#include <cstdio>
#include <limits>

#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
using namespace andueprober;
int main() {
    for (bool chunked : {false, true}) {
        OwnedObjectArray fixture(chunked);
        ReadBudget budget; budget.generation = fixture.epoch;
        std::vector<Offset> values;
        REQUIRE(probeObjectArrayIndices(fixture, fixture.Array, fixture.profile, 32, budget, values));
        REQUIRE(values.size() == 1 && values[0].value == 0 && values[0].validation == Validation::Validated);
        REQUIRE(values[0].evidence.size() == 2 && values[0].evidence[1].source == fixture.profile.identity);
        REQUIRE(values[0].evidence[0].sampleIdentities.size() == 4);
        std::uintptr_t object = 1;
        REQUIRE(readObjectAt(fixture, fixture.Array, fixture.profile, 4, budget, object).code == Error::InvalidArgument);
        REQUIRE(object == 0);
        fixture.put<std::int32_t>(fixture.Array + 8, -1);
        REQUIRE(probeObjectArrayIndices(fixture, fixture.Array, fixture.profile, 32, budget, values).code == Error::InvalidEvidence);
        REQUIRE(values.empty());
    }
    {
        OwnedObjectArray fixture(true); ReadBudget budget; budget.generation = fixture.epoch;
        std::vector<Offset> values;
        fixture.put<std::int32_t>(fixture.Array + 16, 1);
        REQUIRE(probeObjectArrayIndices(fixture, fixture.Array, fixture.profile, 32, budget, values).code == Error::InvalidEvidence);
    }
    {
        OwnedObjectArray fixture(false); ReadBudget budget; budget.generation = fixture.epoch;
        std::vector<Offset> values;
        fixture.put<std::uintptr_t>(256 + 16 + *fixture.profile.itemObject, 1024);
        REQUIRE(probeObjectArrayIndices(fixture, fixture.Array, fixture.profile, 32, budget, values).code == Error::InvalidEvidence);
        REQUIRE(values.empty());
    }
    {
        OwnedObjectArray fixture(false); ReadBudget budget; budget.generation = fixture.epoch;
        std::vector<Offset> values;
        fixture.put<std::uintptr_t>(fixture.Array, std::numeric_limits<std::uintptr_t>::max() - 3);
        REQUIRE(probeObjectArrayIndices(fixture, fixture.Array, fixture.profile, 32, budget, values).code == Error::Overflow);
    }
    {
        OwnedObjectArray fixture(false); ReadBudget budget; budget.generation = fixture.epoch;
        std::vector<Offset> values;
        bool readObject = false;
        fixture.beforeRead = [&readObject](auto& memory, auto address) {
            if (address >= 1024) readObject = true;
            if (readObject && address == memory.Array + 8) memory.template put<std::int32_t>(address, 3);
        };
        REQUIRE(probeObjectArrayIndices(fixture, fixture.Array, fixture.profile, 32, budget, values).code == Error::InvalidEvidence);
        REQUIRE(values.empty());
    }
    {
        OwnedObjectArray fixture(false); ReadBudget budget; budget.generation = fixture.epoch;
        std::vector<Offset> values;
        fixture.beforeRead = [](auto& memory, auto) { if (memory.reads == 5) ++memory.epoch; };
        REQUIRE(probeObjectArrayIndices(fixture, fixture.Array, fixture.profile, 32, budget, values).code == Error::StaleIdentity);
        REQUIRE(values.empty());
    }
    {
        OwnedObjectArray fixture(false); ReadBudget budget; budget.generation = fixture.epoch;
        std::vector<Offset> values;
        std::atomic<bool> cancelled{true}; budget.cancelled = &cancelled;
        REQUIRE(probeObjectArrayIndices(fixture, fixture.Array, fixture.profile, 32, budget, values).code == Error::Cancelled);
        cancelled = false; budget.remainingBytes = 3;
        REQUIRE(probeObjectArrayIndices(fixture, fixture.Array, fixture.profile, 32, budget, values).code == Error::BudgetExceeded);
        budget.remainingBytes = 4096; budget.deadline = std::chrono::steady_clock::now();
        REQUIRE(probeObjectArrayIndices(fixture, fixture.Array, fixture.profile, 32, budget, values).code == Error::DeadlineExceeded);
    }
}

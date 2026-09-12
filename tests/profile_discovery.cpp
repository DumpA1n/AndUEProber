#include "OwnedDiscovery.hpp"
#include "andueprober/Engine.hpp"
#include "IGameProfileEx.hpp"
#include "PUBG.hpp"
#include "UE/UEGameProfiles/PES.hpp"
#include <cstdlib>
#include <iostream>
#define CHECK(test) do { if (!(test)) { std::cerr << "Profile discovery check failed at " << __LINE__ << "\n"; std::abort(); } } while (0)
int main() {
    OwnedDiscovery memory;
    auto budget = memory.budget();
    andueprober::ModuleImage module;
    CHECK(memory.image(budget, module));
    GameProfileEx<PUBGProfile> profile;
    CHECK(profile.SupportsBoundedDiscovery());
    andueprober::DiscoveryValue value;
    CHECK(profile.FindAdrpXrefToAddr(memory, module, memory.base + 0xff0, budget, value));
    CHECK(value.address == memory.base + 0x3004);
    CHECK(profile.DiscoverObjectArray(memory, module, budget, value));
    CHECK(value.address == memory.base + 0x1900);
    CHECK(profile.FindFNameToString(memory, module, budget, value));
    CHECK(value.address == memory.base + 0x3300);
    CHECK(andueprober::engineTextAdmission(andueprober::EngineSignature::Unknown, {},
        {module.identity, "unconfigured-profile-text", module.generation}).code == andueprober::Error::Unsupported);
    memory.failure = andueprober::Error::PermissionDenied;
    CHECK(profile.DiscoverObjectArray(memory, module, budget, value).code == andueprober::Error::PermissionDenied);
    CHECK(!value.address);
    CHECK(profile.GetGUObjectArrayPtr() == 0 && profile.GetNamesPtr() == 0 && !profile.ArchSupprted());
    CHECK(profile.GetNameByID(0).empty());
    GameProfileEx<PESProfile> upstream;
    const auto reads = memory.reads;
    CHECK(!upstream.SupportsBoundedDiscovery());
    CHECK(upstream.DiscoverObjectArray(memory, module, budget, value).code == andueprober::Error::Unsupported);
    CHECK(memory.reads == reads && !value.address);
    std::cout << "PASS: actual profile adapter consumes bounded discovery; unsupported upstream paths perform no reads\n";
}

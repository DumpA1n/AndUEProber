#include "OwnedDiscovery.hpp"
#include "andueprober/Engine.hpp"
#include "BoundedUEMemory.hpp"
#include "IGameProfileEx.hpp"
#include "PUBG.hpp"
#include "DeltaForce.hpp"
#include "UE/UEGameProfiles/PES.hpp"
#include <cstdlib>
#include <iostream>
#define CHECK(test) do { if (!(test)) { std::cerr << "Profile discovery check failed at " << __LINE__ << "\n"; std::abort(); } } while (0)
namespace {
bool boundedRead(void* context, std::uintptr_t address, void* output, std::size_t size) noexcept {
    auto& memory = *static_cast<OwnedDiscovery*>(context);
    const auto result = memory.read(address, {static_cast<std::byte*>(output), size});
    return result.error == andueprober::Error::None && result.transferred == size;
}
class ExposedDeltaForce final : public DeltaForceProfile {
public:
    using DeltaForceProfile::GetNameEntryString;
};
}
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
    CHECK(profile.GetGUObjectArrayPtr() == 0 && profile.GetNamesPtr() == 0 && profile.ArchSupprted());
    CHECK(profile.GetNameByID(0).empty());
    profile.BindRuntime(memory.base + 0x1900, memory.base + 0x2900);
    CHECK(profile.GetGUObjectArrayPtr() == memory.base + 0x1900 && profile.GetNamesPtr() == memory.base + 0x2900);
    memory.failure = andueprober::Error::None;
    GameProfileEx<DeltaForceProfile> deltaForce;
    CHECK(deltaForce.DiscoverObjectArray(memory, module, budget, value));
    CHECK(value.address == memory.base + 0x1c00);
    CHECK(deltaForce.DiscoverNamePool(memory, module, budget, value));
    CHECK(value.address == memory.base + 0x1a00);
    memory.unreadableStart = memory.base + 0x4000;
    memory.unreadableEnd = memory.base + 0x5000;
    CHECK(deltaForce.DiscoverNamePool(memory, module, budget, value));
    CHECK(value.address == memory.base + 0x1a00);
    memory.unreadableStart = memory.unreadableEnd = 0;
    ExposedDeltaForce decoder;
    const std::string expected = "Object";
    memory.put(0x2000, expected.size() << 6, 2);
    for (std::size_t index = 0; index < expected.size(); ++index)
        memory.put(0x2002 + index, static_cast<std::uint8_t>(~expected[index]), 1);
    UEMemory::SetBoundedReader(&memory, boundedRead);
    CHECK(decoder.GetNameEntryString(reinterpret_cast<std::uint8_t*>(memory.base + 0x2000)) == expected);
    UEMemory::ClearBoundedReader();
    memory.text(0xff0, u"Game engine shut down");
    GameProfileEx<PESProfile> upstream;
    const auto reads = memory.reads;
    CHECK(upstream.SupportsBoundedDiscovery());
    CHECK(upstream.DiscoverObjectArray(memory, module, budget, value));
    CHECK(memory.reads > reads && value.address == memory.base + 0x1900);
    CHECK(upstream.DiscoverNamePool(memory, module, budget, value));
    CHECK(value.address == memory.base + 0x1d00);
    std::cout << "PASS: profile adapters consume bounded discovery and bind operation-owned runtime anchors\n";
}

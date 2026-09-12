#include <andueprober/Core.hpp>
#include <andueprober/Probe.hpp>
#include <andueprober/Names.hpp>
#include <andueprober/Relations.hpp>
#include <andueprober/Commands.hpp>
#include <andueprober/Export.hpp>
#include <andueprober/Agent.h>

int main() {
    andueprober::NameLayout layout{0, {}, 4, 8};
    andueprober::NamePoolProfile profile; profile.identity = "consumer";
    if (andueprober::nameLayoutIdentity(layout, profile).empty()) return 3;
    andueprober::Snapshot initial;
    initial.sessionId = "consumer";
    if (!andueprober::beginFieldProbe(initial, "Index")) return 4;
    andueprober::CommandSession commands(initial, [](const auto&, auto&, const auto&) { return andueprober::Status{}; });
    if (!commands.start() || !commands.stop()) return 5;
    andueprober::Session session(initial);
    if (!session.stop()) return 1;
    return session.start([](auto&, const auto&) { return andueprober::Status{}; }).code == andueprober::Error::Busy ? 0 : 2;
}

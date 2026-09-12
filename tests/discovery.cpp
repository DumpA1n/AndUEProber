#include "OwnedDiscovery.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
using namespace andueprober;
#define CHECK(test) do { if (!(test)) { std::cerr << "Discovery check failed at " << __LINE__ << ": " #test "\n"; std::abort(); } } while (0)
int main() {
    OwnedDiscovery memory;
    auto budget = memory.budget();
    ModuleImage module;
    CHECK(memory.image(budget, module));
    CHECK(module.ranges.size() == 2 && module.generation == 7);
    DiscoveryValue value;
    CHECK(findAdrpReference(memory, module, memory.base + 0xff0, budget, value));
    CHECK(value.address == memory.base + 0x3004);
    CHECK(findObjectArrayCandidate(memory, module, budget, value));
    CHECK(value.address == memory.base + 0x1900 && value.evidence.source == module.identity);
    CHECK(value.evidence.relativeAddresses.back() == 0x1900);
    CHECK(findNameToStringCandidate(memory, module, budget, value));
    CHECK(value.address == memory.base + 0x3300);
    memory.call(0x310c, 0x3080);
    CHECK(findNameToStringCandidate(memory, module, budget, value));
    CHECK(value.address == memory.base + 0x3080); // Negative BL displacement is decoded without signed shifts.
    memory.put(0x1800, memory.base + 0x1980, 8);
    CHECK(findObjectArrayCandidate(memory, module, budget, value));
    CHECK(value.address == memory.base + 0x1980); // Repeated discovery reads current slots, without an address cache.
    {
        auto copy = module; copy.generation++;
        CHECK(findAdrpReference(memory, copy, memory.base, budget, value).code == Error::StaleIdentity && !value.address);
        copy = module; copy.ranges[1].start = copy.ranges[0].start;
        CHECK(findAdrpReference(memory, copy, memory.base, budget, value).code == Error::InvalidEvidence);
        copy = module; copy.ranges[0].start = std::numeric_limits<std::uintptr_t>::max() - 4;
        CHECK(findAdrpReference(memory, copy, memory.base, budget, value).code == Error::Overflow);
    }
    for (auto failure : {Error::ShortRead, Error::PermissionDenied, Error::Unmapped}) {
        memory.failure = failure;
        CHECK(findObjectArrayCandidate(memory, module, budget, value).code == failure && !value.address);
    }
    memory.failure = Error::None;
    {
        auto limited = budget; limited.remainingBytes = 20;
        CHECK(findObjectArrayCandidate(memory, module, limited, value).code == Error::BudgetExceeded);
        std::atomic<bool> cancelled{true}; limited = budget; limited.cancelled = &cancelled;
        CHECK(findObjectArrayCandidate(memory, module, limited, value).code == Error::Cancelled);
        limited = budget; limited.deadline = std::chrono::steady_clock::now();
        CHECK(findObjectArrayCandidate(memory, module, limited, value).code == Error::DeadlineExceeded);
        cancelled = false; limited = budget; limited.cancelled = &cancelled;
        memory.onRead = [&](auto) { cancelled = true; };
        CHECK(findObjectArrayCandidate(memory, module, limited, value).code == Error::Cancelled);
        memory.onRead = [&](auto) { ++memory.epoch; };
        CHECK(findObjectArrayCandidate(memory, module, budget, value).code == Error::StaleIdentity);
        memory.onRead = {}; memory.epoch = module.generation;
    }
    {
        memory.text(0x1400, u"Game engine shut down");
        CHECK(findObjectArrayCandidate(memory, module, budget, value).code == Error::InvalidEvidence);
        memory.put(0x1400, 0, 2);
        memory.reference(0x3400, 0xff0, 8);
        CHECK(findObjectArrayCandidate(memory, module, budget, value).code == Error::InvalidEvidence);
        memory.put(0x3400, 0, 4);
        memory.call(0x3110, 0x3400); memory.put(0x3114, 0xd65f03c0, 4);
        CHECK(findNameToStringCandidate(memory, module, budget, value).code == Error::InvalidEvidence);
        memory.put(0x3110, 0xd65f03c0, 4);
        memory.put(0x300c, 0x14000001, 4);
        CHECK(findObjectArrayCandidate(memory, module, budget, value).code == Error::Unsupported);
        memory.call(0x300c, 0x3200);
        memory.put(0x3010, 0, 4);
        CHECK(!findObjectArrayCandidate(memory, module, budget, value));
        memory.put(0x3010, 0xd65f03c0, 4);
        memory.put(0x3204, 0xf9400000u | (0x800u / 8u << 10) | (12u << 5) | 11, 4);
        CHECK(!findObjectArrayCandidate(memory, module, budget, value));
        memory.put(0x3204, 0xf9400000u | (0x800u / 8u << 10) | (10u << 5) | 11, 4);
        unsigned slotReads = 0;
        memory.onRead = [&](auto address) { if (address == memory.base + 0x1800 && ++slotReads == 2) memory.put(0x1800, memory.base + 0x1900, 8); };
        CHECK(findObjectArrayCandidate(memory, module, budget, value).code == Error::StaleIdentity);
        memory.onRead = {};
    }
    {
        OwnedDiscovery corrupt; auto limit = corrupt.budget(); ModuleImage image;
        corrupt.put(18, 62, 2);
        CHECK(corrupt.image(limit, image).code == Error::Unsupported && image.ranges.empty());
        corrupt.put(18, 183, 2); corrupt.put(56, 65, 2);
        CHECK(corrupt.image(limit, image).code == Error::InvalidEvidence);
        corrupt.put(56, 3, 2); corrupt.segment(176, 0x6000, 0, 4); corrupt.put(176 + 32, 1, 8);
        CHECK(corrupt.image(limit, image).code == Error::InvalidEvidence);
        corrupt.put(56, 2, 2); corrupt.put(120 + 16, 0x1000, 8);
        CHECK(corrupt.image(limit, image).code == Error::InvalidEvidence);
        corrupt.put(120 + 16, 0x3000, 8);
        unsigned headers = 0;
        corrupt.onRead = [&](auto address) { if (address == corrupt.base && ++headers == 2) corrupt.put(56, 1, 2); };
        CHECK(corrupt.image(limit, image).code == Error::StaleIdentity);
    }
    {
        Snapshot initial; initial.sessionId = "owned-discovery"; initial.moduleIdentity = module.identity;
        initial.generation = module.generation; initial.layoutIdentity = "owned-elf-candidate";
        Session session(initial);
        CHECK(session.start([&](Snapshot& snapshot, const std::atomic<bool>& cancelled) {
            auto bounded = memory.budget(); bounded.cancelled = &cancelled;
            DiscoveryValue candidate;
            auto status = findObjectArrayCandidate(memory, module, bounded, candidate); if (!status) return status;
            Offset offset; offset.value = static_cast<std::uint32_t>(*candidate.address - module.loadBias);
            offset.evidence.push_back(candidate.evidence);
            return publishOffset(snapshot, "Module::GUObjectArray", std::move(offset));
        }));
        while (session.snapshot()->state == TaskState::Running || session.snapshot()->state == TaskState::Pending) std::this_thread::yield();
        CHECK(session.stop());
        auto frozen = session.snapshot();
        CHECK(frozen->state == TaskState::Succeeded);
        CHECK(frozen->offsets.at("Module::GUObjectArray").value == 0x1900);
        CHECK(!validateSnapshot(*frozen)); // Instruction candidates are not validated engine layouts.
        memory.put(0x1800, memory.base + 0x1980, 8);
        CHECK(frozen->offsets.at("Module::GUObjectArray").value == 0x1900);
    }
    std::cout << "PASS: owned ELF discovery, instruction candidates, budgets, failures, generations and frozen evidence\n";
}

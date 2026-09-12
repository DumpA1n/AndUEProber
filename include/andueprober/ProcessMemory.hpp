#pragma once
#include "Core.hpp"

namespace andueprober {
// Uses the public AndSwapChainHook::Memory provider. Only normal Android/Linux
// linker modules are supported. A NOLOAD reference prevents ordinary dlclose unload
// during the lease; manually mapped modules and external unmapping are unsupported.
// Open and read calls have one owner. Destruction releases the descriptor and lease.
class ProcessMemory final : public MemoryReader {
public:
    ProcessMemory();
    ~ProcessMemory() override;
    ProcessMemory(const ProcessMemory&) = delete;
    ProcessMemory& operator=(const ProcessMemory&) = delete;
    Status open(const std::string& modulePath, std::uintptr_t addressInModule, ReadBudget* budget = nullptr);
    Status openByName(std::span<const std::string> moduleNames, ReadBudget&);
    std::uintptr_t elfAddress() const;
    std::uintptr_t loadBias() const;
    ReadResult read(std::uintptr_t, std::span<std::byte>) override;
    std::uint64_t generation() const override;
    std::string identity() const;
    static std::string providerIdentity();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

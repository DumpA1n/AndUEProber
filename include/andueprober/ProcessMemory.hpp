#pragma once
#include "Core.hpp"
#include <sys/types.h>

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
    // Opens an existing read-only procfs channel with explicit force-read access
    // to mapped pages, including ranges absent from /proc/<pid>/maps; the kernel
    // decides whether such pages exist. Targets can hide live mappings, such as
    // FName pool blocks, from that listing. This never attaches to or changes the
    // target. The caller must revalidate the lease between bounded operations
    // because an external loader lease is impossible.
    Status openRemoteByName(pid_t, std::span<const std::string> moduleNames, ReadBudget&);
    Status validateLease() const;
    std::uintptr_t elfAddress() const;
    std::uintptr_t loadBias() const;
    std::string modulePath() const;
    pid_t targetPid() const;
    bool isRemote() const;
    ReadResult read(std::uintptr_t, std::span<std::byte>) override;
    std::uint64_t generation() const override;
    std::string identity() const;
    static std::string providerIdentity();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

#include "andueprober/ProcessMemory.hpp"
#include "andueprober/Discovery.hpp"
#include <array>
#if defined(__ANDROID__)
#include <android/dlext.h>
#endif
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto path = std::filesystem::canonical(argv[1]).string();
    std::uint64_t prior = 0;
    for (int cycle = 0; cycle < 8; ++cycle) {
        void* original = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(original);
        auto address = reinterpret_cast<std::uintptr_t>(dlsym(original, "andueprober_owned_module_value")); REQUIRE(address);
        {
            andueprober::ProcessMemory memory;
            REQUIRE(memory.open("", address).code == andueprober::Error::InvalidArgument);
            REQUIRE(memory.open(path, 1).code == andueprober::Error::StaleIdentity);
            REQUIRE(memory.open(path, reinterpret_cast<std::uintptr_t>(&std::puts)).code == andueprober::Error::StaleIdentity);
            andueprober::ReadBudget discoveryBudget;
            discoveryBudget.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            const std::array<std::string, 1> names{std::filesystem::path(path).filename().string()};
            const std::array<std::string, 1> missing{"libandueprober_deliberately_absent.so"};
            REQUIRE(memory.openByName(missing, discoveryBudget).code == andueprober::Error::Unmapped);
            auto limited = discoveryBudget; limited.remainingBytes = 0;
            REQUIRE(memory.openByName(names, limited).code == andueprober::Error::BudgetExceeded);
            std::atomic<bool> cancelled{true}; limited = discoveryBudget; limited.cancelled = &cancelled;
            REQUIRE(memory.openByName(names, limited).code == andueprober::Error::Cancelled);
            limited = discoveryBudget; limited.deadline = std::chrono::steady_clock::now();
            REQUIRE(memory.openByName(names, limited).code == andueprober::Error::DeadlineExceeded);
            auto status = memory.openByName(names, discoveryBudget);
            if (!status) std::fprintf(stderr, "%s\n", status.message.c_str());
            REQUIRE(status); REQUIRE(memory.generation() > prior);
            REQUIRE(memory.identity().find("#gnu-build-id:") != std::string::npos);
            REQUIRE(memory.open(path, address).code == andueprober::Error::Busy);
            REQUIRE(dlclose(original) == 0);
            andueprober::ModuleImage image;
            REQUIRE(andueprober::readModuleImage(memory, memory.elfAddress(), memory.loadBias(), memory.identity(), discoveryBudget, image));
            REQUIRE(image.identity == memory.identity() && image.generation == memory.generation() && !image.ranges.empty());
            std::uint32_t value = 0;
            andueprober::ReadBudget budget; budget.generation = memory.generation();
            REQUIRE(andueprober::readExact(memory, address, std::as_writable_bytes(std::span(&value, 1)), budget));
            REQUIRE(value == 0x1234abcd);
            budget.generation = prior;
            REQUIRE(andueprober::readExact(memory, address, std::as_writable_bytes(std::span(&value, 1)), budget).code == andueprober::Error::StaleIdentity);
            prior = memory.generation();
        }
        void* retired = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
        REQUIRE(retired == nullptr);
    }
#if defined(__ANDROID__)
    {
        void* first = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(first);
        android_dlextinfo extension{}; extension.flags = ANDROID_DLEXT_FORCE_LOAD;
        void* second = android_dlopen_ext(path.c_str(), RTLD_NOW | RTLD_LOCAL, &extension); REQUIRE(second);
        auto firstAddress = reinterpret_cast<std::uintptr_t>(dlsym(first, "andueprober_owned_module_value"));
        auto secondAddress = reinterpret_cast<std::uintptr_t>(dlsym(second, "andueprober_owned_module_value"));
        REQUIRE(firstAddress && secondAddress && firstAddress != secondAddress);
        andueprober::ProcessMemory ambiguous;
        REQUIRE(ambiguous.open(path, firstAddress).code == andueprober::Error::InvalidEvidence);
        REQUIRE(ambiguous.open(path, secondAddress).code == andueprober::Error::InvalidEvidence);
        andueprober::ReadBudget budget;
        const std::array<std::string, 1> names{std::filesystem::path(path).filename().string()};
        REQUIRE(ambiguous.openByName(names, budget).code == andueprober::Error::InvalidEvidence);
        REQUIRE(dlclose(second) == 0);
        REQUIRE(ambiguous.open(path, firstAddress));
        REQUIRE(dlclose(first) == 0);
    }
    REQUIRE(dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD) == nullptr);
#endif
    std::puts("PASS: exact linker selection, bounded ELF and identity reads, cancellation, budgets, eight lease/reload generations and duplicate FORCE_LOAD rejection; no UE engine executed");
}

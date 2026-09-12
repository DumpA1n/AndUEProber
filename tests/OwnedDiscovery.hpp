#pragma once
#include "andueprober/Discovery.hpp"
#include <array>
#include <cstring>
#include <functional>

struct OwnedDiscovery final : andueprober::MemoryReader {
    static constexpr std::uintptr_t base = 0x100000;
    std::array<std::byte, 0x5000> bytes{};
    std::uint64_t epoch = 7;
    std::size_t reads = 0;
    andueprober::Error failure = andueprober::Error::None;
    std::function<void(std::uintptr_t)> onRead;
    OwnedDiscovery() {
        put(0, 0x464c457f, 4); put(4, 0x010102, 3);
        put(16, 3, 2); put(18, 183, 2); put(20, 1, 4);
        put(32, 64, 8); put(52, 64, 2); put(54, 56, 2); put(56, 2, 2);
        segment(64, 0, 0x2000, 4); segment(120, 0x3000, 0x2000, 5);
        text(0xff0, u"Game engine shut down"); text(0x1100, u"%s__Direction_%s");
        put(0x3000, 0xa9bf7bfd, 4);
        reference(0x3004, 0xff0, 8);
        call(0x300c, 0x3200); put(0x3010, 0xd65f03c0, 4);
        put(0x3100, 0xd10083ff, 4); reference(0x3104, 0x1100, 9);
        call(0x310c, 0x3300); put(0x3110, 0xd65f03c0, 4);
        adrp(0x3200, 0x1800, 10); put(0x3204, 0xf9400000u | (0x800u / 8u << 10) | (10u << 5) | 11, 4);
        put(0x3208, 0xd65f03c0, 4); put(0x3300, 0xd65f03c0, 4);
        put(0x1800, base + 0x1900, 8);
    }
    void put(std::size_t offset, std::uint64_t value, std::size_t size) {
        for (std::size_t i = 0; i < size; ++i) bytes.at(offset + i) = std::byte((value >> (i * 8)) & 255);
    }
    template<std::size_t N> void text(std::size_t offset, const char16_t (&value)[N]) {
        for (std::size_t i = 0; i < N; ++i) put(offset + i * 2, value[i], 2);
    }
    void segment(std::size_t offset, std::size_t address, std::size_t size, unsigned flags) {
        put(offset, 1, 4); put(offset + 4, flags, 4); put(offset + 8, address, 8);
        put(offset + 16, address, 8); put(offset + 32, size, 8); put(offset + 40, size, 8); put(offset + 48, 4096, 8);
    }
    void adrp(std::size_t pc, std::size_t target, unsigned reg) {
        const auto pages = (static_cast<std::int64_t>((base + target) & ~std::uintptr_t{4095}) -
            static_cast<std::int64_t>((base + pc) & ~std::uintptr_t{4095})) / 4096;
        const auto immediate = static_cast<std::uint32_t>(pages) & 0x1fffffu;
        put(pc, 0x90000000u | ((immediate & 3u) << 29) | ((immediate >> 2) << 5) | reg, 4);
    }
    void reference(std::size_t pc, std::size_t target, unsigned reg) {
        adrp(pc, target, reg);
        put(pc + 4, 0x91000000u | ((target & 4095u) << 10) | (reg << 5) | reg, 4);
    }
    void call(std::size_t pc, std::size_t target) {
        const auto words = (static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc)) / 4;
        put(pc, 0x94000000u | (static_cast<std::uint32_t>(words) & 0x03ffffffu), 4);
    }
    andueprober::ReadResult read(std::uintptr_t address, std::span<std::byte> destination) override {
        ++reads;
        if (onRead) onRead(address);
        if (failure != andueprober::Error::None) return {0, failure, 13};
        if (address < base || address - base > bytes.size() || destination.size() > bytes.size() - (address - base))
            return {0, andueprober::Error::Unmapped};
        std::memcpy(destination.data(), bytes.data() + address - base, destination.size());
        return {destination.size()};
    }
    std::uint64_t generation() const override { return epoch; }
    andueprober::ReadBudget budget() const {
        andueprober::ReadBudget result;
        result.remainingBytes = 1024 * 1024; result.generation = epoch; return result;
    }
    andueprober::Status image(andueprober::ReadBudget& budget, andueprober::ModuleImage& image) {
        return andueprober::readModuleImage(*this, base, base, "owned-elf64-aarch64-v1", budget, image);
    }
};

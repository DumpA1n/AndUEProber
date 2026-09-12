#pragma once
#include "andueprober/Probe.hpp"
#include <array>
#include <cstring>
#include <functional>

// A bounded memory image with independently defined flat/chunked fixture layouts.
struct OwnedObjectArray final : andueprober::MemoryReader {
    static constexpr std::uintptr_t Array = 32;
    std::array<std::byte, 2048> bytes;
    andueprober::ObjectArrayProfile profile;
    std::uint64_t epoch = 1;
    std::size_t reads = 0;
    std::function<void(OwnedObjectArray&, std::uintptr_t)> beforeRead;
    explicit OwnedObjectArray(bool chunked) {
        bytes.fill(std::byte{0x5a});
        profile.identity = chunked ? "owned-array-chunked-v1" : "owned-array-flat-v1";
        profile.objects = 0; profile.count = 8; profile.capacity = 12;
        profile.itemStride = chunked ? 24 : 16; profile.itemObject = chunked ? 8 : 0;
        profile.chunkCount = 16; profile.chunkCapacity = 20;
        profile.elementsPerChunk = chunked ? 2 : 0;
        profile.maximumObjects = 16; profile.maximumExamined = 4; profile.sampleLimit = 4;
        put<std::uintptr_t>(Array, 256); put<std::int32_t>(Array + 8, 4);
        put<std::int32_t>(Array + 12, 4); put<std::int32_t>(Array + 16, 2); put<std::int32_t>(Array + 20, 2);
        if (chunked) { put<std::uintptr_t>(256, 384); put<std::uintptr_t>(264, 512); }
        for (std::uint32_t i = 0; i < 4; ++i) {
            const auto storage = chunked ? (i < 2 ? 384u : 512u) : 256u;
            const auto item = chunked ? i % 2 : i;
            put<std::uintptr_t>(storage + item * *profile.itemStride + *profile.itemObject, 1024 + i * 64);
            put<std::uint32_t>(1024 + i * 64, i);
        }
    }
    template<class T> void put(std::uintptr_t address, T value) {
        if (address + sizeof(value) > bytes.size()) std::abort();
        std::memcpy(bytes.data() + address, &value, sizeof(value));
    }
    andueprober::ReadResult read(std::uintptr_t address, std::span<std::byte> destination) override {
        ++reads;
        if (beforeRead) beforeRead(*this, address);
        if (address > bytes.size() || destination.size() > bytes.size() - address)
            return {0, andueprober::Error::Unmapped};
        std::memcpy(destination.data(), bytes.data() + address, destination.size());
        return {destination.size(), andueprober::Error::None};
    }
    std::uint64_t generation() const override { return epoch; }
};

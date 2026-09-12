#pragma once
#include <cstddef>
#include <cstdint>

namespace owned_layout {
struct Item { std::uint16_t value; std::int8_t sign; };
enum class Mode : std::uint8_t { First = 1, Last = 255 };
struct alignas(16) Packet {
    std::uint8_t tag;
    Item items[2];
    double weight;
    Mode mode;
    std::uint64_t address;
};
}

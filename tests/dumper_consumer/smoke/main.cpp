#include <Reflection.hpp>
#include "../OwnedLayout.hpp"
#include <limits>
using andueprober_sdk::Item;
using andueprober_sdk::Packet;
using andueprober_sdk::SignedByte;
using andueprober_sdk::SignedLong;
using andueprober_sdk::UnsignedLong;
static_assert(sizeof(andueprober_sdk::std) == 2);
static_assert(sizeof(andueprober_sdk::Shadow) == 24);
static_assert(offsetof(andueprober_sdk::Shadow, Item) == 0);
static_assert(offsetof(andueprober_sdk::Shadow, std) == 16);
static_assert(sizeof(andueprober_sdk::auep_padding_0) == 4);
static_assert(sizeof(Item) == sizeof(owned_layout::Item));
static_assert(alignof(Item) == alignof(owned_layout::Item));
static_assert(sizeof(Packet) == sizeof(owned_layout::Packet));
static_assert(alignof(Packet) == alignof(owned_layout::Packet));
static_assert(offsetof(Packet, items) == offsetof(owned_layout::Packet, items));
static_assert(offsetof(Packet, weight) == offsetof(owned_layout::Packet, weight));
static_assert(offsetof(Packet, mode) == offsetof(owned_layout::Packet, mode));
static_assert(offsetof(Packet, address) == offsetof(owned_layout::Packet, address));
static_assert(static_cast<std::int8_t>(SignedByte::Minimum) == -128);
static_assert(static_cast<std::int8_t>(SignedByte::NegativeOne) == -1);
static_assert(static_cast<std::int8_t>(SignedByte::Maximum) == 127);
static_assert(static_cast<std::int64_t>(SignedLong::Minimum) == std::numeric_limits<std::int64_t>::min());
static_assert(static_cast<std::int64_t>(SignedLong::NegativeOne) == -1);
static_assert(static_cast<std::int64_t>(SignedLong::Maximum) == std::numeric_limits<std::int64_t>::max());
static_assert(static_cast<std::uint64_t>(UnsignedLong::Maximum) == std::numeric_limits<std::uint64_t>::max());
int main() {
    Packet value{};
    value.items[1].sign = -1;
    value.address = UINT64_MAX;
    return value.items[1].sign == -1 && value.address == UINT64_MAX ? 0 : 1;
}

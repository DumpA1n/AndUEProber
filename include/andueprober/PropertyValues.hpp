#pragma once
#include "Properties.hpp"

namespace andueprober {
enum class BoolEncoding { Unknown, NativeByte, SingleBit };
struct BoolPropertyProfile {
    std::string identity;
    PropertyProbeProfile property;
    std::uint32_t extent = 0, propertyBaseExtent = 0;
};
struct BoolPropertySample {
    std::uintptr_t object = 0;
    std::string identity;
    BoolEncoding encoding = BoolEncoding::Unknown;
    std::uint8_t fieldSize = 0, byteOffset = 0, byteMask = 0, fieldMask = 0;
    std::uint32_t storageExtent = 0;
    std::string storageIdentity;
};
enum class FieldPathRepresentation { Unknown, InlineFName };
struct FieldPathPropertyProfile {
    std::string identity;
    PropertyProbeProfile property;
    std::uint32_t extent = 0, propertyBaseExtent = 0;
    FieldPathRepresentation representation = FieldPathRepresentation::Unknown;
};
struct FieldPathPropertySample {
    std::uintptr_t object = 0;
    std::string identity, expectedName;
};

// Independent 3-16 named metadata records declare the complete nine-field property
// prefix. Its Name, Owner and scalar evidence must match the exact physical pool,
// generation and preceding profiles. All candidates follow that occupied prefix;
// no prefix size or field order is inferred. Shared budgets bound reads, evidence,
// deadlines and cancellation. Caller-owned metadata remains immutable and target
// synchronization belongs to the caller. Final readback and transitive dependency
// checks precede atomic publication; validated User overrides retain ownership.
//
// Bool observes four separate uint8 fields: FieldSize, ByteOffset, ByteMask and
// FieldMask. NativeByte requires exactly 1/0/1/255. SingleBit requires a nonzero
// one-bit ByteMask equal to FieldMask and ByteOffset < FieldSize <= storageExtent
// (1-255). Storage is an independent declaration and is never read. Each column
// needs a unique offset; pure NativeByte or pure SingleBit sets cannot distinguish
// equivalent columns and are rejected. Encoding Unknown is Unsupported.
Status probeBoolProperty(MemoryReader&, const BoolPropertyProfile&, std::span<const BoolPropertySample>,
    const NameLayout&, std::uintptr_t pool, const NamePoolProfile&, ReadBudget&, Snapshot&);

// FieldPath observes only an explicitly declared inline FName PropertyClass.
// Exact full names, raw FName bytes and pool reads are checked again before
// publication. Other representations return Unsupported. This contract does not
// identify arbitrary UE FieldPath wrappers or dereference a property class.
Status probeFieldPathProperty(MemoryReader&, const FieldPathPropertyProfile&, std::span<const FieldPathPropertySample>,
    const NameLayout&, std::uintptr_t pool, const NamePoolProfile&, ReadBudget&, Snapshot&);
}

#pragma once
#include "Core.hpp"

namespace andueprober {
enum class ReflectionScalar { UInt8, Int8, UInt16, Int16, UInt32, Int32, UInt64, Int64, Float32, Float64, Address64 };
enum class ReflectionTypeKind { Scalar, Record, Enumeration };
struct ReflectionType {
    ReflectionTypeKind kind = ReflectionTypeKind::Scalar;
    ReflectionScalar scalar = ReflectionScalar::UInt8;
    std::uint32_t reference = 0;
    std::uint32_t count = 1;
};
struct ReflectionFieldSpec {
    std::string name;
    ReflectionType type;
    std::string offsetSource;
};
struct ReflectionRecordSpec {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t size = 0, alignment = 0;
    std::string metadataSource;
    std::vector<ReflectionFieldSpec> fields;
};
struct ReflectionEnumValue { std::string name; std::uint64_t bits = 0; };
struct ReflectionEnumSpec {
    std::uint32_t id = 0;
    std::string name;
    ReflectionScalar underlying = ReflectionScalar::UInt8;
    std::string metadataSource;
    std::vector<ReflectionEnumValue> values;
};
struct ReflectionSchema {
    std::string identity;
    std::vector<ReflectionRecordSpec> records;
    std::vector<ReflectionEnumSpec> enumerations;
};
struct ReflectionLimits {
    std::size_t maximumTypes = 1024;
    std::size_t maximumFields = 16384;
    std::size_t maximumEnumValues = 16384;
    std::size_t maximumMetadataBytes = 4 * 1024 * 1024;
    std::uint32_t maximumTypeBytes = 16 * 1024 * 1024;
    std::uint32_t maximumArrayElements = 65536;
    const std::atomic<bool>* cancelled = nullptr;
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
};
struct ReflectionField {
    std::string name;
    ReflectionType type;
    std::uint32_t offset = 0;
    std::string offsetSource;
    std::uint64_t offsetVersion = 0;
};
struct ReflectionRecord {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t size = 0, alignment = 0;
    std::string metadataSource;
    std::vector<ReflectionField> fields;
};
struct FreezeReflectionResult;
class FrozenReflection {
public:
    const Snapshot& analysis() const { return analysis_; }
    const std::string& schemaIdentity() const { return identity_; }
    std::span<const ReflectionRecord> records() const { return records_; }
    std::span<const ReflectionEnumSpec> enumerations() const { return enumerations_; }
private:
    FrozenReflection() = default;
    Snapshot analysis_;
    std::string identity_;
    std::vector<ReflectionRecord> records_;
    std::vector<ReflectionEnumSpec> enumerations_;
    friend FreezeReflectionResult freezeReflection(const Snapshot&, const ReflectionSchema&, const ReflectionLimits&);
};
struct FreezeReflectionResult {
    Status status;
    std::shared_ptr<const FrozenReflection> snapshot;
};
// Builds an immutable data-layout subset. Field offsets and versions are resolved
// from validated analysis offsets; zero is valid. Field types, sizes, alignment, enum values
// and their source labels are independent caller declarations, never inferred
// from a probe extent. Validation checks their internal layout consistency; it
// does not establish the correctness of an external engine's declared type size.
//
// Supported representation is little-endian, 64-bit addresses and naturally
// aligned scalar, enum, fixed-array and by-value record fields. Recursive value
// types, overlapping fields, bitfields, inheritance and callable engine methods
// are not represented. Enum bits contain exactly the underlying width; signed
// values use two's-complement bits. Input storage stays immutable during this
// call. The frozen analysis retains offset evidence, excluding UI diagnostics.
FreezeReflectionResult freezeReflection(const Snapshot&, const ReflectionSchema&, const ReflectionLimits& = {});
}

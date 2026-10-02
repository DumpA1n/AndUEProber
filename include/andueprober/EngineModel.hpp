#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace andueprober {
// Each function reproduces the named Unreal Engine 4.25-5.x function from values
// already read by the caller. None reads memory or calls engine code.

// FName::ToString: the entry text, then "_<Number - 1>" unless Number is
// NAME_NO_NUMBER_INTERNAL (0). Two FNames are equal only when both the entry and
// Number agree, so dropping Number merges distinct objects.
std::string fnameToString(std::string_view entry, std::uint32_t number);

// EClassCastFlags bits of the CoreUObject intrinsic classes. UClass binding ORs the
// super class's flags into its own, so each value includes its ancestors' bits.
struct IntrinsicCastClass {
    std::string_view name;
    std::uint64_t classCastFlags;
};
std::span<const IntrinsicCastClass> intrinsicCastClasses();

// UFunction::InitializeDerivedMembers walks ChildProperties in order. Each CPF_Parm
// property increments NumParms and sets ParmsSize to its offset plus
// ArrayDim * ElementSize; CPF_ReturnParm sets ReturnValueOffset. The walk ends at the
// first non-parameter, unless FUNC_HasDefaults lets CPF_ZeroConstructor locals pass.
struct FunctionParameter {
    std::uint64_t propertyFlags = 0;
    std::int32_t offset = 0, arrayDim = 0, elementSize = 0;
};
struct FunctionParameterSummary {
    std::uint8_t numParms = 0;
    std::uint16_t parmsSize = 0, returnValueOffset = 0xffff;
    bool operator==(const FunctionParameterSummary&) const = default;
};
// Returns nothing when a property's storage is negative or exceeds the uint8/uint16
// fields that the engine would silently truncate.
std::optional<FunctionParameterSummary> deriveFunctionParameters(std::uint32_t functionFlags,
    std::span<const FunctionParameter> properties);

// FScriptSparseArray, FScriptSet and FScriptMap::GetScriptLayout, built with
// FStructBuilder: each member starts at Align(end, alignment) and the record size
// is Align(end, largest alignment). Sizes and alignments are the property's
// GetSize() and GetMinAlignment(); alignments must be powers of two.
struct ScriptSparseArrayLayout {
    std::int32_t alignment = 0, size = 0;
    bool operator==(const ScriptSparseArrayLayout&) const = default;
};
struct ScriptSetLayout {
    std::int32_t hashNextIdOffset = 0, hashIndexOffset = 0, size = 0;
    ScriptSparseArrayLayout sparseArray;
    bool operator==(const ScriptSetLayout&) const = default;
};
struct ScriptMapLayout {
    std::int32_t valueOffset = 0;
    ScriptSetLayout set;
    bool operator==(const ScriptMapLayout&) const = default;
};
std::optional<ScriptSetLayout> scriptSetLayout(std::int32_t elementSize, std::int32_t elementAlignment);
std::optional<ScriptMapLayout> scriptMapLayout(std::int32_t keySize, std::int32_t keyAlignment,
    std::int32_t valueSize, std::int32_t valueAlignment);

// FBoolProperty::SetBoolSize postconditions. ElementSize equals FieldSize. A native
// bool has ByteOffset 0, ByteMask 1 and FieldMask 0xff. A bitfield bool selects one
// bit of the byte at ByteOffset < FieldSize, with FieldMask equal to ByteMask, so its
// value is (byte & FieldMask) != 0 rather than the whole byte.
enum class BoolPropertyKind { Invalid, Native, Bitfield };
BoolPropertyKind classifyBoolProperty(std::int32_t elementSize, std::uint8_t fieldSize,
    std::uint8_t byteOffset, std::uint8_t byteMask, std::uint8_t fieldMask);

// FStructBaseChain::ReinitializeBaseChainArray: one FStructBaseChain* per struct in
// the SuperStruct walk, root first and the struct itself last. ancestry lists the
// walk from the struct to the root; chainOffset is the FStructBaseChain subobject
// offset inside UStruct. NumStructBasesInChainMinusOne is the result size minus one.
std::vector<std::uintptr_t> structBaseChainArray(std::span<const std::uintptr_t> ancestry,
    std::uintptr_t chainOffset);
}

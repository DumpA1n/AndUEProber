#include "andueprober/EngineModel.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace andueprober {
namespace {
constexpr std::uint64_t parm = 0x80, zeroConstructor = 0x200, returnParm = 0x400;
constexpr std::uint32_t hasDefaults = 0x80;
constexpr std::int64_t maximumInt32 = std::numeric_limits<std::int32_t>::max();
bool alignmentValid(std::int64_t alignment) {
    return alignment > 0 && alignment <= 4096 && std::has_single_bit(static_cast<std::uint64_t>(alignment));
}
std::int64_t align(std::int64_t value, std::int64_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }
struct StructBuilder {
    std::int64_t end = 0, alignment = 0;
    std::int64_t add(std::int64_t size, std::int64_t memberAlignment) {
        const auto offset = align(end, memberAlignment);
        end = offset + size;
        alignment = std::max(alignment, memberAlignment);
        return offset;
    }
    std::int64_t size() const { return align(end, alignment); }
};
std::optional<ScriptSetLayout> setLayout(std::int64_t elementSize, std::int64_t elementAlignment) {
    if (elementSize < 0 || elementSize > maximumInt32 || !alignmentValid(elementAlignment)) return {};
    StructBuilder element;
    element.add(elementSize, elementAlignment);
    const auto hashNextId = element.add(sizeof(std::int32_t), alignof(std::int32_t));
    const auto hashIndex = element.add(sizeof(std::int32_t), alignof(std::int32_t));
    const auto size = element.size();
    constexpr std::int64_t freeListLinkSize = 2 * sizeof(std::int32_t), freeListLinkAlignment = alignof(std::int32_t);
    const auto sparseSize = std::max(size, freeListLinkSize);
    if (sparseSize > maximumInt32) return {};
    return ScriptSetLayout{static_cast<std::int32_t>(hashNextId), static_cast<std::int32_t>(hashIndex),
        static_cast<std::int32_t>(size), {static_cast<std::int32_t>(std::max(element.alignment, freeListLinkAlignment)),
        static_cast<std::int32_t>(sparseSize)}};
}
}

std::string fnameToString(std::string_view entry, std::uint32_t number) {
    std::string result(entry);
    if (number) result += "_" + std::to_string(number - 1);
    return result;
}

std::span<const IntrinsicCastClass> intrinsicCastClasses() {
    constexpr std::uint64_t field = 0x1, enumeration = 0x4, structure = 0x8, scriptStruct = 0x10,
        uclass = 0x20, function = 0x80000;
    static constexpr std::array<IntrinsicCastClass, 7> classes{{
        {"Object", 0},
        {"Field", field},
        {"Enum", field | enumeration},
        {"Struct", field | structure},
        {"ScriptStruct", field | structure | scriptStruct},
        {"Class", field | structure | uclass},
        {"Function", field | structure | function}}};
    return classes;
}

std::optional<FunctionParameterSummary> deriveFunctionParameters(std::uint32_t functionFlags,
    std::span<const FunctionParameter> properties) {
    FunctionParameterSummary summary;
    std::int64_t numParms = 0;
    for (const auto& property : properties) {
        if (property.propertyFlags & parm) {
            const auto size = std::int64_t{property.arrayDim} * property.elementSize;
            const auto end = std::int64_t{property.offset} + size;
            if (property.offset < 0 || property.arrayDim < 0 || property.elementSize < 0 ||
                ++numParms > std::numeric_limits<std::uint8_t>::max() || end > std::numeric_limits<std::uint16_t>::max())
                return {};
            summary.numParms = static_cast<std::uint8_t>(numParms);
            summary.parmsSize = static_cast<std::uint16_t>(end);
            if (property.propertyFlags & returnParm) {
                if (property.offset >= std::numeric_limits<std::uint16_t>::max()) return {};
                summary.returnValueOffset = static_cast<std::uint16_t>(property.offset);
            }
        } else if (!(functionFlags & hasDefaults) || !(property.propertyFlags & zeroConstructor)) {
            break;
        }
    }
    return summary;
}

std::optional<ScriptSetLayout> scriptSetLayout(std::int32_t elementSize, std::int32_t elementAlignment) {
    return setLayout(elementSize, elementAlignment);
}

std::optional<ScriptMapLayout> scriptMapLayout(std::int32_t keySize, std::int32_t keyAlignment,
    std::int32_t valueSize, std::int32_t valueAlignment) {
    if (keySize < 0 || valueSize < 0 || !alignmentValid(keyAlignment) || !alignmentValid(valueAlignment)) return {};
    StructBuilder pair;
    pair.add(keySize, keyAlignment);
    const auto valueOffset = pair.add(valueSize, valueAlignment);
    if (valueOffset > maximumInt32) return {};
    const auto set = setLayout(pair.size(), pair.alignment);
    if (!set) return {};
    return ScriptMapLayout{static_cast<std::int32_t>(valueOffset), *set};
}

BoolPropertyKind classifyBoolProperty(std::int32_t elementSize, std::uint8_t fieldSize,
    std::uint8_t byteOffset, std::uint8_t byteMask, std::uint8_t fieldMask) {
    if (elementSize != fieldSize || (fieldSize != 1 && fieldSize != 2 && fieldSize != 4 && fieldSize != 8) ||
        byteOffset >= fieldSize)
        return BoolPropertyKind::Invalid;
    if (byteOffset == 0 && byteMask == 1 && fieldMask == 0xff) return BoolPropertyKind::Native;
    if (std::has_single_bit(byteMask) && fieldMask == byteMask) return BoolPropertyKind::Bitfield;
    return BoolPropertyKind::Invalid;
}

std::vector<std::uintptr_t> structBaseChainArray(std::span<const std::uintptr_t> ancestry,
    std::uintptr_t chainOffset) {
    std::vector<std::uintptr_t> bases;
    bases.reserve(ancestry.size());
    for (auto ancestor = ancestry.rbegin(); ancestor != ancestry.rend(); ++ancestor)
        bases.push_back(*ancestor + chainOffset);
    return bases;
}
}

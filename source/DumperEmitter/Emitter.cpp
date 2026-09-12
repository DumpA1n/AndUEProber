#include "Emitter.hpp"
#include "BoundedBuffer.hpp"

#include <limits>
#include <new>

namespace andueprober::dumper_emitter {
namespace {

struct EnumKind { unsigned width; bool isSigned; };
EnumKind enumKind(std::string_view underlying) {
    if (underlying.starts_with("::")) underlying.remove_prefix(2);
    if (underlying.starts_with("std::")) underlying.remove_prefix(5);
    if (underlying == "uint8_t") return {8, false};
    if (underlying == "int8_t") return {8, true};
    if (underlying == "uint16_t") return {16, false};
    if (underlying == "int16_t") return {16, true};
    if (underlying == "uint32_t") return {32, false};
    if (underlying == "int32_t") return {32, true};
    if (underlying == "uint64_t") return {64, false};
    if (underlying == "int64_t") return {64, true};
    throw Stop{Error::Unsupported};
}

struct InputBudget {
    const Limits& limits;
    std::size_t bytes{};
    std::size_t items{};
    void item(std::size_t count = 1) {
        checkpoint(limits);
        if (count > limits.maxItems - items) throw Stop{Error::InputLimit};
        items += count;
    }
    void text(const std::string& value) {
        checkpoint(limits);
        if (value.size() > limits.maxInputBytes - bytes) throw Stop{Error::InputLimit};
        bytes += value.size();
    }
};

void validate(std::vector<ExtractedPackage::Struct>& records,
              std::vector<ExtractedPackage::Enum>& enums, const Limits& limits) {
    if (limits.maxOutputBytes == 0 || limits.maxOutputBytes > 64 * 1024 * 1024 ||
        limits.maxItems == 0 || limits.maxInputBytes == 0)
        throw Stop{Error::InvalidInput};
    InputBudget budget{limits};
    budget.item(records.size());
    budget.item(enums.size());
    for (const auto& record : records) {
        for (const auto* text : {&record.Name, &record.FullName, &record.CppName,
                &record.CppNameOnly, &record.SuperCppName, &record.ExtraDecls,
                &record.PrefixDecls, &record.Trailer}) budget.text(*text);
        budget.item(record.Members.size());
        budget.item(record.Functions.size());
        budget.item(record.FullDeps.size());
        budget.item(record.ForwardDeps.size());
        for (const auto& value : record.FullDeps) budget.text(value);
        for (const auto& value : record.ForwardDeps) budget.text(value);
        for (const auto& member : record.Members) {
            budget.text(member.Type);
            budget.text(member.Name);
            budget.text(member.extra);
        }
        for (const auto& function : record.Functions) {
            if (function.Func != 0) throw Stop{Error::Unsupported};
            for (const auto* text : {&function.Name, &function.FullName, &function.CppName,
                    &function.Params, &function.ReturnType, &function.OwnerCppName,
                    &function.OwnerUEName, &function.Flags}) budget.text(*text);
            budget.item(function.ParamsList.size());
            for (const auto& param : function.ParamsList) {
                budget.text(param.Type);
                budget.text(param.Name);
            }
        }
    }
    for (const auto& value : enums) {
        if (value.Members.empty()) throw Stop{Error::InvalidInput};
        const auto kind = enumKind(value.UnderlyingType);
        for (const auto* text : {&value.FullName, &value.CppName,
                &value.CppNameOnly, &value.UnderlyingType}) budget.text(*text);
        budget.item(value.Members.size());
        for (const auto& member : value.Members) {
            budget.text(member.first);
            if (kind.width < 64 && member.second >= (std::uint64_t{1} << kind.width))
                throw Stop{Error::InvalidInput};
        }
    }
}

} // namespace

std::string formatEnumValue(std::string_view underlying, std::uint64_t bits) {
    const auto kind = enumKind(underlying);
    if (!kind.isSigned) return std::to_string(bits) + "ULL";
    const auto sign = std::uint64_t{1} << (kind.width - 1);
    if ((bits & sign) == 0) return std::to_string(bits) + "LL";
    if (kind.width == 64 && bits == sign) return "(-9223372036854775807LL - 1)";
    const auto mask = kind.width == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << kind.width) - 1;
    return "-" + std::to_string((~bits + 1) & mask) + "LL";
}

Result emit(std::vector<ExtractedPackage::Struct>& records,
            std::vector<ExtractedPackage::Enum>& enums, const Limits& limits) noexcept {
    try {
        validate(records, enums, limits);
        BufferFmt buffer(limits);
        ExtractedPackage::AppendEnumsToBuffer(enums, &buffer);
        ExtractedPackage::AppendStructsToBuffer(records, &buffer);
        checkpoint(limits);
        return {Error::None, buffer.read()};
    } catch (const Stop& stop) {
        return {stop.error, {}};
    } catch (const std::bad_alloc&) {
        return {Error::AllocationFailure, {}};
    } catch (...) {
        return {Error::FormatFailure, {}};
    }
}

} // namespace andueprober::dumper_emitter

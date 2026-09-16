#include "andueprober/Reflection.hpp"
#include "Core/Budget.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <set>
#include <string_view>

namespace andueprober {
namespace {
bool identifier(const std::string& value) {
    const auto letter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    if (value.empty() || value.size() > 127 || !letter(value.front()) || value.find("__") != std::string::npos) return false;
    if (!std::all_of(value.begin(), value.end(), [&](char c) { return letter(c) || (c >= '0' && c <= '9') || c == '_'; })) return false;
    constexpr std::string_view keywords[] = {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
        "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval", "constexpr",
        "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
        "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float", "for",
        "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
        "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
        "requires", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
        "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
        "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"};
    return std::find(std::begin(keywords), std::end(keywords), value) == std::end(keywords);
}
std::uint32_t scalarSize(ReflectionScalar scalar) {
    switch (scalar) {
    case ReflectionScalar::UInt8: case ReflectionScalar::Int8: return 1;
    case ReflectionScalar::UInt16: case ReflectionScalar::Int16: return 2;
    case ReflectionScalar::UInt32: case ReflectionScalar::Int32: case ReflectionScalar::Float32: return 4;
    case ReflectionScalar::UInt64: case ReflectionScalar::Int64: case ReflectionScalar::Float64:
    case ReflectionScalar::Address64: return 8;
    }
    fail(Error::Unsupported, "Unsupported reflection scalar representation");
}
bool integer(ReflectionScalar scalar) {
    return scalar >= ReflectionScalar::UInt8 && scalar <= ReflectionScalar::Int64;
}
void proof(const Snapshot& analysis, Budget& budget) {
    budget.text(analysis.sessionId); budget.text(analysis.moduleIdentity); budget.text(analysis.layoutIdentity);
    budget.charge(analysis.offsets.size(), sizeof(Offset));
    for (const auto& [name, offset] : analysis.offsets) {
        budget.text(name);
        budget.charge(offset.dependencies.size(), sizeof(std::pair<std::string, std::uint64_t>));
        for (const auto& [dependency, version] : offset.dependencies) { (void)version; budget.text(dependency); }
        budget.charge(offset.evidence.size(), sizeof(Evidence));
        for (const auto& evidence : offset.evidence) {
            budget.text(evidence.check); budget.text(evidence.source);
            budget.charge(evidence.relativeAddresses.size(), sizeof(std::uintptr_t));
            budget.charge(evidence.sampleIdentities.size(), sizeof(std::string));
            if (!evidence.passed || !evidence.samples || evidence.sampleIdentities.size() != evidence.samples)
                fail(Error::InvalidEvidence, "Reflection offset evidence requires matching named samples");
            for (const auto& identity : evidence.sampleIdentities) budget.text(identity);
        }
    }
    if (!analysis.result || !validateSnapshot(analysis))
        fail(Error::InvalidEvidence, "Reflection requires a successful validated analysis snapshot");
}
}

FreezeReflectionResult freezeReflection(const Snapshot& analysis, const ReflectionSchema& schema, const ReflectionLimits& limits) {
    try {
        if (std::endian::native != std::endian::little || sizeof(std::uintptr_t) != 8 || sizeof(float) != 4 ||
            sizeof(double) != 8 || !std::numeric_limits<float>::is_iec559 || !std::numeric_limits<double>::is_iec559)
            fail(Error::Unsupported, "Reflection layouts require little-endian 64-bit addresses and IEEE binary32/binary64");
        if (!limits.maximumTypes || !limits.maximumFields || !limits.maximumEnumValues || !limits.maximumMetadataBytes ||
            !limits.maximumTypeBytes || !limits.maximumArrayElements)
            fail(Error::InvalidArgument, "Reflection limits must be nonzero");
        Budget budget{"Reflection freeze", limits.cancelled, limits.deadline, limits.maximumMetadataBytes};
        budget.check(); budget.text(schema.identity);
        if (schema.records.empty() && schema.enumerations.empty()) fail(Error::InvalidArgument, "Reflection requires at least one type");
        if (schema.records.size() > limits.maximumTypes || schema.enumerations.size() > limits.maximumTypes - schema.records.size())
            fail(Error::BudgetExceeded, "Reflection type count exceeds its limit");
        budget.charge(schema.records.size(), sizeof(ReflectionRecordSpec));
        budget.charge(schema.enumerations.size(), sizeof(ReflectionEnumSpec));
        proof(analysis, budget);
        std::map<std::uint32_t, const ReflectionRecordSpec*> records;
        std::map<std::uint32_t, const ReflectionEnumSpec*> enumerations;
        std::set<std::string> names;
        const auto typeName = [&](std::uint32_t id, const std::string& name, const std::string& source) {
            budget.text(name); budget.text(source);
            if (!id || !identifier(name) || !names.insert(name).second || records.contains(id) || enumerations.contains(id))
                fail(Error::InvalidArgument, "Reflection types require unique nonzero IDs and C++ identifiers");
        };
        std::size_t fieldCount = 0, valueCount = 0;
        for (const auto& record : schema.records) {
            typeName(record.id, record.name, record.metadataSource);
            if (!record.size || !record.alignment || record.alignment > 4096 ||
                (record.alignment & (record.alignment - 1)) || record.size % record.alignment)
                fail(Error::InvalidArgument, "Reflection records require bounded sizes and power-of-two alignment");
            if (record.size > limits.maximumTypeBytes) fail(Error::BudgetExceeded, "Reflection record size exceeds its limit");
            if (record.fields.empty()) fail(Error::InvalidArgument, "Reflection records require at least one typed field");
            if (record.fields.size() > limits.maximumFields - fieldCount)
                fail(Error::BudgetExceeded, "Reflection field count exceeds its limit");
            fieldCount += record.fields.size(); budget.charge(record.fields.size(), sizeof(ReflectionFieldSpec));
            records.emplace(record.id, &record);
        }
        for (const auto& enumeration : schema.enumerations) {
            typeName(enumeration.id, enumeration.name, enumeration.metadataSource);
            if (!integer(enumeration.underlying)) fail(Error::Unsupported, "Reflection enums require an integer underlying representation");
            const auto width = scalarSize(enumeration.underlying);
            if (enumeration.values.empty()) fail(Error::InvalidArgument, "Reflection enums require at least one named value");
            if (enumeration.values.size() > limits.maximumEnumValues - valueCount)
                fail(Error::BudgetExceeded, "Reflection enum value count exceeds its limit");
            valueCount += enumeration.values.size(); budget.charge(enumeration.values.size(), sizeof(ReflectionEnumValue));
            std::set<std::string> members;
            for (const auto& value : enumeration.values) {
                budget.text(value.name);
                if (!identifier(value.name) || !members.insert(value.name).second)
                    fail(Error::InvalidArgument, "Reflection enum values require distinct C++ identifiers");
                if (width < 8 && value.bits >= (std::uint64_t{1} << (width * 8)))
                    fail(Error::InvalidEvidence, "Reflection enum bits exceed the underlying width");
            }
            enumerations.emplace(enumeration.id, &enumeration);
        }
        std::map<std::uint32_t, ReflectionRecord> resolved;
        std::map<std::uint32_t, std::set<std::uint32_t>> dependencies, dependents;
        for (const auto& [id, record] : records) {
            ReflectionRecord output{id, record->name, record->size, record->alignment, record->metadataSource, {}};
            std::set<std::string> fieldNames;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> occupied;
            for (const auto& field : record->fields) {
                budget.check(); budget.text(field.name); budget.text(field.offsetSource);
                if (!identifier(field.name) || !fieldNames.insert(field.name).second || field.name == record->name)
                    fail(Error::InvalidArgument, "Reflection fields require distinct C++ identifiers");
                if (!field.type.count) fail(Error::InvalidArgument, "Reflection fixed arrays require a nonzero count");
                if (field.type.count > limits.maximumArrayElements)
                    fail(Error::BudgetExceeded, "Reflection fixed-array count exceeds its limit");
                std::uint32_t width = 0, alignment = 0;
                switch (field.type.kind) {
                case ReflectionTypeKind::Scalar:
                    if (field.type.reference) fail(Error::InvalidArgument, "Scalar fields cannot reference a named type");
                    width = alignment = scalarSize(field.type.scalar); break;
                case ReflectionTypeKind::Record: {
                    const auto type = records.find(field.type.reference);
                    if (type == records.end()) fail(Error::InvalidArgument, "Reflection record reference does not exist");
                    width = type->second->size; alignment = type->second->alignment;
                    dependencies[id].insert(field.type.reference); dependents[field.type.reference].insert(id); break;
                }
                case ReflectionTypeKind::Enumeration: {
                    const auto type = enumerations.find(field.type.reference);
                    if (type == enumerations.end()) fail(Error::InvalidArgument, "Reflection enum reference does not exist");
                    width = alignment = scalarSize(type->second->underlying); break;
                }
                default: fail(Error::Unsupported, "Unsupported reflection type kind");
                }
                const auto offset = analysis.offsets.find(field.offsetSource);
                if (offset == analysis.offsets.end() || !offset->second.value || offset->second.validation != Validation::Validated)
                    fail(Error::InvalidEvidence, "Reflection field offset lacks current validated evidence");
                if (field.type.count > limits.maximumTypeBytes / width)
                    fail(Error::BudgetExceeded, "Reflection array byte count exceeds its limit");
                const auto bytes = width * field.type.count, begin = *offset->second.value;
                if (begin > record->size || bytes > record->size - begin || begin % alignment || record->alignment < alignment)
                    fail(Error::InvalidEvidence, "Reflection field does not fit its declared size and alignment");
                occupied.emplace_back(begin, begin + bytes);
                output.fields.push_back({field.name, field.type, begin, field.offsetSource, offset->second.version});
            }
            std::sort(occupied.begin(), occupied.end());
            for (std::size_t i = 1; i < occupied.size(); ++i)
                if (occupied[i].first < occupied[i - 1].second)
                    fail(Error::InvalidEvidence, "Reflection fields overlap");
            std::sort(output.fields.begin(), output.fields.end(), [](const auto& left, const auto& right) { return left.offset < right.offset; });
            resolved.emplace(id, std::move(output));
        }
        std::vector<std::uint32_t> ready;
        for (const auto& [id, record] : records) { (void)record; if (dependencies[id].empty()) ready.push_back(id); }
        std::vector<ReflectionRecord> ordered;
        for (std::size_t i = 0; i < ready.size(); ++i) {
            budget.check();
            const auto id = ready[i]; ordered.push_back(std::move(resolved.at(id)));
            for (const auto dependent : dependents[id]) {
                dependencies[dependent].erase(id);
                if (dependencies[dependent].empty()) ready.push_back(dependent);
            }
        }
        if (ordered.size() != records.size()) fail(Error::InvalidEvidence, "Recursive by-value reflection records are unsupported");
        auto result = std::shared_ptr<FrozenReflection>(new FrozenReflection);
        result->analysis_.schemaVersion = analysis.schemaVersion; result->analysis_.sessionId = analysis.sessionId;
        result->analysis_.moduleIdentity = analysis.moduleIdentity; result->analysis_.generation = analysis.generation;
        result->analysis_.layout = analysis.layout; result->analysis_.layoutIdentity = analysis.layoutIdentity;
        result->analysis_.state = analysis.state; result->analysis_.offsets = analysis.offsets;
        result->identity_ = schema.identity; result->records_ = std::move(ordered); result->enumerations_ = schema.enumerations;
        budget.check();
        return {{}, std::move(result)};
    } catch (const Interrupted& stop) {
        try { return {{stop.code, stop.message()}, {}}; } catch (...) { return {{stop.code, {}}, {}}; }
    } catch (...) { return {{Error::Internal, {}}, {}}; }
}
}

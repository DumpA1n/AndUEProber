#include "andueprober/DumperAdapter.hpp"
#include "Emitter.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string_view>

namespace andueprober {
#define ANDUEPROBER_STRINGIFY_INNER(value) #value
#define ANDUEPROBER_STRINGIFY(value) ANDUEPROBER_STRINGIFY_INNER(value)
std::string_view dumperDependencyIdentity() noexcept {
    return ANDUEPROBER_STRINGIFY(ANDUEPROBER_DUMPER_IDENTITY);
}
#undef ANDUEPROBER_STRINGIFY
#undef ANDUEPROBER_STRINGIFY_INNER
namespace {
struct Failure { Error code; const char* message; };
[[noreturn]] void fail(Error code, const char* message) { throw Failure{code, message}; }
bool standardMacro(std::string_view name) {
    constexpr std::string_view fixed[] = {"NULL", "offsetof", "SIZE_MAX", "PTRDIFF_MIN", "PTRDIFF_MAX",
        "PTRDIFF_WIDTH", "SIZE_WIDTH", "INTPTR_MIN", "INTPTR_MAX", "INTPTR_WIDTH", "UINTPTR_MAX", "UINTPTR_WIDTH",
        "INTMAX_MIN", "INTMAX_MAX", "INTMAX_C", "INTMAX_WIDTH", "UINTMAX_MAX", "UINTMAX_C", "UINTMAX_WIDTH",
        "SIG_ATOMIC_MIN", "SIG_ATOMIC_MAX", "SIG_ATOMIC_WIDTH", "WCHAR_MIN", "WCHAR_MAX", "WCHAR_WIDTH",
        "WINT_MIN", "WINT_MAX", "WINT_WIDTH", "CHAR_BIT", "MB_LEN_MAX", "linux", "unix"};
    for (const auto value : fixed) if (value == name) return true;
    constexpr std::string_view families[] = {"INT", "UINT", "INT_LEAST", "UINT_LEAST", "INT_FAST", "UINT_FAST"};
    constexpr std::string_view suffixes[] = {"8_MIN", "8_MAX", "8_C", "8_WIDTH", "16_MIN", "16_MAX", "16_C", "16_WIDTH",
        "32_MIN", "32_MAX", "32_C", "32_WIDTH", "64_MIN", "64_MAX", "64_C", "64_WIDTH"};
    for (const auto family : families) {
        if (name.starts_with(family)) {
            const auto suffix = name.substr(family.size());
            for (const auto value : suffixes) if (value == suffix) return true;
        }
    }
    return false;
}
void declarationName(std::string_view name, bool enumerator = false) {
    if (standardMacro(name) || (!enumerator && name == "EM_MAX"))
        fail(Error::Unsupported, "A generated identifier conflicts with a standard-header macro");
}
struct Budget {
    const DumperOptions& options;
    std::size_t bytes{};
    std::size_t items{};
    void check() const {
        if (options.cancelled && options.cancelled->load()) fail(Error::Cancelled, "Dumper formatting cancelled");
        if (std::chrono::steady_clock::now() >= options.deadline)
            fail(Error::DeadlineExceeded, "Dumper formatting deadline exceeded");
    }
    void item(std::size_t count) {
        check();
        if (count > options.maximumItems - items) fail(Error::BudgetExceeded, "Dumper input item limit exceeded");
        items += count;
    }
    void text(std::string_view text) {
        check();
        if (text.size() > options.maximumInputBytes - bytes) fail(Error::BudgetExceeded, "Dumper input byte limit exceeded");
        bytes += text.size();
    }
    void append(std::string& output, std::string_view text) {
        check();
        if (text.size() > options.maximumOutputBytes - output.size())
            fail(Error::BudgetExceeded, "Dumper output byte limit exceeded");
        output.append(text);
    }
};
struct Scalar { const char* name; std::uint32_t size; };
Scalar scalar(ReflectionScalar type) {
    switch (type) {
    case ReflectionScalar::UInt8: return {"::std::uint8_t", 1};
    case ReflectionScalar::Int8: return {"::std::int8_t", 1};
    case ReflectionScalar::UInt16: return {"::std::uint16_t", 2};
    case ReflectionScalar::Int16: return {"::std::int16_t", 2};
    case ReflectionScalar::UInt32: return {"::std::uint32_t", 4};
    case ReflectionScalar::Int32: return {"::std::int32_t", 4};
    case ReflectionScalar::UInt64: case ReflectionScalar::Address64: return {"::std::uint64_t", 8};
    case ReflectionScalar::Int64: return {"::std::int64_t", 8};
    case ReflectionScalar::Float32: return {"float", 4};
    case ReflectionScalar::Float64: return {"double", 8};
    }
    fail(Error::Unsupported, "Unsupported frozen scalar representation");
}
bool isCancelled(void* value) noexcept {
    return value && static_cast<const std::atomic<bool>*>(value)->load();
}
Error translate(dumper_emitter::Error code) {
    using E = dumper_emitter::Error;
    switch (code) {
    case E::None: return Error::None;
    case E::InvalidInput: return Error::InvalidArgument;
    case E::Unsupported: return Error::Unsupported;
    case E::InputLimit: case E::OutputLimit: return Error::BudgetExceeded;
    case E::Cancelled: return Error::Cancelled;
    case E::DeadlineExceeded: return Error::DeadlineExceeded;
    case E::AllocationFailure: case E::FormatFailure: return Error::Internal;
    }
    return Error::Internal;
}
} // namespace

DumperHeaderResult buildDumperHeader(const FrozenReflection& frozen, const DumperOptions& options) {
    try {
        if (!options.maximumInputBytes || !options.maximumItems || !options.maximumOutputBytes ||
            options.maximumOutputBytes > 64 * 1024 * 1024 ||
            options.maximumItems > std::numeric_limits<std::size_t>::max() / 3 ||
            options.maximumInputBytes > std::numeric_limits<std::size_t>::max() - options.maximumOutputBytes)
            fail(Error::InvalidArgument, "Dumper limits are invalid");
        Budget budget{options};
        budget.check();
        budget.item(frozen.records().size());
        budget.item(frozen.enumerations().size());
        budget.text(frozen.schemaIdentity());
        for (const auto& record : frozen.records()) {
            declarationName(record.name);
            budget.text(record.name); budget.text(record.metadataSource);
            budget.item(record.fields.size());
            for (const auto& field : record.fields) {
                declarationName(field.name);
                budget.text(field.name); budget.text(field.offsetSource);
            }
        }
        for (const auto& enumeration : frozen.enumerations()) {
            declarationName(enumeration.name);
            budget.text(enumeration.name); budget.text(enumeration.metadataSource);
            budget.item(enumeration.values.size());
            for (const auto& value : enumeration.values) {
                declarationName(value.name, true);
                budget.text(value.name);
            }
        }
        using Package = dumper_emitter::ExtractedPackage;
        std::vector<Package::Struct> records;
        std::vector<Package::Enum> enumerations;
        std::map<std::uint32_t, std::pair<std::string, std::uint32_t>> types;
        for (const auto& record : frozen.records()) types.emplace(record.id, std::pair{"::andueprober_sdk::" + record.name, record.size});
        for (const auto& enumeration : frozen.enumerations()) {
            budget.check();
            const auto underlying = scalar(enumeration.underlying);
            types.emplace(enumeration.id, std::pair{"::andueprober_sdk::" + enumeration.name, underlying.size});
            Package::Enum output;
            output.FullName = enumeration.name;
            output.CppNameOnly = enumeration.name;
            output.UnderlyingType = underlying.name;
            output.CppName = "enum class " + enumeration.name + " : " + underlying.name;
            std::set<std::string> emittedNames;
            for (const auto& value : enumeration.values) {
                budget.check();
                const auto emittedName = value.name == "EM_MAX" ? enumeration.name + "_EM_MAX" : value.name;
                if (!emittedNames.insert(emittedName).second)
                    fail(Error::Unsupported, "Enumerator names collide after upstream macro qualification");
                output.Members.emplace_back(value.name, value.bits);
            }
            enumerations.push_back(std::move(output));
        }
        for (const auto& record : frozen.records()) {
            budget.check();
            Package::Struct output;
            output.FullName = record.name;
            output.CppNameOnly = record.name;
            output.CppName = "struct alignas(" + std::to_string(record.alignment) + ") " + record.name;
            output.Size = record.size;
            std::set<std::string> names;
            names.insert(record.name);
            for (const auto& field : record.fields) names.insert(field.name);
            std::uint32_t next = 0;
            std::size_t paddingIndex = 0;
            const auto pad = [&](std::uint32_t end) {
                if (end <= next) return;
                std::string name;
                do { budget.check(); name = "auep_padding_" + std::to_string(paddingIndex++); }
                while (names.contains(name));
                names.insert(name);
                output.Members.push_back({"::std::uint8_t", name + "[" + std::to_string(end - next) + "]", "", next, end - next});
            };
            budget.append(output.Trailer, "static_assert(::std::is_standard_layout_v<" + record.name + ">);\n");
            budget.append(output.Trailer, "static_assert(sizeof(" + record.name + ") == " + std::to_string(record.size) + ");\n");
            budget.append(output.Trailer, "static_assert(alignof(" + record.name + ") == " + std::to_string(record.alignment) + ");\n");
            for (const auto& field : record.fields) {
                budget.check();
                pad(field.offset);
                auto type = field.type.kind == ReflectionTypeKind::Scalar
                    ? std::pair{std::string(scalar(field.type.scalar).name), scalar(field.type.scalar).size}
                    : types.at(field.type.reference);
                const auto bytes = type.second * field.type.count;
                const auto suffix = field.type.count == 1 ? "" : "[" + std::to_string(field.type.count) + "]";
                output.Members.push_back({std::move(type.first), field.name + suffix, "", field.offset, bytes});
                next = field.offset + bytes;
                budget.append(output.Trailer, "static_assert(offsetof(" + record.name + ", " + field.name + ") == " + std::to_string(field.offset) + ");\n");
            }
            pad(record.size);
            records.push_back(std::move(output));
        }
        dumper_emitter::Limits limits;
        limits.maxInputBytes = options.maximumInputBytes + options.maximumOutputBytes;
        limits.maxItems = options.maximumItems * 3;
        limits.maxOutputBytes = options.maximumOutputBytes;
        limits.deadline = options.deadline;
        limits.cancelled = isCancelled;
        limits.cancellationContext = const_cast<std::atomic<bool>*>(options.cancelled);
        auto result = dumper_emitter::emit(records, enumerations, limits);
        if (!result) fail(translate(result.error), "Pinned dumper emission failed");
        std::string header;
        budget.append(header, "#pragma once\n#include <cstddef>\n#include <cstdint>\n#include <type_traits>\n\nnamespace andueprober_sdk {\n");
        budget.append(header, result.output);
        budget.append(header, "} // namespace andueprober_sdk\n");
        budget.check();
        return {{}, std::move(header)};
    } catch (const Failure& error) {
        try { return {{error.code, error.message}, {}}; } catch (...) { return {{error.code, {}}, {}}; }
    } catch (...) { return {{Error::Internal, {}}, {}}; }
}

} // namespace andueprober

#include <andueprober/DumperAdapter.hpp>
#include <andueprober/Export.hpp>
#include "OwnedLayout.hpp"

#include <cstdio>
#include <fstream>
#include <limits>

namespace {
unsigned checks{};
void require(bool condition, const char* name) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", name); throw name; }
}
using namespace andueprober;
Snapshot compiledSnapshot() {
    Snapshot result;
    result.sessionId = "owned-dumper-consumer";
    result.moduleIdentity = "owned-compiled-layout";
    result.layout = Layout::FField;
    result.layoutIdentity = "independent-native-compiler";
    result.generation = 1;
    result.state = TaskState::Succeeded;
    const auto add = [&](const char* name, std::uint32_t offset) {
        Offset value;
        value.value = offset;
        value.origin = Origin::Profile;
        value.validation = Validation::Validated;
        value.evidence.push_back({"compiled offsetof", true, 2, {offset},
                                  "owned native declarations", {"first", "second"}});
        require(bool(publishOffset(result, name, std::move(value))), "publish compiled offset evidence");
    };
    add("Item::Value", offsetof(owned_layout::Item, value));
    add("Item::Sign", offsetof(owned_layout::Item, sign));
    add("Packet::Tag", offsetof(owned_layout::Packet, tag));
    add("Packet::Items", offsetof(owned_layout::Packet, items));
    add("Packet::Weight", offsetof(owned_layout::Packet, weight));
    add("Packet::Mode", offsetof(owned_layout::Packet, mode));
    add("Packet::Address", offsetof(owned_layout::Packet, address));
    return result;
}
ReflectionSchema schema() {
    const std::string metadata = "*/\n#error UNTRUSTED_METADATA_MUST_NOT_BECOME_CODE\n";
    ReflectionSchema result;
    result.identity = metadata;
    result.records = {
        {2, "Packet", sizeof(owned_layout::Packet), alignof(owned_layout::Packet), metadata, {
            {"tag", {ReflectionTypeKind::Scalar, ReflectionScalar::UInt8, 0, 1}, "Packet::Tag"},
            {"items", {ReflectionTypeKind::Record, ReflectionScalar::UInt8, 1, 2}, "Packet::Items"},
            {"weight", {ReflectionTypeKind::Scalar, ReflectionScalar::Float64, 0, 1}, "Packet::Weight"},
            {"mode", {ReflectionTypeKind::Enumeration, ReflectionScalar::UInt8, 3, 1}, "Packet::Mode"},
            {"address", {ReflectionTypeKind::Scalar, ReflectionScalar::Address64, 0, 1}, "Packet::Address"}}},
        {1, "Item", sizeof(owned_layout::Item), alignof(owned_layout::Item), metadata, {
            {"value", {ReflectionTypeKind::Scalar, ReflectionScalar::UInt16, 0, 1}, "Item::Value"},
            {"sign", {ReflectionTypeKind::Scalar, ReflectionScalar::Int8, 0, 1}, "Item::Sign"}}},
        {7, "std", 2, 2, metadata, {
            {"value", {ReflectionTypeKind::Scalar, ReflectionScalar::UInt16, 0, 1}, "Item::Value"}}},
        {8, "Shadow", 24, 8, metadata, {
            {"Item", {ReflectionTypeKind::Record, ReflectionScalar::UInt8, 1, 1}, "Item::Value"},
            {"std", {ReflectionTypeKind::Scalar, ReflectionScalar::UInt64, 0, 1}, "Packet::Weight"}}},
        {9, "auep_padding_0", 4, 2, metadata, {
            {"value", {ReflectionTypeKind::Scalar, ReflectionScalar::UInt16, 0, 1}, "Item::Value"}}},
    };
    result.enumerations = {
        {3, "Mode", ReflectionScalar::UInt8, metadata, {{"First", 1}, {"Last", 255}}},
        {4, "SignedByte", ReflectionScalar::Int8, metadata, {{"Minimum", 0x80}, {"NegativeOne", 0xff}, {"Maximum", 0x7f}}},
        {5, "SignedLong", ReflectionScalar::Int64, metadata, {{"Minimum", 0x8000000000000000ULL}, {"NegativeOne", UINT64_MAX}, {"Maximum", 0x7fffffffffffffffULL}}},
        {6, "UnsignedLong", ReflectionScalar::UInt64, metadata, {{"Maximum", UINT64_MAX}}},
    };
    return result;
}
}

extern "C" int andueprober_owned_dumper(const char* headerPath, const char* exportRoot) try {
    using namespace andueprober;
    checks = 0;
    auto analysis = compiledSnapshot();
    auto model = schema();
    auto frozen = freezeReflection(analysis, model);
    require(bool(frozen.status) && bool(frozen.snapshot), "freeze typed native layout");
    auto result = buildDumperHeader(*frozen.snapshot);
    require(dumperDependencyIdentity().starts_with("AndUEDumper:a03b50cc8ad6b3d6843d407667d484133f507265:emitter-sha256:"),
            "dependency identity comes from checked extraction");
    require(bool(result.status) && !result.header.empty(), "emit typed native layout");
    require(result.header.find("UNTRUSTED_METADATA") == std::string::npos, "metadata is excluded from generated C++");
    require(result.header.find("Minimum = -128LL") != std::string::npos, "signed eight-bit minimum");
    require(result.header.find("NegativeOne = -1LL") != std::string::npos, "signed negative one");
    require(result.header.find("(-9223372036854775807LL - 1)") != std::string::npos, "signed 64-bit minimum");
    require(result.header.find("18446744073709551615ULL") != std::string::npos, "unsigned 64-bit maximum");
    auto repeat = buildDumperHeader(*frozen.snapshot);
    require(repeat.status && repeat.header == result.header, "deterministic immutable emission");
    analysis.offsets.clear(); model.records.clear(); model.enumerations.clear();
    repeat = buildDumperHeader(*frozen.snapshot);
    require(repeat.status && repeat.header == result.header, "emission retains frozen input ownership");
    DumperOptions options;
    options.maximumOutputBytes = result.header.size();
    require(bool(buildDumperHeader(*frozen.snapshot, options).status), "exact complete header capacity");
    --options.maximumOutputBytes;
    auto failure = buildDumperHeader(*frozen.snapshot, options);
    require(failure.status.code == Error::BudgetExceeded && failure.header.empty(), "short capacity returns no partial header");
    options = {}; options.maximumInputBytes = 1;
    failure = buildDumperHeader(*frozen.snapshot, options);
    require(failure.status.code == Error::BudgetExceeded && failure.header.empty(), "typed metadata byte limit");
    options = {}; options.maximumItems = 1;
    failure = buildDumperHeader(*frozen.snapshot, options);
    require(failure.status.code == Error::BudgetExceeded && failure.header.empty(), "typed metadata item limit");
    options = {}; options.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    failure = buildDumperHeader(*frozen.snapshot, options);
    require(failure.status.code == Error::DeadlineExceeded && failure.header.empty(), "formatting deadline");
    std::atomic<bool> cancelled{true}; options = {}; options.cancelled = &cancelled;
    failure = buildDumperHeader(*frozen.snapshot, options);
    require(failure.status.code == Error::Cancelled && failure.header.empty(), "formatting cancellation");
    options = {}; options.maximumOutputBytes = std::numeric_limits<std::size_t>::max();
    failure = buildDumperHeader(*frozen.snapshot, options);
    require(failure.status.code == Error::InvalidArgument && failure.header.empty(), "oversized output limit is rejected");
    analysis = compiledSnapshot(); model = schema();
    model.enumerations[0].values = {{"EM_MAX", 1}, {"Mode_EM_MAX", 2}};
    auto collision = freezeReflection(analysis, model);
    require(bool(collision.status), "macro collision has valid typed input identifiers");
    failure = buildDumperHeader(*collision.snapshot);
    require(failure.status.code == Error::Unsupported && failure.header.empty(), "upstream enum renaming collision is explicit");
    for (const auto* macro : {"NULL", "UINT8_MAX"}) {
        for (unsigned location = 0; location != 3; ++location) {
            model = schema();
            if (location == 0) model.records[0].name = macro;
            if (location == 1) model.records[0].fields[0].name = macro;
            if (location == 2) model.enumerations[0].values[0].name = macro;
            const auto reserved = freezeReflection(analysis, model);
            require(bool(reserved.status), "macro spelling is a valid typed identifier");
            failure = buildDumperHeader(*reserved.snapshot);
            require(failure.status.code == Error::Unsupported && failure.header.empty(), "standard macro spelling returns no generated code");
        }
    }
    std::ofstream output(headerPath);
    output << result.header;
    output.close();
    require(bool(output), "write generated compile fixture");
    ExportOptions publication;
    publication.root = std::filesystem::absolute(exportRoot);
    publication.maximumFiles = 1;
    publication.maximumFileBytes = result.header.size();
    publication.maximumTotalBytes = result.header.size();
    const auto published = publishExport(frozen.snapshot->analysis(), {{"Reflection.hpp", result.header}}, publication);
    require(bool(published.status) && !published.completionUncertain && isCompletedExport(published.publishedDirectory),
            "production exporter publishes generated header");
    std::printf("%u typed dumper consumer checks passed\n", checks);
    return 0;
} catch (...) { return 1; }

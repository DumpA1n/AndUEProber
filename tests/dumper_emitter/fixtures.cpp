#include "Emitter.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>

using namespace andueprober::dumper_emitter;
namespace {
int checks{};
int failures{};
void check(bool value, const char* label) {
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}
void error(const Result& result, Error expected, const char* label) {
    check(result.error == expected, label);
    check(result.output.empty(), "failure does not expose partial output");
}
bool cancelled(void*) noexcept { return true; }
struct CancelCounter { int calls{}; int stopAfter{}; };
bool eventuallyCancelled(void* context) noexcept {
    auto& counter = *static_cast<CancelCounter*>(context);
    return ++counter.calls > counter.stopAfter;
}
} // namespace

int main(int argc, char** argv) {
    ExtractedPackage::Struct record;
    record.FullName = "Script.Owned.OwnedRecord";
    record.CppName = "struct OwnedRecord";
    record.CppNameOnly = "OwnedRecord";
    record.Size = 8;
    record.Members = {{"std::uint32_t", "first", "", 0, 4},
                      {"std::uint16_t", "second", "", 4, 2},
                      {"std::uint8_t", "pad[2]", "", 6, 2}};
    ExtractedPackage::Enum value;
    value.FullName = "Script.Owned.OwnedEnum";
    value.CppName = "enum class OwnedEnum : std::uint8_t";
    value.CppNameOnly = "OwnedEnum";
    value.UnderlyingType = "uint8_t";
    value.Members = {{"Zero", 0}, {"One", 1}, {"EM_MAX", 255}};
    std::vector<ExtractedPackage::Struct> records{record};
    std::vector<ExtractedPackage::Enum> enums{value};
    auto result = emit(records, enums);
    check(bool(result), "owned record and enum emission succeeds");
    check(result.output.find("OwnedEnum_EM_MAX = 255") != std::string::npos, "upstream macro-conflict qualification");
    check(result.output.find("\tstd::uint16_t second; // 0x4(0x2)") != std::string::npos, "upstream member layout formatting");
    check(result.output.find("// Size: 0x8 (Inherited: 0x0)") != std::string::npos, "upstream record size formatting");
    if (argc == 2 && result) {
        std::ofstream output(argv[1]);
        output << "#pragma once\n#include <cstdint>\n" << result.output;
        check(bool(output), "generated header write succeeds");
    }
    Limits limits;
    limits.maxOutputBytes = result.output.size();
    check(bool(emit(records, enums, limits)), "exact output capacity succeeds");
    --limits.maxOutputBytes;
    error(emit(records, enums, limits), Error::OutputLimit, "one-byte short output capacity fails");
    limits = {};
    limits.maxInputBytes = 1;
    error(emit(records, enums, limits), Error::InputLimit, "input bytes are bounded before emission");
    limits = {};
    limits.maxItems = 1;
    error(emit(records, enums, limits), Error::InputLimit, "aggregate input items are bounded");
    limits = {};
    limits.maxOutputBytes = 0;
    error(emit(records, enums, limits), Error::InvalidInput, "zero output capacity is invalid");
    limits.maxOutputBytes = std::numeric_limits<std::size_t>::max();
    error(emit(records, enums, limits), Error::InvalidInput, "oversized allocation request is invalid");
    limits = {};
    limits.cancelled = cancelled;
    error(emit(records, enums, limits), Error::Cancelled, "preflight cancellation is structured");
    limits = {};
    limits.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    error(emit(records, enums, limits), Error::DeadlineExceeded, "expired deadline is structured");
    enums[0].Members.clear();
    error(emit(records, enums), Error::InvalidInput, "empty enum is rejected before upstream underflow");
    enums = {value};
    enums[0].UnderlyingType = "float";
    error(emit(records, enums), Error::Unsupported, "unsupported enum underlying type");
    enums[0] = value;
    enums[0].Members[0].second = 256;
    error(emit(records, enums), Error::InvalidInput, "enum bits must fit underlying width");
    enums[0] = value;
    ExtractedPackage::Function function;
    function.CppName = "void declaredFunction";
    function.FullName = "OwnedRecord.declaredFunction";
    function.Func = 0x1234;
    records[0].Functions.push_back(function);
    error(emit(records, enums), Error::Unsupported, "live function pointer metadata is unsupported");
    records[0].Functions[0].Func = 0;
    result = emit(records, enums);
    check(bool(result), "zero-address declaration emits without a live manager");
    check(result.output.find("void declaredFunction();") != std::string::npos, "upstream function declaration formatting");
    records[0].Members[0].extra.assign(4096, 'x');
    limits = {};
    limits.maxOutputBytes = 256;
    error(emit(records, enums, limits), Error::OutputLimit, "temporary member segment uses the output cap");
    records = {record};
    records[0].Functions.push_back(function);
    records[0].Functions[0].Func = 0;
    records[0].Functions[0].Params.assign(4096, 'x');
    error(emit(records, enums, limits), Error::OutputLimit, "temporary function segment uses the output cap");
    records = {record};
    records[0].Members[0].extra.assign(128 * 1024, 'x');
    limits = {};
    CancelCounter counter{0, 50};
    limits.cancelled = eventuallyCancelled;
    limits.cancellationContext = &counter;
    error(emit(records, enums, limits), Error::Cancelled, "formatting checkpoints observe cancellation");
    check(counter.calls == 51, "cancellation halts at the first requested checkpoint");
    records.clear();
    enums.clear();
    result = emit(records, enums);
    check(bool(result) && result.output.empty(), "empty collection produces an empty successful artifact");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

#include "OwnedAgentFunctions.hpp"
#include "andueprober/Export.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <thread>
#include <unistd.h>
#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
int threadCount() {
    DIR* directory = opendir("/proc/self/task"); REQUIRE(directory);
    int count = 0; while (auto* entry = readdir(directory)) if (entry->d_name[0] != '.') ++count;
    closedir(directory); return count;
}
std::string contents(const std::filesystem::path& path) {
    std::ifstream file(path); REQUIRE(file);
    return {(std::istreambuf_iterator<char>(file)), {}};
}
int main(int argc, char** argv) {
    if (argc != 5) return 2;
    const std::string mode = argv[3];
    REQUIRE(mode == "ffield" || mode == "uproperty" || mode == "invalid-layout" ||
        mode == "corrupt-flags" || mode == "missing" || mode == "no-adapter");
    const bool failed = mode == "corrupt-flags" || mode == "invalid-layout";
    const auto agentPath = std::filesystem::canonical(argv[1]).string();
    const auto modulePath = std::filesystem::canonical(argv[2]).string();
    const auto output = std::filesystem::path(argv[4]) / (mode + "-" + std::to_string(getpid()));
    const auto baseline = threadCount();
    void* module = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(module);
    const auto moduleAddress = reinterpret_cast<std::uintptr_t>(dlsym(module, "andueprober_owned_module_value")); REQUIRE(moduleAddress);
    void* library = dlopen(agentPath.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(library);
    auto initialize = reinterpret_cast<AUEP_Error(*)(const AUEP_Options*)>(dlsym(library, "AUEP_Initialize"));
    auto start = reinterpret_cast<AUEP_Error(*)(const AUEP_FunctionProbeOptions*, const AUEP_LayoutSchema*)>(dlsym(library, "AUEP_StartFunctionLayoutProbe"));
    auto classes = reinterpret_cast<AUEP_Error(*)(const AUEP_ClassProbeOptions*)>(dlsym(library, "AUEP_StartClassProbe"));
    auto stop = reinterpret_cast<AUEP_Error(*)()>(dlsym(library, "AUEP_Stop"));
    auto query = reinterpret_cast<AUEP_Error(*)(AUEP_Result*)>(dlsym(library, "AUEP_Query"));
    REQUIRE(initialize && start && classes && stop && query);
    REQUIRE(start(nullptr, nullptr) == AUEP_INVALID_ARGUMENT);
    AUEP_Options initializeOptions{sizeof(initializeOptions), getprogname(), output.c_str()};
    REQUIRE(initialize(&initializeOptions) == AUEP_OK);
    OwnedFunctions fixture;
    OwnedAgentFunctionConfig config(fixture, modulePath, moduleAddress, mode);
    auto& foundation = config.foundation;
    auto& metadata = config.metadata;
    auto& anchors = config.anchors;
    auto& options = config.options;
    std::array<std::string, 5> fieldNames{"flags", "numParms", "parmsSize", "returnOffset", "nativeFunction"};
    std::array<AUEP_LayoutField, 5> fields{{
        {fieldNames[0].c_str(), "UFunction::FunctionFlags", AUEP_LAYOUT_UINT32},
        {fieldNames[1].c_str(), "UFunction::NumParms", AUEP_LAYOUT_UINT8},
        {fieldNames[2].c_str(), "UFunction::ParmsSize", AUEP_LAYOUT_UINT16},
        {fieldNames[3].c_str(), "UFunction::ReturnValueOffset", AUEP_LAYOUT_UINT16},
        {fieldNames[4].c_str(), "UFunction::Func", AUEP_LAYOUT_ADDRESS64}}};
    std::string schemaIdentity = "owned-function-layout-schema-v1";
    std::string recordName = "OwnedFunctionLayout";
    std::string metadataSource = "compiled-owned-layout:sizeof-and-alignof:OwnedNativeFunction";
    AUEP_LayoutSchema schema{sizeof(schema), schemaIdentity.c_str(), 1, recordName.c_str(),
        sizeof(OwnedNativeFunction), alignof(OwnedNativeFunction), metadataSource.c_str(), fields.size(), fields.data()};
    REQUIRE(start(&options, nullptr) == AUEP_INVALID_ARGUMENT);
    auto invalid = schema; invalid.field_count = 6; REQUIRE(start(&options, &invalid) == AUEP_INVALID_ARGUMENT);
    invalid = schema; invalid.independent_size = UINT32_MAX; REQUIRE(start(&options, &invalid) == AUEP_INVALID_ARGUMENT);
    invalid = schema; invalid.independent_alignment = 3; REQUIRE(start(&options, &invalid) == AUEP_INVALID_ARGUMENT);
    auto bad = fields; bad[0].scalar = AUEP_LAYOUT_UINT8;
    invalid = schema; invalid.fields = bad.data(); REQUIRE(start(&options, &invalid) == AUEP_INVALID_ARGUMENT);
    bad = fields; bad[0].offset_source = "FProperty::ElementSize";
    invalid = schema; invalid.fields = bad.data(); REQUIRE(start(&options, &invalid) == AUEP_UNSUPPORTED);
    bad = fields; bad[0] = bad[1]; invalid = schema; invalid.fields = bad.data();
    REQUIRE(start(&options, &invalid) == AUEP_INVALID_ARGUMENT);
    std::string tooLong(1025, 'x'); invalid = schema; invalid.identity = tooLong.c_str();
    REQUIRE(start(&options, &invalid) == AUEP_INVALID_ARGUMENT);
    REQUIRE(threadCount() == baseline);
    if (mode == "missing" || mode == "no-adapter") REQUIRE(start(&options, &schema) == AUEP_MISSING_DEPENDENCY);
    else {
        if (mode == "corrupt-flags") fixture.functions[1].flags ^= 0x10000000;
        if (mode == "invalid-layout") schema.independent_size = 8;
        REQUIRE(start(&options, &schema) == AUEP_OK);
        REQUIRE(start(&options, &schema) == AUEP_BUSY); REQUIRE(classes(&foundation.options) == AUEP_BUSY);
        for (auto& value : anchors) { value.expected_flags = UINT32_MAX; value.expected_native_function = 1; }
        for (auto& value : metadata) value.identity.assign("overwritten caller metadata");
        for (auto& value : foundation.anchors) value.expected_cast_flags = UINT64_MAX;
        for (auto& value : fieldNames) value.assign("overwritten_field_name");
        recordName.assign("overwritten_record_name"); schemaIdentity.assign("overwritten_schema");
        metadataSource.assign("overwritten_layout_metadata");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        AUEP_Result result{};
        do { REQUIRE(query(&result) == AUEP_OK); if (result.state != AUEP_RUNNING) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (result.state != (failed ? AUEP_FAILURE : AUEP_SUCCEEDED))
            std::fprintf(stderr, "Unexpected Agent status: %d %s\n", result.state, result.message);
        REQUIRE(result.state == (failed ? AUEP_FAILURE : AUEP_SUCCEEDED));
        std::size_t completed = 0;
        if (std::filesystem::exists(output)) for (const auto& entry : std::filesystem::directory_iterator(output)) {
            if (!andueprober::isCompletedExport(entry.path())) continue;
            ++completed;
            REQUIRE(!std::filesystem::exists(entry.path() / "struct-observations.txt"));
            REQUIRE(!std::filesystem::exists(entry.path() / "class-observations.txt"));
            const auto text = contents(entry.path() / "function-observations.txt");
            const std::array<std::pair<const char*, std::size_t>, 5> fields{{
                {"FunctionFlags", offsetof(OwnedNativeFunction, flags)}, {"NumParms", offsetof(OwnedNativeFunction, numParms)},
                {"ParmsSize", offsetof(OwnedNativeFunction, parmsSize)}, {"ReturnValueOffset", offsetof(OwnedNativeFunction, returnOffset)},
                {"Func", offsetof(OwnedNativeFunction, nativeFunction)}}};
            for (const auto& [name, offset] : fields)
                REQUIRE(text.find("UFunction::" + std::string(name) + " = " + std::to_string(offset)) != std::string::npos);
            const auto header = contents(entry.path() / "SDK/FunctionLayout.hpp");
            REQUIRE(header.find("OwnedFunctionLayout") != std::string::npos);
            REQUIRE(header.find("overwritten") == std::string::npos);
            REQUIRE(header.find("ProcessEvent") == std::string::npos);
            const auto declaration = contents(entry.path() / "layout-schema.txt");
            REQUIRE(declaration.find("compiled-owned-layout:sizeof-and-alignof:OwnedNativeFunction") != std::string::npos);
            std::printf("SDK_PATH=%s\n", (entry.path() / "SDK/FunctionLayout.hpp").c_str());
            const auto manifest = contents(entry.path() / "completion.json");
            REQUIRE(manifest.find("AndUEDumper.DetachedEmitter") != std::string::npos);
            REQUIRE(manifest.find("owned-compiled-function-metadata-v1") != std::string::npos);
            REQUIRE(manifest.find("\"UStruct::SuperStruct\":1") != std::string::npos);
            REQUIRE(manifest.find("overwritten caller metadata") == std::string::npos);
        }
        REQUIRE(completed == (failed ? 0u : 1u));
    }
    REQUIRE(stop() == AUEP_OK); REQUIRE(stop() == AUEP_OK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (threadCount() != baseline && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(threadCount() == baseline); REQUIRE(ownedNativeFunctionCalls.load() == 0);
    REQUIRE(dlclose(library) == 0); REQUIRE(dlclose(module) == 0);
    REQUIRE(dlopen(modulePath.c_str(), RTLD_NOW | RTLD_NOLOAD) == nullptr);
    std::printf("PASS: owned function layout Agent mode=%s, copied schema, frozen observed offsets and pinned emitter single publication; zero native invocations, data-layout subset only; generated header compilation is a separate check\n", mode.c_str());
}

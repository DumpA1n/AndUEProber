#include "OwnedAgentProperties.hpp"
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
    REQUIRE(mode == "ffield" || mode == "corrupt-size" || mode == "corrupt-offset" ||
        mode == "corrupt-flags" || mode == "ambiguous" || mode == "missing");
    const bool failed = mode == "corrupt-size" || mode == "corrupt-offset" || mode == "corrupt-flags" || mode == "ambiguous";
    const auto agentPath = std::filesystem::canonical(argv[1]).string();
    const auto modulePath = std::filesystem::canonical(argv[2]).string();
    const auto output = std::filesystem::path(argv[4]) / (mode + "-" + std::to_string(getpid()));
    const auto baseline = threadCount();
    void* module = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(module);
    const auto moduleAddress = reinterpret_cast<std::uintptr_t>(dlsym(module, "andueprober_owned_module_value")); REQUIRE(moduleAddress);
    void* library = dlopen(agentPath.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(library);
    auto initialize = reinterpret_cast<AUEP_Error(*)(const AUEP_Options*)>(dlsym(library, "AUEP_Initialize"));
    auto start = reinterpret_cast<AUEP_Error(*)(const AUEP_PropertyProbeOptions*)>(dlsym(library, "AUEP_StartPropertyProbe"));
    auto classes = reinterpret_cast<AUEP_Error(*)(const AUEP_ClassProbeOptions*)>(dlsym(library, "AUEP_StartClassProbe"));
    auto stop = reinterpret_cast<AUEP_Error(*)()>(dlsym(library, "AUEP_Stop"));
    auto query = reinterpret_cast<AUEP_Error(*)(AUEP_Result*)>(dlsym(library, "AUEP_Query"));
    REQUIRE(initialize && start && classes && stop && query);
    REQUIRE(start(nullptr) == AUEP_INVALID_ARGUMENT);
    AUEP_Options initializeOptions{sizeof(initializeOptions), getprogname(), output.c_str()};
    REQUIRE(initialize(&initializeOptions) == AUEP_OK);
    OwnedProperties fixture;
    OwnedAgentPropertyConfig config(fixture, modulePath, moduleAddress, mode);
    auto& foundation = config.foundation;
    auto& metadata = config.metadata;
    auto& anchors = config.anchors;
    auto& options = config.options;
    auto invalid = options; invalid.property_anchor_count = 17; REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    invalid = options; invalid.property_extent = UINT32_MAX; REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    invalid = options; invalid.field_base_extent = 0; REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    invalid = options; invalid.field_base_extent = invalid.property_extent; REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    invalid = options; invalid.fields.owner_representation = 2; REQUIRE(start(&invalid) == AUEP_UNSUPPORTED);
    invalid = options; invalid.fields.functions.classes.structures.indices.layout = 1; REQUIRE(start(&invalid) == AUEP_UNSUPPORTED);
    auto bad = anchors; bad[0].object = std::numeric_limits<std::uintptr_t>::max() - 8;
    invalid = options; invalid.property_anchors = bad.data(); REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    bad = anchors; bad[0].expected_array_dim = 0;
    invalid = options; invalid.property_anchors = bad.data(); REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    bad = anchors; bad[0].expected_element_size = -1;
    invalid = options; invalid.property_anchors = bad.data(); REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    bad = anchors; bad[0].expected_offset_internal = -1;
    invalid = options; invalid.property_anchors = bad.data(); REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    bad = anchors; bad[0].containing_value_size = 1;
    invalid = options; invalid.property_anchors = bad.data(); REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    bad = anchors; bad[0].expected_array_dim = INT32_MAX; bad[0].expected_element_size = INT32_MAX;
    invalid = options; invalid.property_anchors = bad.data(); REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    std::string tooLong(1025, 'x'); invalid = options; invalid.metadata_id = tooLong.c_str();
    REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    REQUIRE(threadCount() == baseline);
    if (mode == "missing") REQUIRE(start(&options) == AUEP_MISSING_DEPENDENCY);
    else {
        if (mode == "corrupt-size") fixture.properties[1].elementSize = -1;
        if (mode == "corrupt-offset") fixture.properties[1].offsetInternal = 0x7fffffff;
        if (mode == "corrupt-flags") fixture.properties[1].propertyFlags ^= 0x8000000000000000ULL;
        if (mode == "ambiguous") for (auto& value : fixture.properties) value.duplicatePropertyFlags = value.propertyFlags;
        REQUIRE(start(&options) == AUEP_OK);
        REQUIRE(start(&options) == AUEP_BUSY); REQUIRE(classes(&foundation.foundation.foundation.options) == AUEP_BUSY);
        for (auto& value : anchors) { value.expected_property_flags = UINT64_MAX; value.containing_value_size = 1; }
        for (auto& value : metadata) value.identity.assign("overwritten caller metadata");
        for (auto& value : foundation.anchors) value.expected_flags = UINT32_MAX;
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
            const auto text = contents(entry.path() / "property-observations.txt");
            REQUIRE(!std::filesystem::exists(entry.path() / "field-base-observations.txt"));
            REQUIRE(!std::filesystem::exists(entry.path() / "function-observations.txt"));
            const std::array<std::pair<const char*, std::size_t>, 4> fields{{
                {"ArrayDim", offsetof(OwnedNativeProperty, arrayDim)}, {"ElementSize", offsetof(OwnedNativeProperty, elementSize)},
                {"PropertyFlags", offsetof(OwnedNativeProperty, propertyFlags)}, {"Offset_Internal", offsetof(OwnedNativeProperty, offsetInternal)}}};
            for (const auto& [name, offset] : fields)
                REQUIRE(text.find("FProperty::" + std::string(name) + " = " + std::to_string(offset)) != std::string::npos);
            REQUIRE(text.find("FProperty::Size") == std::string::npos);
            REQUIRE(text.find("FArrayProperty::") == std::string::npos);
            const auto manifest = contents(entry.path() / "completion.json");
            REQUIRE(manifest.find("owned-compiled-property-metadata-v1") != std::string::npos);
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
    std::printf("PASS: owned FProperty scalar Agent mode=%s, independent containing-value bounds, copied metadata and single frozen publication; zero native invocations, no complete subclass/container phase or UE SDK\n", mode.c_str());
}

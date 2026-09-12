#include "OwnedAgentStructs.hpp"
#include "OwnedEnums.hpp"
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
    REQUIRE(mode == "ffield" || mode == "uproperty" || mode == "reordered" || mode == "corrupt-value" ||
        mode == "corrupt-name" || mode == "corrupt-count" || mode == "ambiguous" || mode == "missing");
    const bool failed = mode.starts_with("corrupt-") || mode == "ambiguous";
    const auto agentPath = std::filesystem::canonical(argv[1]).string();
    const auto modulePath = std::filesystem::canonical(argv[2]).string();
    const auto output = std::filesystem::path(argv[4]) / (mode + "-" + std::to_string(getpid()));
    const auto baseline = threadCount();
    void* module = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(module);
    const auto moduleAddress = reinterpret_cast<std::uintptr_t>(dlsym(module, "andueprober_owned_module_value")); REQUIRE(moduleAddress);
    void* library = dlopen(agentPath.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(library);
    auto initialize = reinterpret_cast<AUEP_Error(*)(const AUEP_Options*)>(dlsym(library, "AUEP_Initialize"));
    auto start = reinterpret_cast<AUEP_Error(*)(const AUEP_EnumProbeOptions*)>(dlsym(library, "AUEP_StartEnumProbe"));
    auto stop = reinterpret_cast<AUEP_Error(*)()>(dlsym(library, "AUEP_Stop"));
    auto query = reinterpret_cast<AUEP_Error(*)(AUEP_Result*)>(dlsym(library, "AUEP_Query"));
    REQUIRE(initialize && start && stop && query); REQUIRE(start(nullptr) == AUEP_INVALID_ARGUMENT);
    AUEP_Options initializeOptions{sizeof(initializeOptions), getprogname(), output.c_str()};
    REQUIRE(initialize(&initializeOptions) == AUEP_OK);
    OwnedEnums fixture(mode == "reordered", mode == "reordered");
    OwnedAgentStructConfig foundation(fixture.phase2, modulePath, moduleAddress, mode);
    auto metadata = fixture.samples(); const auto profile = fixture.profile();
    std::array<std::vector<AUEP_EnumValueAnchor>, 3> values;
    std::array<AUEP_EnumAnchor, 3> anchors;
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        for (const auto& value : metadata[i].values)
            values[i].push_back({value.expectedName.c_str(), value.identity.c_str(), value.expectedValue});
        anchors[i] = {metadata[i].object, metadata[i].identity.c_str(), static_cast<uint32_t>(values[i].size()), values[i].data()};
    }
    AUEP_EnumProbeOptions options{sizeof(options), foundation.options, "owned-enum-metadata-v1",
        profile.extent, profile.fieldBaseExtent, static_cast<uint32_t>(anchors.size()),
        *profile.array.data, *profile.array.count, *profile.array.capacity, profile.array.size,
        *profile.entry.name, *profile.entry.value, profile.entry.stride, profile.maximumValues, profile.maximumCapacity, anchors.data()};
    options.structures.indices.session_id = "owned-enum-agent";
    auto invalid = options; invalid.enum_anchor_count = 17; REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    invalid = options; invalid.enum_extent = UINT32_MAX; REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    invalid = options; invalid.array_size = 8; REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    auto bad = anchors; bad[0].value_count = 16385; invalid = options; invalid.enum_anchors = bad.data();
    REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    bad = anchors; bad[0].object = std::numeric_limits<std::uintptr_t>::max() - 8;
    REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    std::string tooLong(1025, 'x'); invalid = options; invalid.metadata_id = tooLong.c_str();
    REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT); REQUIRE(threadCount() == baseline);
    if (mode == "missing") REQUIRE(start(&options) == AUEP_MISSING_DEPENDENCY);
    else {
        if (mode == "corrupt-value") fixture.entries[1][1].value = 11;
        if (mode == "corrupt-name") fixture.entries[2][2].name = 0;
        if (mode == "corrupt-count") fixture.enums[2].names.count = 2;
        if (mode == "ambiguous") for (auto& value : fixture.enums) value.duplicate = value.names;
        REQUIRE(start(&options) == AUEP_OK); REQUIRE(start(&options) == AUEP_BUSY);
        for (auto& array : values) for (auto& value : array) value.expected_value = 112233;
        for (auto& value : metadata) {
            value.identity.assign("overwritten caller enum");
            for (auto& entry : value.values) { entry.expectedName.assign("overwritten caller name"); entry.identity.assign("overwritten caller entry"); }
        }
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
            const auto text = contents(entry.path() / "enum-observations.txt");
            REQUIRE(text.find("UEnum::Names = " + std::to_string(fixture.namesOffset())) != std::string::npos);
            REQUIRE(text.find("UField::Next = " + std::to_string(offsetof(OwnedNativeField, next))) != std::string::npos);
            REQUIRE(text.find("ProcessEvent::") == std::string::npos && text.find("UClass::") == std::string::npos);
            REQUIRE(text.find("FProperty::") == std::string::npos);
            const auto manifest = contents(entry.path() / "completion.json");
            REQUIRE(manifest.find("owned-enum-metadata-v1") != std::string::npos);
            REQUIRE(manifest.find("owned-enum:2:entry:2") != std::string::npos);
            REQUIRE(manifest.find("\"UField::Next\":1") != std::string::npos);
            REQUIRE(manifest.find("overwritten caller") == std::string::npos);
        }
        REQUIRE(completed == (failed ? 0u : 1u));
    }
    REQUIRE(stop() == AUEP_OK); REQUIRE(stop() == AUEP_OK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (threadCount() != baseline && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(threadCount() == baseline);
    REQUIRE(dlclose(library) == 0); REQUIRE(dlclose(module) == 0);
    REQUIRE(dlopen(modulePath.c_str(), RTLD_NOW | RTLD_NOLOAD) == nullptr);
    std::printf("PASS: owned enum Agent mode=%s, copied complete names and signed values, bounded layout, frozen dependency evidence; no ProcessEvent or class/property prerequisite\n", mode.c_str());
}

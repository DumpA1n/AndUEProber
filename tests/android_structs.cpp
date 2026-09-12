#include "OwnedAgentStructs.hpp"
#include "andueprober/Agent.h"
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
int main(int argc, char** argv) {
    if (argc != 5) return 2;
    const std::string mode = argv[3];
    REQUIRE(mode == "ffield" || mode == "uproperty" || mode == "no-alignment" || mode == "corrupt" || mode == "missing");
    const auto agentPath = std::filesystem::canonical(argv[1]).string();
    const auto modulePath = std::filesystem::canonical(argv[2]).string();
    const auto output = std::filesystem::path(argv[4]) / (mode + "-" + std::to_string(getpid()));
    const auto baseline = threadCount();
    void* module = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(module);
    const auto moduleAddress = reinterpret_cast<std::uintptr_t>(dlsym(module, "andueprober_owned_module_value")); REQUIRE(moduleAddress);
    void* library = dlopen(agentPath.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(library);
    auto initialize = reinterpret_cast<AUEP_Error(*)(const AUEP_Options*)>(dlsym(library, "AUEP_Initialize"));
    auto start = reinterpret_cast<AUEP_Error(*)(const AUEP_StructProbeOptions*)>(dlsym(library, "AUEP_StartStructProbe"));
    auto stop = reinterpret_cast<AUEP_Error(*)()>(dlsym(library, "AUEP_Stop"));
    auto query = reinterpret_cast<AUEP_Error(*)(AUEP_Result*)>(dlsym(library, "AUEP_Query"));
    REQUIRE(initialize && start && stop && query);
    REQUIRE(start(nullptr) == AUEP_INVALID_ARGUMENT);
    AUEP_Options initializeOptions{sizeof(initializeOptions), getprogname(), output.c_str()};
    REQUIRE(initialize(&initializeOptions) == AUEP_OK);
    OwnedStructs fixture;
    OwnedAgentStructConfig config(fixture, modulePath, moduleAddress, mode);
    auto& options = config.options;
    auto& structAnchors = config.structAnchors;
    auto& fieldAnchors = config.fieldAnchors;
    auto& objectAnchors = config.foundation.anchors;
    auto& metadata = config.metadata;
    auto invalid = options; invalid.struct_anchor_count = 5;
    REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    invalid = options; invalid.field_extent = UINT32_MAX;
    REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    invalid = options; auto bad = objectAnchors; bad[0].object = std::numeric_limits<std::uintptr_t>::max() - 8;
    invalid.object_anchors = bad.data(); REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    std::string tooLong(1025, 'x'); invalid = options; invalid.metadata_id = tooLong.c_str();
    REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    auto absent = structAnchors; absent[0].has_child_properties = 0;
    invalid = options; invalid.indices.layout = 2; invalid.struct_anchors = absent.data();
    REQUIRE(start(&invalid) == AUEP_UNSUPPORTED);
    REQUIRE(threadCount() == baseline);
    if (mode == "missing") {
        REQUIRE(start(&options) == AUEP_MISSING_DEPENDENCY);
    } else {
        if (mode == "corrupt") fixture.structs[1].propertiesSize ^= 0x10;
        REQUIRE(start(&options) == AUEP_OK);
        REQUIRE(start(&options) == AUEP_BUSY);
        // Input metadata is copied before start returns; addresses still refer to owned live data.
        for (auto& value : structAnchors) value.expected_properties_size = UINT32_MAX;
        for (auto& value : fieldAnchors) value.expected_next = 1;
        for (auto& value : metadata) value.identity.assign("caller metadata overwritten");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        AUEP_Result result{};
        do { REQUIRE(query(&result) == AUEP_OK); if (result.state != AUEP_RUNNING) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (result.state != (mode == "corrupt" ? AUEP_FAILURE : AUEP_SUCCEEDED))
            std::fprintf(stderr, "Unexpected Agent status: %d %s\n", result.state, result.message);
        REQUIRE(result.state == (mode == "corrupt" ? AUEP_FAILURE : AUEP_SUCCEEDED));
        std::size_t completed = 0;
        if (std::filesystem::exists(output)) for (const auto& entry : std::filesystem::directory_iterator(output)) {
            if (!andueprober::isCompletedExport(entry.path())) continue;
            ++completed;
            std::ifstream input(entry.path() / "struct-observations.txt");
            const std::string text((std::istreambuf_iterator<char>(input)), {});
            REQUIRE(text.find("UField::Next = " + std::to_string(offsetof(OwnedNativeField, next))) != std::string::npos);
            REQUIRE(text.find("UStruct::PropertiesSize = " + std::to_string(offsetof(OwnedNativeStruct, propertiesSize))) != std::string::npos);
            REQUIRE(text.find("sizeof(UObject)") == std::string::npos);
            REQUIRE((text.find("UStruct::MinAlignment") != std::string::npos) == (mode != "no-alignment"));
            REQUIRE((text.find("UStruct::ChildProperties") != std::string::npos) == (mode != "uproperty"));
        }
        REQUIRE(completed == (mode == "corrupt" ? 0u : 1u));
    }
    REQUIRE(stop() == AUEP_OK); REQUIRE(stop() == AUEP_OK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (threadCount() != baseline && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(threadCount() == baseline);
    REQUIRE(dlclose(library) == 0); REQUIRE(dlclose(module) == 0);
    REQUIRE(dlopen(modulePath.c_str(), RTLD_NOW | RTLD_NOLOAD) == nullptr);
    std::printf("PASS: owned struct Agent mode=%s, bounded metadata admission, copied configuration and joined ownership; no UE engine or full SDK\n", mode.c_str());
}

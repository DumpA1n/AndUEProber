#include "OwnedAgentObjects.hpp"
#include "OwnedObjectFlags.hpp"
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
    REQUIRE(mode == "ffield" || mode == "uproperty" || mode == "zero" ||
        mode == "corrupt" || mode == "ambiguous" || mode == "missing");
    const bool failed = mode == "corrupt" || mode == "ambiguous";
    const auto agentPath = std::filesystem::canonical(argv[1]).string();
    const auto modulePath = std::filesystem::canonical(argv[2]).string();
    const auto output = std::filesystem::path(argv[4]) / (mode + "-" + std::to_string(getpid()));
    const auto baseline = threadCount();
    void* module = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(module);
    const auto moduleAddress = reinterpret_cast<std::uintptr_t>(dlsym(module, "andueprober_owned_module_value")); REQUIRE(moduleAddress);
    void* library = dlopen(agentPath.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(library);
    auto initialize = reinterpret_cast<AUEP_Error(*)(const AUEP_Options*)>(dlsym(library, "AUEP_Initialize"));
    auto start = reinterpret_cast<AUEP_Error(*)(const AUEP_ObjectFlagProbeOptions*)>(dlsym(library, "AUEP_StartObjectFlagProbe"));
    auto indices = reinterpret_cast<AUEP_Error(*)(const AUEP_ObjectIndexOptions*)>(dlsym(library, "AUEP_StartIndexProbe"));
    auto stop = reinterpret_cast<AUEP_Error(*)()>(dlsym(library, "AUEP_Stop"));
    auto query = reinterpret_cast<AUEP_Error(*)(AUEP_Result*)>(dlsym(library, "AUEP_Query"));
    REQUIRE(initialize && start && indices && stop && query);
    REQUIRE(start(nullptr) == AUEP_INVALID_ARGUMENT);
    AUEP_Options initializeOptions{sizeof(initializeOptions), getprogname(), output.c_str()};
    REQUIRE(initialize(&initializeOptions) == AUEP_OK);
    OwnedObjectFlags fixture(mode == "zero");
    OwnedAgentObjectConfig foundation(fixture.memory, modulePath, moduleAddress, mode);
    auto metadata = fixture.samples();
    std::array<AUEP_ObjectFlagAnchor, 3> anchors;
    for (std::size_t i = 0; i < anchors.size(); ++i)
        anchors[i] = {metadata[i].object, metadata[i].identity.c_str(), metadata[i].expectedFlags};
    AUEP_ObjectFlagProbeOptions options{sizeof(options), foundation.indices, foundation.pool, "owned-object-flags-v1",
        static_cast<std::uint32_t>(foundation.anchors.size()), static_cast<std::uint32_t>(anchors.size()),
        fixture.profile().extent, foundation.anchors.data(), anchors.data()};
    options.indices.session_id = "owned-object-flags-agent";
    options.indices.object_extent = fixture.profile().extent;
    auto invalid = options; invalid.flag_anchor_count = 17; REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    invalid = options; invalid.flag_extent = UINT32_MAX; REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    invalid = options; invalid.object_anchor_count = 17; REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    auto bad = anchors; bad[0].object = std::numeric_limits<std::uintptr_t>::max() - 8;
    invalid = options; invalid.flag_anchors = bad.data(); REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    std::string tooLong(1025, 'x'); invalid = options; invalid.metadata_id = tooLong.c_str();
    REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    REQUIRE(threadCount() == baseline);
    if (mode == "missing") REQUIRE(start(&options) == AUEP_MISSING_DEPENDENCY);
    else {
        if (mode == "corrupt") fixture.memory.put<std::uint32_t>(2048 + 2 * 64 + 4, OwnedObjectFlags::flags[2] ^ 0x10u);
        if (mode == "ambiguous") for (std::size_t i = 0; i < 4; ++i)
            fixture.memory.put<std::uint32_t>(2048 + i * 64 + 40, OwnedObjectFlags::flags[i]);
        REQUIRE(start(&options) == AUEP_OK);
        REQUIRE(start(&options) == AUEP_BUSY); REQUIRE(indices(&options.indices) == AUEP_BUSY);
        for (auto& value : anchors) { value.expected_flags = 0xDEADBEEFu; }
        for (auto& value : metadata) value.identity.assign("overwritten caller metadata");
        for (auto& value : foundation.anchors) value.expected_outer = 1;
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
            const auto text = contents(entry.path() / "object-flag-observations.txt");
            REQUIRE(text.find("UObject::ObjectFlags = " + std::to_string(mode == "zero" ? 0 : 4)) != std::string::npos);
            REQUIRE(text.find("UObject::InternalIndex = " + std::to_string(mode == "zero" ? 4 : 0)) != std::string::npos);
            REQUIRE(text.find("UStruct::") == std::string::npos);
            REQUIRE(text.find("UClass::") == std::string::npos);
            const auto manifest = contents(entry.path() / "completion.json");
            REQUIRE(manifest.find("owned-object-flags-v1") != std::string::npos);
            REQUIRE(manifest.find("\"UObject::NamePrivate\":1") != std::string::npos);
            REQUIRE(manifest.find("overwritten caller metadata") == std::string::npos);
        }
        REQUIRE(completed == (failed ? 0u : 1u));
    }
    REQUIRE(stop() == AUEP_OK); REQUIRE(stop() == AUEP_OK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (threadCount() != baseline && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(threadCount() == baseline);
    REQUIRE(dlclose(library) == 0); REQUIRE(dlclose(module) == 0);
    REQUIRE(dlopen(modulePath.c_str(), RTLD_NOW | RTLD_NOLOAD) == nullptr);
    std::printf("PASS: owned UObject flags Agent mode=%s, copied independent uint32 metadata, five Core fields and single frozen publication; no mask, engine call or unrelated structure metadata\n", mode.c_str());
}

#include "andueprober/Agent.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    const auto module = std::filesystem::canonical(argv[1]).string();
    const bool chunked = std::string(argv[2]) == "chunked", corrupt = std::string(argv[2]) == "corrupt";
    const bool missing = std::string(argv[2]) == "missing";
    void* library = dlopen(module.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(library);
    auto initialize = reinterpret_cast<decltype(&AUEP_Initialize)>(dlsym(library, "AUEP_Initialize"));
    auto start = reinterpret_cast<decltype(&AUEP_StartIndexProbe)>(dlsym(library, "AUEP_StartIndexProbe"));
    auto query = reinterpret_cast<decltype(&AUEP_Query)>(dlsym(library, "AUEP_Query"));
    auto stop = reinterpret_cast<decltype(&AUEP_Stop)>(dlsym(library, "AUEP_Stop"));
    REQUIRE(initialize && start && query && stop);
    AUEP_Options options{sizeof(options), getprogname(), argv[3]};
    REQUIRE(initialize(&options) == AUEP_OK);
    struct Object { uint32_t index; std::array<uint32_t, 7> padding; };
    struct Item { uint64_t flags; Object* object; uint64_t serial; };
    std::array<Object, 4> objects;
    std::array<Item, 4> items;
    for (uint32_t i = 0; i < 4; ++i) {
        objects[i].index = i; objects[i].padding.fill(0x5a5a5a5a);
        items[i] = {0xaaaaaaaa, &objects[i], 0xbbbbbbbb};
    }
    if (corrupt) items[1].object = items[0].object;
    std::array<Item*, 2> chunks{items.data(), items.data() + 2};
    struct Array { void* objects; int32_t count, capacity, chunks, chunkCapacity; } array{
        chunked ? static_cast<void*>(chunks.data()) : static_cast<void*>(items.data()), 4, 4, 2, 2};
    const std::string session = std::string(argv[2]) + "-" + std::to_string(getpid());
    AUEP_ObjectIndexOptions profile{};
    profile.struct_size = sizeof(profile); profile.module_path = module.c_str();
    profile.module_address = reinterpret_cast<uintptr_t>(start);
    profile.object_array = reinterpret_cast<uintptr_t>(&array);
    profile.profile_id = "owned-native-object-array-v1"; profile.session_id = session.c_str(); profile.layout = 1;
    profile.objects_offset = offsetof(Array, objects); profile.count_offset = offsetof(Array, count);
    profile.capacity_offset = offsetof(Array, capacity); profile.item_stride = sizeof(Item);
    profile.item_object_offset = offsetof(Item, object); profile.chunk_count_offset = offsetof(Array, chunks);
    profile.chunk_capacity_offset = offsetof(Array, chunkCapacity); profile.elements_per_chunk = chunked ? 2 : 0;
    profile.maximum_objects = 16; profile.maximum_examined = 4; profile.sample_limit = 4; profile.object_extent = sizeof(Object);
    const auto started = start(&profile);
    if (missing) {
        REQUIRE(started == AUEP_MISSING_DEPENDENCY); REQUIRE(stop() == AUEP_OK);
        REQUIRE(dlclose(library) == 0);
        std::puts("PASS: unavailable process-memory dependency rejected before analysis"); return 0;
    }
    REQUIRE(started == AUEP_OK); REQUIRE(start(&profile) == AUEP_BUSY);
    AUEP_Result result{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    do {
        REQUIRE(query(&result) == AUEP_OK);
        if (result.state != AUEP_RUNNING) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    if (result.state != (corrupt ? AUEP_FAILURE : AUEP_SUCCEEDED)) std::fprintf(stderr, "state=%d message=%s\n", result.state, result.message);
    REQUIRE(result.state == (corrupt ? AUEP_FAILURE : AUEP_SUCCEEDED));
    REQUIRE(stop() == AUEP_OK); REQUIRE(stop() == AUEP_OK);
    const auto output = std::filesystem::path(argv[3]) / session;
    REQUIRE(std::filesystem::exists(output / "completion.json") == !corrupt);
    if (!corrupt) {
        std::ifstream file(output / "completion.json");
        const std::string manifest((std::istreambuf_iterator<char>(file)), {});
        REQUIRE(manifest.find("gnu-build-id:") != std::string::npos);
        REQUIRE(manifest.find("owned-native-object-array-v1") != std::string::npos);
        REQUIRE(manifest.find("object-array-index:3") != std::string::npos);
        REQUIRE(manifest.find("\"UObject::InternalIndex\":{\"value\":0") != std::string::npos);
        REQUIRE(manifest.find("working-tree-memory-source-sha256:") != std::string::npos);
    }
    REQUIRE(dlclose(library) == 0);
    std::puts(corrupt ? "PASS: corrupt owned object array rejected without completed export" :
        "PASS: owned object array -> public memory adapter -> Core probe -> session -> frozen validated export; no engine calls or full SDK");
}

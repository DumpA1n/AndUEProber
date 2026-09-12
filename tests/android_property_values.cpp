#include "OwnedAgentProperties.hpp"
#include "OwnedPropertyValues.hpp"
#include "andueprober/Export.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>
#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
int threadCount() {
    DIR* directory = opendir("/proc/self/task"); REQUIRE(directory); int count = 0;
    while (auto* entry = readdir(directory)) if (entry->d_name[0] != '.') ++count;
    closedir(directory); return count;
}
std::string contents(const std::filesystem::path& path) {
    std::ifstream file(path); REQUIRE(file); return {(std::istreambuf_iterator<char>(file)), {}};
}
int main(int argc, char** argv) {
    if (argc != 5) return 2;
    const std::string mode = argv[3];
    REQUIRE(mode == "bool" || mode == "path" || mode == "corrupt-bool" || mode == "corrupt-path" || mode == "ambiguous-bool" ||
        mode == "ambiguous-path" || mode == "prefix-bool" || mode == "prefix-path" || mode == "native-only" || mode == "bit-only" || mode == "missing");
    const bool path = mode.ends_with("path"), missing = mode == "missing", failed = mode != "bool" && mode != "path" && !missing;
    const auto agentPath = std::filesystem::canonical(argv[1]).string(), modulePath = std::filesystem::canonical(argv[2]).string();
    const auto output = std::filesystem::path(argv[4]) / (mode + "-" + std::to_string(getpid()));
    const auto baseline = threadCount();
    void* module = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(module);
    const auto moduleAddress = reinterpret_cast<std::uintptr_t>(dlsym(module, "andueprober_owned_module_value")); REQUIRE(moduleAddress);
    void* library = dlopen(agentPath.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(library);
    auto initialize = reinterpret_cast<AUEP_Error(*)(const AUEP_Options*)>(dlsym(library, "AUEP_Initialize"));
    auto startBool = reinterpret_cast<AUEP_Error(*)(const AUEP_BoolPropertyProbeOptions*)>(dlsym(library, "AUEP_StartBoolPropertyProbe"));
    auto startPath = reinterpret_cast<AUEP_Error(*)(const AUEP_FieldPathPropertyProbeOptions*)>(dlsym(library, "AUEP_StartFieldPathPropertyProbe"));
    auto stop = reinterpret_cast<AUEP_Error(*)()>(dlsym(library, "AUEP_Stop"));
    auto query = reinterpret_cast<AUEP_Error(*)(AUEP_Result*)>(dlsym(library, "AUEP_Query"));
    REQUIRE(initialize && startBool && startPath && stop && query);
    REQUIRE(startBool(nullptr) == AUEP_INVALID_ARGUMENT); REQUIRE(startPath(nullptr) == AUEP_INVALID_ARGUMENT);
    AUEP_Options initializeOptions{sizeof(initializeOptions), getprogname(), output.c_str()}; REQUIRE(initialize(&initializeOptions) == AUEP_OK);
    OwnedPropertyValues fixture; OwnedAgentPropertyConfig foundation(fixture.phase5, modulePath, moduleAddress, "ffield");
    auto boolMetadata = fixture.boolSamples(); auto pathMetadata = fixture.pathSamples();
    const auto boolProfile = fixture.boolProfile(); const auto pathProfile = fixture.pathProfile();
    std::array<AUEP_BoolPropertyAnchor, 3> boolAnchors; std::array<AUEP_FieldPathPropertyAnchor, 3> pathAnchors;
    for (std::size_t i = 0; i < boolAnchors.size(); ++i) {
        const auto& value = boolMetadata[i];
        boolAnchors[i] = {value.object, value.identity.c_str(), static_cast<AUEP_BoolEncoding>(value.encoding), value.fieldSize,
            value.byteOffset, value.byteMask, value.fieldMask, value.storageExtent, value.storageIdentity.c_str()};
        pathAnchors[i] = {pathMetadata[i].object, pathMetadata[i].identity.c_str(), pathMetadata[i].expectedName.c_str()};
    }
    AUEP_BoolPropertyProbeOptions boolOptions{sizeof(boolOptions), foundation.options, boolProfile.identity.c_str(), boolProfile.extent,
        boolProfile.propertyBaseExtent, 3, boolAnchors.data()};
    AUEP_FieldPathPropertyProbeOptions pathOptions{sizeof(pathOptions), foundation.options, pathProfile.identity.c_str(), AUEP_FIELD_PATH_INLINE_FNAME,
        pathProfile.extent, pathProfile.propertyBaseExtent, 3, pathAnchors.data()};
    boolOptions.properties.fields.functions.classes.structures.indices.session_id = "owned-bool-property-agent";
    pathOptions.properties.fields.functions.classes.structures.indices.session_id = "owned-fieldpath-property-agent";
    auto badBool = boolOptions; badBool.struct_size = 1; REQUIRE(startBool(&badBool) == AUEP_INVALID_ARGUMENT);
    badBool = boolOptions; badBool.anchor_count = 17; REQUIRE(startBool(&badBool) == AUEP_INVALID_ARGUMENT);
    badBool = boolOptions; badBool.metadata_extent = 4097; REQUIRE(startBool(&badBool) == AUEP_INVALID_ARGUMENT);
    badBool = boolOptions; badBool.property_base_extent = 0; REQUIRE(startBool(&badBool) == AUEP_INVALID_ARGUMENT);
    auto badAnchors = boolAnchors; badBool = boolOptions; badBool.anchors = badAnchors.data();
    badAnchors[0].encoding = AUEP_BOOL_UNKNOWN; REQUIRE(startBool(&badBool) == AUEP_UNSUPPORTED);
    badAnchors = boolAnchors; badAnchors[0].storage_extent = 256; REQUIRE(startBool(&badBool) == AUEP_INVALID_ARGUMENT);
    badAnchors = boolAnchors; badAnchors[1].byte_mask = 3; REQUIRE(startBool(&badBool) == AUEP_INVALID_ARGUMENT);
    badAnchors = boolAnchors; badAnchors[0].field_mask = 1; REQUIRE(startBool(&badBool) == AUEP_INVALID_ARGUMENT);
    badAnchors = boolAnchors; badAnchors[0].object = UINTPTR_MAX - 1; REQUIRE(startBool(&badBool) == AUEP_INVALID_ARGUMENT);
    auto badPath = pathOptions; badPath.representation = AUEP_FIELD_PATH_UNKNOWN; REQUIRE(startPath(&badPath) == AUEP_UNSUPPORTED);
    badPath = pathOptions; badPath.anchor_count = 2; REQUIRE(startPath(&badPath) == AUEP_INVALID_ARGUMENT);
    badPath = pathOptions; badPath.property_base_extent = badPath.metadata_extent; REQUIRE(startPath(&badPath) == AUEP_INVALID_ARGUMENT);
    const std::string tooLong(1025, 'x'); badPath = pathOptions; badPath.metadata_id = tooLong.c_str(); REQUIRE(startPath(&badPath) == AUEP_INVALID_ARGUMENT);
    REQUIRE(threadCount() == baseline);
    if (missing) {
        REQUIRE(startBool(&boolOptions) == AUEP_MISSING_DEPENDENCY); REQUIRE(startPath(&pathOptions) == AUEP_MISSING_DEPENDENCY);
    } else {
        if (mode == "corrupt-bool") ++fixture.booleans[1].fieldMask;
        if (mode == "corrupt-path") ++fixture.paths[1].propertyClass.number;
        if (mode == "ambiguous-bool") for (std::size_t i = 0; i < 3; ++i) fixture.booleans[i].duplicate[0] = boolMetadata[i].fieldSize;
        if (mode == "ambiguous-path") for (auto& value : fixture.paths) value.duplicate = value.propertyClass;
        if (mode == "prefix-bool") boolOptions.property_base_extent = 8;
        if (mode == "prefix-path") pathOptions.property_base_extent = 8;
        if (mode == "native-only" || mode == "bit-only") for (std::size_t i = 0; i < 3; ++i) {
            const auto values = mode == "native-only" ? fixture.expected[0] : fixture.expected[i ? i : 1]; fixture.setBool(i, values);
            boolAnchors[i].encoding = mode == "native-only" ? AUEP_BOOL_NATIVE_BYTE : AUEP_BOOL_SINGLE_BIT;
            boolAnchors[i].field_size = values[0]; boolAnchors[i].byte_offset = values[1]; boolAnchors[i].byte_mask = values[2]; boolAnchors[i].field_mask = values[3];
            boolAnchors[i].storage_extent = values[0];
        }
        const auto start = [&] { return path ? startPath(&pathOptions) : startBool(&boolOptions); };
        REQUIRE(start() == AUEP_OK); REQUIRE(start() == AUEP_BUSY);
        for (auto& value : boolAnchors) { value.byte_mask = 0; value.field_mask = 0; }
        for (auto& value : boolMetadata) { value.identity.assign("overwritten caller bool"); value.storageIdentity.assign("overwritten caller storage"); }
        for (auto& value : pathMetadata) { value.identity.assign("overwritten caller path"); value.expectedName.assign("overwritten caller expected"); }
        AUEP_Result result{}; const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            REQUIRE(query(&result) == AUEP_OK); if (result.state != AUEP_RUNNING) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        if (result.state != (failed ? AUEP_FAILURE : AUEP_SUCCEEDED)) std::fprintf(stderr, "Agent state=%d message=%s\n", result.state, result.message);
        REQUIRE(result.state == (failed ? AUEP_FAILURE : AUEP_SUCCEEDED));
        std::size_t completed = 0;
        if (std::filesystem::exists(output)) for (const auto& entry : std::filesystem::directory_iterator(output)) {
            if (!andueprober::isCompletedExport(entry.path())) continue;
            ++completed; const auto text = contents(entry.path() / (path ? "field-path-property-observations.txt" : "bool-property-observations.txt"));
            if (path) REQUIRE(text.find("FFieldPathProperty::PropertyClass = " + std::to_string(offsetof(OwnedPathMetadata, propertyClass))) != std::string::npos);
            else {
                const std::array<std::string, 4> fields{"FieldSize", "ByteOffset", "ByteMask", "FieldMask"};
                for (std::size_t i = 0; i < fields.size(); ++i) REQUIRE(text.find("FBoolProperty::" + fields[i] + " = " + std::to_string(fixture.boolOffsets[i])) != std::string::npos);
            }
            REQUIRE(text.find("FProperty::PropertyFlags = ") != std::string::npos);
            const auto manifest = contents(entry.path() / "completion.json");
            REQUIRE(manifest.find(path ? pathProfile.identity : boolProfile.identity) != std::string::npos);
            REQUIRE(manifest.find("\"FProperty::ArrayDim\":1") != std::string::npos); REQUIRE(manifest.find("overwritten caller") == std::string::npos);
        }
        REQUIRE(completed == (failed ? 0u : 1u));
    }
    REQUIRE(stop() == AUEP_OK); REQUIRE(stop() == AUEP_OK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (threadCount() != baseline && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(threadCount() == baseline); REQUIRE(ownedNativeFunctionCalls == 0);
    REQUIRE(dlclose(library) == 0); REQUIRE(dlclose(module) == 0); REQUIRE(dlopen(modulePath.c_str(), RTLD_NOW | RTLD_NOLOAD) == nullptr);
    std::printf("PASS: owned Bool/FieldPath Agent mode=%s; copied independent metadata, bounded observation, frozen publication, no engine calls\n", mode.c_str());
}

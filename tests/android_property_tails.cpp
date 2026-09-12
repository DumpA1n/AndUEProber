#include "OwnedAgentProperties.hpp"
#include "OwnedPropertyTails.hpp"
#include "andueprober/Export.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sys/mman.h>
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
    REQUIRE(mode == "enum" || mode == "array" || mode == "set" || mode == "map" || mode == "object" ||
        mode == "struct" || mode == "byte" || mode == "class" || mode == "interface" ||
        mode == "corrupt-first" || mode == "corrupt-second" || mode == "ambiguous" || mode == "prefix" || mode == "missing");
    using andueprober::PropertyTailKind;
    const auto kind = mode == "array" ? PropertyTailKind::Array : mode == "set" ? PropertyTailKind::Set :
        mode == "map" ? PropertyTailKind::Map : mode == "object" ? PropertyTailKind::Object :
        mode == "struct" ? PropertyTailKind::Struct : mode == "byte" ? PropertyTailKind::Byte :
        mode == "class" ? PropertyTailKind::Class : mode == "interface" ? PropertyTailKind::Interface : PropertyTailKind::Enum;
    const bool failed = mode.starts_with("corrupt-") || mode == "ambiguous" || mode == "prefix";
    const auto agentPath = std::filesystem::canonical(argv[1]).string();
    const auto modulePath = std::filesystem::canonical(argv[2]).string();
    const auto output = std::filesystem::path(argv[4]) / (mode + "-" + std::to_string(getpid()));
    const auto baseline = threadCount();
    void* module = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(module);
    const auto moduleAddress = reinterpret_cast<std::uintptr_t>(dlsym(module, "andueprober_owned_module_value")); REQUIRE(moduleAddress);
    void* library = dlopen(agentPath.c_str(), RTLD_NOW | RTLD_LOCAL); REQUIRE(library);
    auto initialize = reinterpret_cast<AUEP_Error(*)(const AUEP_Options*)>(dlsym(library, "AUEP_Initialize"));
    auto start = reinterpret_cast<AUEP_Error(*)(const AUEP_PropertyTailProbeOptions*)>(dlsym(library, "AUEP_StartPropertyTailProbe"));
    auto stop = reinterpret_cast<AUEP_Error(*)()>(dlsym(library, "AUEP_Stop"));
    auto query = reinterpret_cast<AUEP_Error(*)(AUEP_Result*)>(dlsym(library, "AUEP_Query"));
    REQUIRE(initialize && start && stop && query); REQUIRE(start(nullptr) == AUEP_INVALID_ARGUMENT);
    AUEP_Options initializeOptions{sizeof(initializeOptions), getprogname(), output.c_str()};
    REQUIRE(initialize(&initializeOptions) == AUEP_OK);
    OwnedPropertyTails fixture;
    OwnedAgentPropertyConfig foundation(fixture.phase5, modulePath, moduleAddress, "ffield");
    const auto pageSize = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    void* opaque = mmap(nullptr, pageSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); REQUIRE(opaque != MAP_FAILED);
    auto metadata = fixture.samples(kind); const auto profile = fixture.profile(kind);
    // Declared target addresses identify owned unreadable storage, never field values to infer.
    for (std::size_t i = 0; i < metadata.size(); ++i) {
        metadata[i].expectedFirst = reinterpret_cast<std::uintptr_t>(opaque) + (i % 2) * 16;
        fixture.setPointer(kind, i, 0, metadata[i].expectedFirst);
        if (fixture.dual(kind)) {
            metadata[i].expectedSecond = reinterpret_cast<std::uintptr_t>(opaque) + 128 + (i % 2) * 16;
            fixture.setPointer(kind, i, 1, *metadata[i].expectedSecond);
        }
    }
    std::array<AUEP_PropertyTailAnchor, 3> anchors;
    for (std::size_t i = 0; i < anchors.size(); ++i) {
        const auto& value = metadata[i];
        anchors[i] = {value.object, value.identity.c_str(), value.expectedFirst, value.firstIdentity.c_str(),
            value.expectedSecond ? 1u : 0u, value.expectedSecond.value_or(0), value.secondIdentity.c_str()};
    }
    AUEP_PropertyTailProbeOptions options{sizeof(options), foundation.options, profile.identity.c_str(),
        static_cast<AUEP_PropertyTailKind>(kind), profile.extent, profile.propertyBaseExtent,
        static_cast<std::uint32_t>(anchors.size()), anchors.data()};
    options.properties.fields.functions.classes.structures.indices.session_id = "owned-property-tail-agent";
    auto invalid = options; invalid.kind = AUEP_TAIL_UNKNOWN; REQUIRE(start(&invalid) == AUEP_UNSUPPORTED);
    invalid = options; invalid.kind = AUEP_TAIL_BOOL; REQUIRE(start(&invalid) == AUEP_UNSUPPORTED);
    invalid = options; invalid.tail_anchor_count = 17; REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    invalid = options; invalid.tail_extent = UINT32_MAX; REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    invalid = options; invalid.property_base_extent = 0; REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    auto bad = anchors; bad[0].has_second = fixture.dual(kind) ? 0u : 1u;
    invalid = options; invalid.tail_anchors = bad.data(); REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    bad = anchors; bad[0].object = std::numeric_limits<std::uintptr_t>::max() - 8;
    REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT);
    std::string tooLong(1025, 'x'); invalid = options; invalid.metadata_id = tooLong.c_str();
    REQUIRE(start(&invalid) == AUEP_INVALID_ARGUMENT); REQUIRE(threadCount() == baseline);
    if (mode == "missing") REQUIRE(start(&options) == AUEP_MISSING_DEPENDENCY);
    else {
        if (mode == "corrupt-first") fixture.setPointer(kind, 1, 0, metadata[1].expectedFirst + 1);
        if (mode == "corrupt-second") fixture.setPointer(kind, 1, 1, *metadata[1].expectedSecond + 1);
        if (mode == "ambiguous") for (std::size_t i = 0; i < metadata.size(); ++i)
            fixture.setPointer(kind, i, 0, metadata[i].expectedFirst, true);
        if (mode == "prefix") options.property_base_extent = 8;
        REQUIRE(start(&options) == AUEP_OK); REQUIRE(start(&options) == AUEP_BUSY);
        for (auto& value : anchors) { value.expected_first = 1; value.expected_second = 1; }
        for (auto& value : metadata) { value.identity.assign("overwritten caller tail"); value.firstIdentity.assign("overwritten caller target"); }
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
            const auto text = contents(entry.path() / "property-tail-observations.txt");
            const auto first = kind == PropertyTailKind::Enum ? "FEnumProperty::UnderlyingType" : kind == PropertyTailKind::Array ?
                "FArrayProperty::Inner" : kind == PropertyTailKind::Set ? "FSetProperty::ElementProp" :
                kind == PropertyTailKind::Map ? "FMapProperty::KeyProp" : kind == PropertyTailKind::Struct ? "FStructProperty::Struct" :
                kind == PropertyTailKind::Byte ? "FByteProperty::Enum" : kind == PropertyTailKind::Interface ?
                "FInterfaceProperty::InterfaceClass" : "FObjectPropertyBase::PropertyClass";
            REQUIRE(text.find(std::string(first) + " = " + std::to_string(fixture.offset(kind, 0))) != std::string::npos);
            if (fixture.dual(kind)) {
                const auto second = kind == PropertyTailKind::Enum ? "FEnumProperty::Enum" :
                    kind == PropertyTailKind::Class ? "FClassProperty::MetaClass" : "FMapProperty::ValueProp";
                REQUIRE(text.find(std::string(second) + " = " + std::to_string(fixture.offset(kind, 1))) != std::string::npos);
                REQUIRE(kind == PropertyTailKind::Class ? fixture.offset(kind, 1) > fixture.offset(kind, 0) + 8 :
                    fixture.offset(kind, 0) > fixture.offset(kind, 1) + 8);
            }
            REQUIRE(text.find("FProperty::PropertyFlags = ") != std::string::npos);
            REQUIRE(text.find("FBoolProperty::") == std::string::npos && text.find("FProperty::Size") == std::string::npos);
            const auto manifest = contents(entry.path() / "completion.json");
            REQUIRE(manifest.find(profile.identity) != std::string::npos);
            REQUIRE(manifest.find("\"FProperty::ArrayDim\":1") != std::string::npos);
            REQUIRE(manifest.find("overwritten caller") == std::string::npos);
        }
        REQUIRE(completed == (failed ? 0u : 1u));
    }
    REQUIRE(stop() == AUEP_OK); REQUIRE(stop() == AUEP_OK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (threadCount() != baseline && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(threadCount() == baseline); REQUIRE(ownedNativeFunctionCalls.load() == 0);
    REQUIRE(munmap(opaque, pageSize) == 0);
    REQUIRE(dlclose(library) == 0); REQUIRE(dlclose(module) == 0);
    REQUIRE(dlopen(modulePath.c_str(), RTLD_NOW | RTLD_NOLOAD) == nullptr);
    std::printf("PASS: owned property-tail Agent mode=%s, declared opaque pointer columns and prefix, copied metadata, unreadable pointees, frozen dependencies; no Bool or complete container claim\n", mode.c_str());
}

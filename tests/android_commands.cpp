#include "andueprober/Agent.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <dlfcn.h>
#include <thread>
#include <string>

#define REQUIRE(value) do { if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
int threads() {
    auto* directory = opendir("/proc/self/task"); REQUIRE(directory); int count = 0;
    while (auto* entry = readdir(directory)) if (entry->d_name[0] != '.') ++count;
    closedir(directory); return count;
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const bool missing = std::string(argv[2]) == "missing";
    const auto baselineThreads = threads();
    auto* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); REQUIRE(library);
    auto initialize = reinterpret_cast<AUEP_Error(*)(const AUEP_Options*)>(dlsym(library, "AUEP_Initialize"));
    auto start = reinterpret_cast<AUEP_Error(*)()>(dlsym(library, "AUEP_StartInteractive"));
    auto automatic = reinterpret_cast<AUEP_Error(*)()>(dlsym(library, "AUEP_Start"));
    auto submit = reinterpret_cast<AUEP_Error(*)(const AUEP_Command*, uint64_t*)>(dlsym(library, "AUEP_Submit"));
    auto query = reinterpret_cast<AUEP_Error(*)(AUEP_CommandResult*)>(dlsym(library, "AUEP_QueryCommands"));
    auto draw = reinterpret_cast<AUEP_Error(*)(void*)>(dlsym(library, "AUEP_DrawInspector"));
    auto cancel = reinterpret_cast<AUEP_Error(*)()>(dlsym(library, "AUEP_Cancel"));
    auto stop = reinterpret_cast<AUEP_Error(*)()>(dlsym(library, "AUEP_Stop"));
    REQUIRE(initialize && start && automatic && submit && query && draw && cancel && stop);
    REQUIRE(start() == AUEP_NOT_INITIALIZED); REQUIRE(draw(nullptr) == AUEP_NOT_INITIALIZED);
    AUEP_Options options{sizeof(options), getprogname(), "/data/local/tmp/andueprober-memory-owned/commands-output"};
    REQUIRE(initialize(&options) == AUEP_OK);
    if (missing) {
        REQUIRE(start() == AUEP_MISSING_DEPENDENCY); REQUIRE(threads() == baselineThreads);
    } else {
        REQUIRE(start() == AUEP_OK); REQUIRE(start() == AUEP_BUSY); REQUIRE(automatic() == AUEP_BUSY);
        AUEP_CommandResult state{}; state.struct_size = sizeof(state);
        REQUIRE(query(&state) == AUEP_OK && state.accepting && !state.pending && !state.completed && !state.running);
        REQUIRE(draw(nullptr) == AUEP_INVALID_ARGUMENT);
        AUEP_Command command{}; command.struct_size = sizeof(command); command.kind = AUEP_PROBE_PHASE; command.phase = 7;
        uint64_t id = 99; REQUIRE(submit(&command, &id) == AUEP_INVALID_ARGUMENT && !id);
        command.phase = 1; command.generation = 999;
        REQUIRE(submit(&command, &id) == AUEP_OK && id == 1);
        auto wait = [&](AUEP_Error expected = AUEP_FAILED) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            do { REQUIRE(query(&state) == AUEP_OK); if (state.completed == id) break; std::this_thread::yield(); }
            while (std::chrono::steady_clock::now() < deadline);
            REQUIRE(state.completed == id && state.operation.error == expected);
        };
        wait();
        command.kind = AUEP_DETECT; command.phase = 0; command.generation = 0;
        REQUIRE(submit(&command, &id) == AUEP_OK && id == 2); wait(AUEP_UNSUPPORTED);
        REQUIRE(state.generation == 0 && state.accepting);
        REQUIRE(cancel() == AUEP_OK); REQUIRE(submit(&command, &id) == AUEP_BUSY && !id);
    }
    REQUIRE(stop() == AUEP_OK); REQUIRE(stop() == AUEP_OK);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (threads() != baselineThreads && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    REQUIRE(threads() == baselineThreads); REQUIRE(dlclose(library) == 0);
    std::puts(missing ? "PASS: explicit interactive start rejects missing Memory dependency without a worker" :
        "PASS: idle interactive owner, explicit command admission, stale/unavailable profile failures, cancellation and joined worker; no UE engine executed");
}

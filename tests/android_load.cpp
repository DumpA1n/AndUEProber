#include "andueprober/Agent.h"
#include <dirent.h>
#include <dlfcn.h>
#include <jni.h>
#include <signal.h>
#include <sys/prctl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int threadCount() {
    DIR* directory = opendir("/proc/self/task");
    if (!directory) return -1;
    int count = 0;
    while (auto* item = readdir(directory)) if (item->d_name[0] != '.') ++count;
    closedir(directory);
    return count;
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    struct sigaction oldSegv{}, oldBus{}, newSegv{}, newBus{};
    if (sigaction(SIGSEGV, nullptr, &oldSegv) || sigaction(SIGBUS, nullptr, &oldBus)) return 3;
    int threads = threadCount();
    int dumpable = prctl(PR_GET_DUMPABLE);
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::fprintf(stderr, "%s\n", dlerror()); return 4; }
    auto onLoad = reinterpret_cast<jint(*)(JavaVM*, void*)>(dlsym(library, "JNI_OnLoad"));
    if (!onLoad || onLoad(nullptr, reinterpret_cast<void*>(1337)) != JNI_VERSION_1_6) return 5;
    if (sigaction(SIGSEGV, nullptr, &newSegv) || sigaction(SIGBUS, nullptr, &newBus)) return 6;
    if (threadCount() != threads || prctl(PR_GET_DUMPABLE) != dumpable ||
        oldSegv.sa_handler != newSegv.sa_handler || oldBus.sa_handler != newBus.sa_handler ||
        oldSegv.sa_flags != newSegv.sa_flags || oldBus.sa_flags != newBus.sa_flags) return 7;
    auto query = reinterpret_cast<AUEP_Error(*)(AUEP_Result*)>(dlsym(library, "AUEP_Query"));
    auto initialize = reinterpret_cast<AUEP_Error(*)(const AUEP_Options*)>(dlsym(library, "AUEP_Initialize"));
    auto stop = reinterpret_cast<AUEP_Error(*)()>(dlsym(library, "AUEP_Stop"));
    if (!query || !initialize || !stop) return 8;
    AUEP_Result result{};
    if (query(&result) != AUEP_OK || result.state != AUEP_CREATED) return 9;
    AUEP_Options options{sizeof(AUEP_Options), "deliberately-wrong-owned-fixture", "/data/local/tmp/andueprober-owned-output"};
    if (initialize(&options) != AUEP_TARGET_MISMATCH) return 10;
    options.expected_package = getprogname();
    if (initialize(&options) != AUEP_OK || query(&result) != AUEP_OK || result.state != AUEP_READY) return 11;
    if (stop() != AUEP_OK || stop() != AUEP_OK || threadCount() != threads) return 12;
    if (dlclose(library) != 0) return 13;
    std::puts("PASS: inert JNI loading, explicit target selection, idempotent stop; no analysis or engine calls executed");
    return 0;
}

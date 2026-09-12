#include <cstdio>
#include <dlfcn.h>
#include <filesystem>

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    std::filesystem::create_directories(std::filesystem::path(argv[2]).parent_path());
    std::filesystem::remove_all(argv[3]);
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::fprintf(stderr, "%s\n", dlerror()); return 3; }
    using Function = int (*)(const char*, const char*);
    auto function = reinterpret_cast<Function>(dlsym(library, "andueprober_owned_dumper"));
    int result = function ? function(argv[2], argv[3]) : 4;
    if (dlclose(library) && !result) result = 5;
    return result;
}

#include <dlfcn.h>
#include <cstdio>
#include <filesystem>

int main(int argc, char** argv) {
    if (argc != 3) return 1;
    const auto output = std::filesystem::absolute(argv[2]);
    std::filesystem::remove_all(output);
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::fprintf(stderr, "%s\n", dlerror()); return 2; }
    const auto run = reinterpret_cast<int(*)(const char*)>(dlsym(library, "andueprober_owned_component"));
    const auto result = run ? run(output.c_str()) : 3;
    if (dlclose(library) != 0) return 4;
    std::filesystem::remove_all(output);
    return result;
}

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>

#define MAKE_CRASH()     \
    __asm__ volatile (   \
        "mov x0, xzr;"   \
        "mov x29, x0;"   \
        "mov sp, x0;"    \
        "br x0;"         \
        : : :            \
    );

template<int64_t v>
inline int64_t Return() { return v; }

template <typename Ret, typename... Args>
Ret CallFunc(uintptr_t address, Args... args) {
    using orig_func_t = Ret (*)(Args...);
    orig_func_t f = (orig_func_t)address;
    return f(args...);
}

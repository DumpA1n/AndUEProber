#include "BackTracer.h"

#include <utility>
#include <mutex>
#include <unordered_map>
#include <pthread.h>

namespace BackTracer {

std::string Trim(const std::string& str)
{
    size_t pos = str.find_last_of("/");
    if (pos != std::string::npos) {
        return str.substr(pos + 1);
    }
    return str;
}

void Trace(int32_t max_depth, bool use_cache, const tracer_callback_t& callback)
{
    static std::mutex g_mutex;
    static std::unordered_map<uintptr_t, Dl_info> g_cache;

    uintptr_t fp = 0;
    int32_t depth = 0;
    void* stack_base = nullptr;
    size_t stack_size = 0;
    uintptr_t stack_top = UINTPTR_MAX;

    constexpr auto Strip = [](uintptr_t address) -> uintptr_t {
        return address & 0x7fffffffff;
    };

    constexpr auto IsValid = [](uintptr_t address) -> bool {
        return address >= 0x10000000 && address <= 0x7fffffffff && (address & 0x3) == 0;
    };

#ifdef __APPLE__
    stack_base = pthread_get_stackaddr_np(pthread_self());
    stack_size = pthread_get_stacksize_np(pthread_self());
    stack_top = (uintptr_t)stack_base;
    stack_base = (void*)((uintptr_t)stack_base - stack_size);
#else
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        pthread_attr_getstack(&attr, &stack_base, &stack_size);
        stack_top = (uintptr_t)stack_base + stack_size;
        pthread_attr_destroy(&attr);
    }
#endif

    asm volatile ("ldr %0, [x29];" : "=r" (fp));

    std::lock_guard<std::mutex> lock(g_mutex);

    while (true) {
        fp = Strip(fp);

        if (!IsValid(fp)) {
            break;
        }
        if (fp < (uintptr_t)stack_base || fp > stack_top) {
            break;
        }

        uintptr_t lr = Strip(*(uintptr_t*)(fp + 0x8));

        if (!IsValid(lr)) {
            break;
        }

        const uint32_t insn = *(uint32_t*)(lr - 0x4);
        if ((insn & 0xfc000000) == 0x94000000) {
            /* BL <imm26> */
        } else if ((insn & 0xfffffc1f) == 0xd63f0000) {
            /* BLR <reg> */
        } else if ((insn & 0xfffffc1f) == 0xd63f081f) {
            /* BLRAAZ <reg> */
        } else {
            break;
        }

        Dl_info dl_info{};
        if (use_cache && g_cache.find(lr) != g_cache.end()) {
            dl_info = g_cache[lr];
        } else {
            if (dladdr((const void*)lr, &dl_info)) {
                if (use_cache) {
                    g_cache.emplace(lr, dl_info);
                }
            }
        }

        if (callback != nullptr) {
            callback({ fp, lr, std::move(dl_info), depth });
        }

        depth++;
        if (depth > max_depth) {
            break;
        }

        fp = *(uintptr_t*)fp;
    }
}

__attribute__((always_inline))
void Trace(const tracer_callback_t& callback)
{
    Trace(128, true, callback);
}

}

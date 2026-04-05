#include "IPointerHook.h"

#include <sys/mman.h>

#include "KittyMemory/KittyMemory.hpp"
#include "Logger.h"

#define MAKE_CRASH()     \
    __asm__ volatile (   \
        "mov x0, xzr;"   \
        "mov x29, x0;"   \
        "mov sp, x0;"    \
        "br x0;"         \
        : : :            \
    );

std::array<std::atomic<IPointerHook*>, IPointerHook::MAX_HOOKS> IPointerHook::g_Hacks_{};
std::atomic<uint32_t> IPointerHook::g_Index_{0};

IPointerHook::IPointerHook()
    : initialized_(false)
    , prepared_(false)
    , installed_(false)
    , index_(0)
    , orig_ptr_addr_(0)
    , orig_func_addr_(0)
    , fake_func_addr_(0)
{
}

IPointerHook::~IPointerHook()
{
    DestroyHook();
}

void IPointerHook::Initialize()
{
    orig_ptr_addr_ = GetPtrAddrImpl();

    if (GetFuncAddrImpl()) {
        orig_func_addr_ = GetFuncAddrImpl();
    } else {
        uintptr_t temp = 0;
        if (KittyMemory::memRead((void*)orig_ptr_addr_, &temp, sizeof(uintptr_t)) && temp != 0) {
            orig_func_addr_ = temp;
        } else {
            LOGE("[%s] Failed to initialize: orig_func_addr_ is null", GetName().c_str());
            return;
        }
    }

    initialized_ = true;

    LOGI("[%s] Initialized", GetName().c_str());
}

bool IPointerHook::PrepareTrampoline()
{
    if (!initialized_) {
        return false;
    }

    void *mapped_address = x_mmap(4096);
    LOGI("[%s] mmap: %p", GetName().c_str(), mapped_address);

    index_ = g_Index_.fetch_add(1);

    uintptr_t addr = (uintptr_t)mapped_address;

    size_t get_size = (size_t)((uintptr_t)get_regs_end - (uintptr_t)get_regs);
    memcpy((void*)addr, (const void*)&get_regs, get_size);

    uintptr_t d_addr = (uintptr_t)&IPointerHook::Dispatcher;
    uint32_t shellcode[] = {
        // mov x1, #index
        (uint32_t)(0xD2800001 | ((index_ & 0xFFFF) << 5)),

        // mov x16, #(addr & 0xFFFF)
        (uint32_t)(0xD2800010 | (((d_addr >> 0)  & 0xFFFF) << 5)),

        // movk x16, #((addr >> 16) & 0xFFFF, lsl #16
        (uint32_t)(0xF2A00010 | (((d_addr >> 16) & 0xFFFF) << 5)),

        // movk x16, #((addr >> 32) & 0xFFFF, lsl #32
        (uint32_t)(0xF2C00010 | (((d_addr >> 32) & 0xFFFF) << 5)),

        // movk x16, #((addr >> 48) & 0xFFFF, lsl #48
        (uint32_t)(0xF2E00010 | (((d_addr >> 48) & 0xFFFF) << 5)),

        // blr x16
        (uint32_t)0xD63F0200,
    };
    memcpy((void*)(addr + get_size), (const void*)shellcode, sizeof(shellcode));

    size_t set_size = (size_t)((uintptr_t)set_regs_end - (uintptr_t)set_regs);
    memcpy((void*)(addr + get_size + sizeof(shellcode)), (const void*)&set_regs, set_size);

    fake_func_addr_ = addr;

    return true;
}

void IPointerHook::InstallHook()
{
    if (!prepared_ && PrepareTrampoline()) {
        prepared_ = true;
    }

    if (index_ >= MAX_HOOKS) {
        LOGE("[%s] InstallHook failed: index %u exceeds MAX_HOOKS %zu", GetName().c_str(), index_, MAX_HOOKS);
        return;
    }

    g_Hacks_[index_].store(this, std::memory_order_release);

    if (!KittyMemory::memWrite((void*)orig_ptr_addr_, &fake_func_addr_, sizeof(uintptr_t))) {
        LOGE("[%s] InstallHook failed: memWrite error at %p", GetName().c_str(), (void*)orig_ptr_addr_);
        return;
    }

    installed_ = true;

    LOGI("[%s] InstallHook: orig_ptr_addr_: %p, orig_func_addr_: %p, fake_func_addr_: %p",
        GetName().c_str(),
        (void*)orig_ptr_addr_,
        (void*)orig_func_addr_,
        (void*)fake_func_addr_);
}

void IPointerHook::RestoreHook()
{
    if (installed_) {
        if (!KittyMemory::memWrite((void*)orig_ptr_addr_, &orig_func_addr_, sizeof(uintptr_t))) {
            LOGE("[%s] RestoreHook failed: memWrite error at %p", GetName().c_str(), (void*)orig_ptr_addr_);
        }

        installed_ = false;

        LOGI("[%s] RestoreHook", GetName().c_str());
    }
}

void IPointerHook::DestroyHook()
{
    if (initialized_) {
        if (!KittyMemory::memWrite((void*)orig_ptr_addr_, &orig_func_addr_, sizeof(uintptr_t))) {
            LOGE("[%s] DestroyHook failed: memWrite error at %p", GetName().c_str(), (void*)orig_ptr_addr_);
        }
        x_munmap((void*)fake_func_addr_, 4096);
        if (index_ < MAX_HOOKS) {
            g_Hacks_[index_].store(nullptr, std::memory_order_release);
        }
        index_ = 0;
        orig_ptr_addr_ = 0;
        orig_func_addr_ = 0;
        fake_func_addr_ = 0;
        initialized_ = false;
    }
}

void *IPointerHook::x_mmap(size_t size)
{
    void *addr = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        LOGE("[!] mmap failed: %s", strerror(errno));
        MAKE_CRASH();
    }
    return addr;
}

bool IPointerHook::x_munmap(void *addr, size_t size)
{
    if (munmap(addr, size) == -1) {
        LOGE("[!] munmap failed: %s", strerror(errno));
        MAKE_CRASH();
    }
    return true;
}

uintptr_t IPointerHook::Dispatcher(RegContext *ctx, uint32_t index)
{
    if (index >= MAX_HOOKS) {
        return 0;
    }

    IPointerHook* hook = g_Hacks_[index].load(std::memory_order_acquire);
    if (hook == nullptr) {
        return 0;
    }

    return hook->FakeFunction(ctx);
}

#pragma once

#include <sys/mman.h>
#include <sys/prctl.h>

#include "HookUtils.h"
#include "IPointerHook.h"
#include "Logger.h"

class SafePointerHook : virtual public IPointerHook {
public:
    SafePointerHook() : IPointerHook() {}
    virtual ~SafePointerHook() override = default;

    void InstallHook() override {
        IPointerHook::InstallHook();
    }

    void DestroyHook() override {
        IPointerHook::DestroyHook();
    }

protected:
    void *x_mmap(size_t size) override {
        return (void*)IPointerHook::x_mmap(size);
    }

    bool x_munmap(void *addr, size_t size) override {
        return IPointerHook::x_munmap(addr, size);
    }
};

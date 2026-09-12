#pragma once
#include "andueprober/Engine.hpp"
#include <atomic>
#include <thread>

struct EngineFixtureObject {
    int mode = 0;
    std::thread::id owner = std::this_thread::get_id();
    std::atomic<int> calls{0}, allocations{0}, releases{0}, outstanding{0}, wrongThread{0};
    void* control = nullptr;
    void (*onInvoke)(void*) = nullptr;
    void (*onRelease)(void*) = nullptr;
};
using FixtureProviderFunction = void (*)(andueprober::CompiledTextProvider*);

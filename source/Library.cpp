#include <atomic>
#include <cstdint>
#include <jni.h>
#include <stdlib.h>
#include <string>
#include <thread>
#include <unistd.h>

#include <KittyUtils.hpp>

#include "Dump.hpp"

// Gives the engine time to register its startup objects after injection.
constexpr unsigned kWaitSeconds = 8;

// Match the injector activation contract used by AndUEProber.
constexpr uintptr_t kAndKittyMagic = 1337;
constexpr uintptr_t kZygoteInjectorMagic = 2006124;

extern "C" jint JNIEXPORT JNI_OnLoad(JavaVM *, void *key)
{
    static std::atomic<bool> started{false};
    const uintptr_t magic = reinterpret_cast<uintptr_t>(key);
    if ((magic == kAndKittyMagic || magic == kZygoteInjectorMagic) && !started.exchange(true))
    {
        std::thread([]
        {
            sleep(kWaitSeconds);
            const std::string package = getprogname();
            DumpGame(getpid(), package, KittyUtils::Android::getAppExternalFilesDir(package));
        }).detach();
    }
    return JNI_VERSION_1_6;
}

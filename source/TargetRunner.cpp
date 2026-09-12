#include "UEProber/DumperBridge.h"
#include "UEProber/UEProber.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s <pid> <package> <absolute-output-root>\n", argv[0]);
        return 2;
    }
    char* end = nullptr;
    errno = 0;
    const auto parsed = std::strtol(argv[1], &end, 10);
    if (errno || !end || *end || parsed <= 0 || parsed > INT_MAX) {
        std::fputs("invalid target PID\n", stderr);
        return 2;
    }
    std::atomic<bool> cancelled{false};
    ConfigureProbeOperation(&cancelled);
    const auto configured = ConfigureTargetProcess(static_cast<pid_t>(parsed), argv[2]);
    if (!configured) {
        std::fprintf(stderr, "target configuration failed: %s\n", configured.message.c_str());
        return 2;
    }
    UEProber prober(argv[3]);
    prober.RunAutoDumpFlow();
    const auto snapshot = prober.GetSnapshot();
    std::printf("result=%d\nmessage=%s\nmodule=%s\ngeneration=%llu\noffsets=%zu\noutput=%s\n",
        static_cast<int>(snapshot.result.code), snapshot.result.message.c_str(), snapshot.moduleIdentity.c_str(),
        static_cast<unsigned long long>(snapshot.generation), snapshot.offsets.size(), prober.GetDumpOutputDir().c_str());
    for (const auto& [name, offset] : snapshot.offsets) {
        if (!offset.value) continue;
        std::printf("offset %s=%u origin=%d validation=%d version=%llu\n", name.c_str(), *offset.value,
            static_cast<int>(offset.origin), static_cast<int>(offset.validation),
            static_cast<unsigned long long>(offset.version));
    }
    return prober.GetDumpStatus() == EDumpStatus::Success ? 0 : 1;
}

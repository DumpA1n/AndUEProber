#include "CrashHandler.h"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <format>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "BackTracer/BackTracer.h"
#include "FileLogger.h"

namespace CrashHandler {

// ── Signal table ─────────────────────────────────────────────────────────────

struct SignalEntry {
    int              signo;
    const char*      name;
    struct sigaction saved;
};

static SignalEntry g_signals[] = {
    { SIGSEGV, "SIGSEGV (Segmentation Fault)",    {} },
    { SIGABRT, "SIGABRT (Aborted)",               {} },
    { SIGBUS,  "SIGBUS  (Bus Error)",             {} },
    { SIGFPE,  "SIGFPE  (Floating Point Error)",  {} },
    { SIGILL,  "SIGILL  (Illegal Instruction)",   {} },
    { SIGTRAP, "SIGTRAP (Trace/Breakpoint Trap)", {} },
};

// Alternate stack so we can handle SIGSEGV caused by stack overflow
static constexpr size_t kAltStackSize = 64 * 1024; // 64 KiB
static uint8_t          g_alt_stack[kAltStackSize];

static bool g_installed = false;

// ── Helpers ───────────────────────────────────────────────────────────────────

static constexpr uintptr_t StripPAC(uintptr_t addr) { return addr & (uintptr_t)0x7fffffffff; }

static constexpr bool IsValidAddr(uintptr_t addr)
{
    return addr >= (uintptr_t)0x10000000 && addr <= (uintptr_t)0x7fffffffff && (addr & 0x3) == 0;
}

/** Safe 8-byte read: returns false if the page is not readable. */
static bool SafeRead8(uintptr_t addr, uintptr_t& out)
{
    if (!IsValidAddr(addr)) return false;
    // msync on the page is the lightest "is this mapped?" probe available
    // without installing another signal handler.  PAGE_SIZE is always 4096
    // on Android aarch64.
    if (msync(reinterpret_cast<void*>(addr & ~0xFFFu), 0x1000, MS_ASYNC) != 0)
        return false;
    out = StripPAC(*reinterpret_cast<const uintptr_t*>(addr));
    return true;
}

// ── Core handler ─────────────────────────────────────────────────────────────

static void OnCrash(int signo, siginfo_t* info, void* ctx)
{
    // Find signal table entry
    const char*      sig_name   = "UNKNOWN";
    struct sigaction old_action = {};
    bool             found      = false;

    for (auto& e : g_signals) {
        if (e.signo == signo) {
            sig_name   = e.name;
            old_action = e.saved;
            found      = true;
            break;
        }
    }

    FileIO* log = GetLogFile("CrashLog");

    // ── Header ──────────────────────────────────────────────────────────────
    log->Append("================================================================\n");
    log->Append(std::format("{}  *** CRASH: {} (signal {:d}) ***\n",
        __DATE__ " " __TIME__, sig_name, signo));

    if (info) {
        log->Append(std::format("    Fault address : 0x{:016X}\n",
            reinterpret_cast<uintptr_t>(info->si_addr)));
        log->Append(std::format("    si_code       : {:d}\n", info->si_code));
        log->Append(std::format("    pid           : {:d}  tid: {:d}\n",
            static_cast<int>(info->si_pid), static_cast<int>(gettid())));
    }

    // ── Registers ───────────────────────────────────────────────────────────
    if (ctx) {
        auto* uc = reinterpret_cast<ucontext_t*>(ctx);
        auto& mc = uc->uc_mcontext;

        log->Append("\n    --- Registers ---\n");
        for (int i = 0; i <= 28; ++i)
            log->Append(std::format("    x{:<2}  = 0x{:016X}\n", i,
                static_cast<uintptr_t>(mc.regs[i])));

        log->Append(std::format("    x29  = 0x{:016X}  (fp)\n",
            static_cast<uintptr_t>(mc.regs[29])));
        log->Append(std::format("    x30  = 0x{:016X}  (lr)\n",
            static_cast<uintptr_t>(mc.regs[30])));
        log->Append(std::format("    sp   = 0x{:016X}\n",
            static_cast<uintptr_t>(mc.sp)));
        log->Append(std::format("    pc   = 0x{:016X}\n",
            static_cast<uintptr_t>(mc.pc)));
        log->Append(std::format("    pstate = 0x{:016X}\n",
            static_cast<uintptr_t>(mc.pstate)));

        // Decode PC into module + offset
        Dl_info dl_pc{};
        if (dladdr(reinterpret_cast<const void*>(mc.pc), &dl_pc) && dl_pc.dli_fbase) {
            log->Append(std::format("    pc (in module)  = 0x{:010X}  {}\n",
                static_cast<uintptr_t>(mc.pc) - reinterpret_cast<uintptr_t>(dl_pc.dli_fbase),
                dl_pc.dli_fname ? dl_pc.dli_fname : "UNKNOWN"));
        }

        // ── Stack trace from the saved frame pointer ─────────────────────
        log->Append("\n    --- Stack Trace ---\n");

        uintptr_t fp      = StripPAC(static_cast<uintptr_t>(mc.regs[29]));
        int32_t   depth   = 0;

        while (depth < 128) {
            if (!IsValidAddr(fp)) break;

            uintptr_t lr = 0;
            uintptr_t next_fp = 0;

            if (!SafeRead8(fp + 0x8, lr)) break;
            if (!SafeRead8(fp, next_fp)) break;
            if (!IsValidAddr(lr)) break;

            Dl_info frame{};
            dladdr(reinterpret_cast<const void*>(lr), &frame);

            if (frame.dli_fbase) {
                log->Append(std::format(
                    "    #{:02d}  pc 0x{:010X}  {}  {}\n",
                    depth,
                    lr - reinterpret_cast<uintptr_t>(frame.dli_fbase),
                    frame.dli_fname
                        ? BackTracer::Trim(frame.dli_fname)
                        : "UNKNOWN",
                    frame.dli_sname ? frame.dli_sname : ""));
            } else {
                log->Append(std::format(
                    "    #{:02d}  pc 0x{:016X}  (no symbol info)\n",
                    depth, lr));
            }

            fp = next_fp;
            ++depth;
        }
    }

    log->Append("================================================================\n\n");

    // ── Re-raise with the original handler ───────────────────────────────────
    if (found) {
        sigaction(signo, &old_action, nullptr);
    } else {
        signal(signo, SIG_DFL);
    }
    raise(signo);
}

// ── Public API ────────────────────────────────────────────────────────────────

void Install()
{
    if (g_installed) return;
    g_installed = true;

    // Set up alternate signal stack so we survive stack-overflow crashes
    stack_t ss{};
    ss.ss_sp    = g_alt_stack;
    ss.ss_size  = kAltStackSize;
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_sigaction = OnCrash;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    for (auto& e : g_signals)
        sigaction(e.signo, &sa, &e.saved);
}

void Uninstall()
{
    if (!g_installed) return;
    g_installed = false;

    for (auto& e : g_signals)
        sigaction(e.signo, &e.saved, nullptr);

    // Remove alternate stack
    stack_t ss{};
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, nullptr);
}

} // namespace CrashHandler

#pragma once

namespace CrashHandler {

    /**
     * Install signal handlers for SIGSEGV / SIGABRT / SIGBUS / SIGFPE / SIGILL / SIGTRAP.
     * On crash the handler dumps registers + stack trace to
     * <external-storage>/Android/data/<pkg>/cache/<proj>/CrashLog/<date>.log
     * then re-raises the signal with the previously installed handler.
     *
     * Call once at startup, before any worker threads are spawned.
     */
    void Install();

    /** Restore previously saved signal disposition for all watched signals. */
    void Uninstall();

} // namespace CrashHandler

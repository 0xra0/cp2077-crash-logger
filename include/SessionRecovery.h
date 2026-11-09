#pragma once

// -----------------------------------------------------------------------------
// SessionRecovery — next-launch evidence for crashes that cannot be caught in the
// dying process (raw __fastfail/int 29h, driver reset, SIGKILL, Wine termination,
// power loss, or a crash before the rich writer completes).
//
// Each run owns a tiny <pid>.active marker. Clean shutdown removes it. On the next
// run, stale markers are correlated with CDPR's native
// %LOCALAPPDATA%\REDEngine\ReportQueue and a self-contained postmortem is made.
// -----------------------------------------------------------------------------

namespace SessionRecovery
{
    // Analyze stale markers from previous runs and arm the marker for this run.
    // Deferred-init thread only; performs filesystem I/O.
    void Init();

    // Allocation-free state bit set as soon as CrashHandler commits to a crash.
    void MarkAbnormal() noexcept;

    // Called from known clean process-exit hooks. Best-effort.
    void MarkCleanExit() noexcept;

    // Loader-lock-safe shutdown fallback. DLL_PROCESS_DETACH is also reached when
    // WinMain simply returns, a path that need not import ExitProcess at all.
    // The marker is removed only when MarkAbnormal() was never set.
    void OnProcessDetach() noexcept;
}

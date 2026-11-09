#pragma once

// -----------------------------------------------------------------------------
// HangDetector — reports a freeze the way CrashHandler reports a crash.
//
// A crash is an event the OS hands you. A freeze is the absence of events, so it
// has to be gone looking for. The signal used here is the one Windows itself uses
// to decide an application is "Not Responding": send the main window a WM_NULL
// with SMTO_ABORTIFHUNG and see whether the message pump answers. A game inside a
// 37-second frame is not pumping messages, so it does not answer.
//
// That choice matters. The obvious alternative — watching CPU time — cannot work
// here, because during these freezes the process is *busier* than normal: the
// probe measured ~3.8 cores of load while the frame time was 37 s. "Stalled" and
// "spinning" look identical from CPU counters alone, and this game does the latter.
//
// On a confirmed hang the main thread is suspended and its stack walked three
// times about a second apart. One sample says where it is; three say whether it is
// stuck on one instruction or looping through a range — which is the difference
// between a deadlock and a runaway loop, and points at different culprits.
// -----------------------------------------------------------------------------

namespace HangDetector
{
    // Start the watchdog thread. Call from deferred init, once the window exists.
    // Never throws; if no game window is ever found it logs that and gives up.
    void Start();

    // Seconds of an unresponsive message pump before a hang is declared.
    // Chosen well above a normal loading-screen stall.
    inline constexpr int kHangSeconds = 6;
}

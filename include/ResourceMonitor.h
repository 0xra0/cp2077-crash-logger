#pragma once

// -----------------------------------------------------------------------------
// ResourceMonitor — records resource pressure *before* a crash.
//
// Sampling memory only after an allocation failure can be too late: the system may
// already have reclaimed memory or the rich crash writer itself may be unable to
// allocate.  A tiny watchdog therefore keeps atomics with the last/peak/minimum
// values and emits durable breadcrumbs when commit, RAM, handles or disk space
// enter dangerous territory.
// -----------------------------------------------------------------------------

namespace ResourceMonitor
{
    void Start();

    // Pre-render the latest/peak resource state from atomic samples.  No filesystem
    // access and no process enumeration happens here; suitable for crash reports.
    std::string Report();
}

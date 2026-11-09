#pragma once

#include "StackWalker.h"

// -----------------------------------------------------------------------------
// ThreadSnapshot — raw unwind snapshots of the *other* threads in the process.
//
// A crashing thread tells us where the fault surfaced, but deadlocks, worker-pool
// failures and cross-thread lifetime bugs often leave the useful evidence on a
// different thread.  The capture path suspends one thread at a time only long
// enough to copy CONTEXT + unwind addresses, resumes it immediately, and performs
// every allocation afterwards.  That avoids the classic reporter deadlock where a
// suspended victim owns the CRT heap or dbghelp lock.
// -----------------------------------------------------------------------------

struct ThreadStackSnapshot
{
    std::uint32_t              threadId{};
    std::string                description;
    std::vector<std::uint64_t> addresses;
};

struct ThreadSnapshotResult
{
    std::vector<ThreadStackSnapshot> threads;
    std::size_t                      candidates{};
    std::size_t                      failed{};
    bool                             truncated{};
};

namespace ThreadSnapshot
{
    // Capture all process threads except `excludedThreadId` and the caller.  Each
    // thread is resumed before any vector/string work is performed.
    ThreadSnapshotResult Capture(std::uint32_t excludedThreadId,
                                 std::size_t maxThreads = 48,
                                 std::size_t maxFrames = 24);
}

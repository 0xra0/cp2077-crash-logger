#include "PCH.h"
#include "ThreadSnapshot.h"
#include "ThreadInfo.h"

#include <TlHelp32.h>

namespace ThreadSnapshot
{
    ThreadSnapshotResult Capture(std::uint32_t excludedThreadId,
                                 std::size_t maxThreads,
                                 std::size_t maxFrames)
    {
        ThreadSnapshotResult result;
        if (maxThreads == 0 || maxFrames == 0)
            return result;

        constexpr std::size_t kHardMaxFrames = 64;
        maxFrames = (std::min)(maxFrames, kHardMaxFrames);

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return result;

        const DWORD pid        = GetCurrentProcessId();
        const DWORD currentTid = GetCurrentThreadId();

        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (!Thread32First(snap, &te)) {
            CloseHandle(snap);
            return result;
        }

        do {
            if (te.th32OwnerProcessID != pid)
                continue;
            if (te.th32ThreadID == excludedThreadId || te.th32ThreadID == currentTid)
                continue;

            ++result.candidates;
            if (result.threads.size() >= maxThreads) {
                result.truncated = true;
                continue;
            }

            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                       THREAD_QUERY_LIMITED_INFORMATION,
                                       FALSE, te.th32ThreadID);
            if (!thread) {
                ++result.failed;
                continue;
            }

            std::array<std::uint64_t, kHardMaxFrames> raw{};
            std::size_t count = 0;
            bool captured = false;

            if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                if (GetThreadContext(thread, &ctx)) {
                    count = StackWalker::Unwind(&ctx, raw.data(), maxFrames);
                    captured = count != 0;
                }
                ResumeThread(thread); // resume before allocations / strings / logging
            }
            CloseHandle(thread);

            if (!captured) {
                ++result.failed;
                continue;
            }

            ThreadStackSnapshot ts;
            ts.threadId    = te.th32ThreadID;
            ts.description = ThreadInfo::Describe(te.th32ThreadID);
            ts.addresses.assign(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(count));
            result.threads.push_back(std::move(ts));
        } while (Thread32Next(snap, &te));

        CloseHandle(snap);
        return result;
    }
}

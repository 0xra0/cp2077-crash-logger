#include "PCH.h"
#include "ResourceMonitor.h"
#include "Breadcrumbs.h"
#include "LogWriter.h"

namespace ResourceMonitor
{
    namespace
    {
        constexpr DWORD         kSampleMs        = 5000;
        constexpr std::uint64_t kGiB             = 1024ull * 1024ull * 1024ull;
        constexpr std::uint64_t kLowDiskBytes    = 2ull * kGiB;
        constexpr DWORD         kHighHandleCount = 20000;
        constexpr std::uint64_t kNoMinimum       = ~std::uint64_t{0};

        std::atomic_bool          s_started{ false };
        std::atomic<std::uint64_t> s_physAvail{ 0 }, s_physTotal{ 0 };
        std::atomic<std::uint64_t> s_commitAvail{ 0 }, s_commitLimit{ 0 };
        std::atomic<std::uint64_t> s_privateBytes{ 0 }, s_workingSet{ 0 };
        std::atomic<std::uint64_t> s_peakPrivate{ 0 };
        std::atomic<std::uint64_t> s_minPhysAvail{ kNoMinimum }, s_minCommitAvail{ kNoMinimum };
        std::atomic<std::uint64_t> s_diskFree{ 0 };
        std::atomic<DWORD>         s_handles{ 0 }, s_peakHandles{ 0 };
        std::atomic<std::uint64_t> s_samples{ 0 };

        std::atomic<int>  s_commitWarnLevel{ 0 };
        std::atomic_bool  s_physWarned{ false };
        std::atomic_bool  s_handleWarned{ false };
        std::atomic_bool  s_diskWarned{ false };

        void AtomicMax(std::atomic<std::uint64_t>& target, std::uint64_t value)
        {
            auto cur = target.load(std::memory_order_relaxed);
            while (value > cur && !target.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {}
        }

        void AtomicMin(std::atomic<std::uint64_t>& target, std::uint64_t value)
        {
            auto cur = target.load(std::memory_order_relaxed);
            while (value < cur && !target.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {}
        }

        void AtomicMax(std::atomic<DWORD>& target, DWORD value)
        {
            auto cur = target.load(std::memory_order_relaxed);
            while (value > cur && !target.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {}
        }

        unsigned Percent(std::uint64_t used, std::uint64_t total)
        {
            return total ? static_cast<unsigned>((used * 100 + total / 2) / total) : 0;
        }

        std::string Bytes(std::uint64_t bytes)
        {
            constexpr double MiB = 1024.0 * 1024.0;
            constexpr double GiB = MiB * 1024.0;
            if (bytes >= kGiB)
                return std::format("{:.1f} GiB", static_cast<double>(bytes) / GiB);
            return std::format("{:.1f} MiB", static_cast<double>(bytes) / MiB);
        }

        void Sample()
        {
            MEMORYSTATUSEX ms{};
            ms.dwLength = sizeof(ms);
            if (GlobalMemoryStatusEx(&ms)) {
                const auto commitAvail = ms.ullAvailPageFile;
                s_physAvail.store(ms.ullAvailPhys, std::memory_order_relaxed);
                s_physTotal.store(ms.ullTotalPhys, std::memory_order_relaxed);
                s_commitAvail.store(commitAvail, std::memory_order_relaxed);
                s_commitLimit.store(ms.ullTotalPageFile, std::memory_order_relaxed);
                AtomicMin(s_minPhysAvail, ms.ullAvailPhys);
                AtomicMin(s_minCommitAvail, commitAvail);

                const auto commitUsed = ms.ullTotalPageFile - commitAvail;
                const auto commitPct  = Percent(commitUsed, ms.ullTotalPageFile);
                const int level       = commitPct >= 97 ? 2 : commitPct >= 90 ? 1 : 0;
                int old = s_commitWarnLevel.load(std::memory_order_relaxed);
                if (level > old && s_commitWarnLevel.compare_exchange_strong(old, level)) {
                    Breadcrumbs::Log("RESOURCE %s: system commit %u%% used, %llu MiB available",
                        level == 2 ? "CRITICAL" : "warning", commitPct,
                        static_cast<unsigned long long>(commitAvail / (1024ull * 1024ull)));
                }

                const bool lowPhys = ms.ullAvailPhys < kGiB ||
                    (ms.ullTotalPhys && ms.ullAvailPhys * 20 < ms.ullTotalPhys); // under 5%
                if (lowPhys && !s_physWarned.exchange(true))
                    Breadcrumbs::Log("RESOURCE warning: low physical memory, %llu MiB available",
                        static_cast<unsigned long long>(ms.ullAvailPhys / (1024ull * 1024ull)));
            }

            PROCESS_MEMORY_COUNTERS_EX pmc{};
            pmc.cb = sizeof(pmc);
            if (GetProcessMemoryInfo(GetCurrentProcess(),
                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
                const std::uint64_t priv = pmc.PrivateUsage ? pmc.PrivateUsage : pmc.PagefileUsage;
                s_privateBytes.store(priv, std::memory_order_relaxed);
                s_workingSet.store(pmc.WorkingSetSize, std::memory_order_relaxed);
                AtomicMax(s_peakPrivate, priv);
            }

            DWORD handles = 0;
            if (GetProcessHandleCount(GetCurrentProcess(), &handles)) {
                s_handles.store(handles, std::memory_order_relaxed);
                AtomicMax(s_peakHandles, handles);
                if (handles >= kHighHandleCount && !s_handleWarned.exchange(true))
                    Breadcrumbs::Log("RESOURCE warning: unusually high handle count: %lu",
                        static_cast<unsigned long>(handles));
            }

            ULARGE_INTEGER freeBytes{};
            const auto dir = LogWriter::GetLogDir();
            if (!dir.empty() && GetDiskFreeSpaceExW(dir.root_path().wstring().c_str(),
                    &freeBytes, nullptr, nullptr)) {
                s_diskFree.store(freeBytes.QuadPart, std::memory_order_relaxed);
                if (freeBytes.QuadPart < kLowDiskBytes && !s_diskWarned.exchange(true))
                    Breadcrumbs::Log("RESOURCE warning: low free disk space on crash-log volume: %llu MiB",
                        static_cast<unsigned long long>(freeBytes.QuadPart / (1024ull * 1024ull)));
            }

            s_samples.fetch_add(1, std::memory_order_relaxed);
        }

        DWORD WINAPI MonitorThread(LPVOID)
        {
            for (;;) {
                Sample();
                Sleep(kSampleMs);
            }
        }
    }

    void Start()
    {
        if (s_started.exchange(true))
            return;

        Sample();
        if (HANDLE h = CreateThread(nullptr, 0, MonitorThread, nullptr, 0, nullptr)) {
            CloseHandle(h);
            REX::INFO("ResourceMonitor: sampling every {} ms.", kSampleMs);
        } else {
            REX::WARN("ResourceMonitor: watchdog thread could not be created; one startup sample retained.");
        }
    }

    std::string Report()
    {
        const auto samples = s_samples.load(std::memory_order_relaxed);
        if (!samples)
            return "  (resource watchdog not started)\n";

        const auto physAvail   = s_physAvail.load(std::memory_order_relaxed);
        const auto physTotal   = s_physTotal.load(std::memory_order_relaxed);
        const auto commitAvail = s_commitAvail.load(std::memory_order_relaxed);
        const auto commitLimit = s_commitLimit.load(std::memory_order_relaxed);
        const auto privateNow  = s_privateBytes.load(std::memory_order_relaxed);
        const auto workingSet  = s_workingSet.load(std::memory_order_relaxed);
        const auto peakPrivate = s_peakPrivate.load(std::memory_order_relaxed);
        auto minPhys           = s_minPhysAvail.load(std::memory_order_relaxed);
        auto minCommit         = s_minCommitAvail.load(std::memory_order_relaxed);
        const auto diskFree    = s_diskFree.load(std::memory_order_relaxed);
        const auto handles     = s_handles.load(std::memory_order_relaxed);
        const auto peakHandles = s_peakHandles.load(std::memory_order_relaxed);

        if (minPhys == kNoMinimum) minPhys = 0;
        if (minCommit == kNoMinimum) minCommit = 0;

        const auto commitUsed = commitLimit > commitAvail ? commitLimit - commitAvail : 0;
        std::string out;
        out += std::format("  samples: {} (5s cadence)\n", samples);
        out += std::format("  RAM now: {} free / {} total; minimum observed free: {}\n",
            Bytes(physAvail), Bytes(physTotal), Bytes(minPhys));
        out += std::format("  Commit:  {} used / {} limit ({}%); minimum observed free: {}\n",
            Bytes(commitUsed), Bytes(commitLimit), Percent(commitUsed, commitLimit), Bytes(minCommit));
        out += std::format("  Process: private {} now / {} peak; working set {}\n",
            Bytes(privateNow), Bytes(peakPrivate), Bytes(workingSet));
        out += std::format("  Handles: {} now / {} peak\n", handles, peakHandles);
        if (diskFree)
            out += std::format("  Crash-log volume free: {}\n", Bytes(diskFree));
        return out;
    }
}

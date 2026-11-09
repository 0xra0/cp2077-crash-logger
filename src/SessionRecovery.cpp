#include "PCH.h"
#include "SessionRecovery.h"
#include "LogWriter.h"

#include <cctype>
#include <cstdlib>

namespace SessionRecovery
{
    namespace
    {
        constexpr std::uint64_t kTicksPerSecond = 10'000'000ull;
        constexpr std::uint64_t kCorrelationSlack = 120ull * kTicksPerSecond;
        constexpr std::uintmax_t kTextCap = 256 * 1024;

        std::filesystem::path s_currentMarker;
        std::atomic_bool       s_abnormal{ false };

        std::uint64_t FileTimeNow()
        {
            FILETIME ft{};
            GetSystemTimeAsFileTime(&ft);
            ULARGE_INTEGER u{};
            u.LowPart = ft.dwLowDateTime;
            u.HighPart = ft.dwHighDateTime;
            return u.QuadPart;
        }

        std::uint64_t PathWriteTime(const std::filesystem::path& p)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &data))
                return 0;
            ULARGE_INTEGER u{};
            u.LowPart = data.ftLastWriteTime.dwLowDateTime;
            u.HighPart = data.ftLastWriteTime.dwHighDateTime;
            return u.QuadPart;
        }

        bool ProcessAlive(DWORD pid)
        {
            // Init() runs before this process creates its own marker, so a marker
            // carrying our freshly-reused PID is necessarily stale.
            if (!pid || pid == GetCurrentProcessId())
                return false;
            HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!h)
                return false;
            const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
            CloseHandle(h);
            return alive;
        }

        struct Marker
        {
            DWORD         pid{};
            std::uint64_t started{};
            std::string   display;
        };

        Marker ReadMarker(const std::filesystem::path& p)
        {
            Marker m;
            std::ifstream in(p);
            std::string line;
            while (std::getline(in, line)) {
                if (line.starts_with("pid="))
                    m.pid = static_cast<DWORD>(std::strtoul(line.c_str() + 4, nullptr, 10));
                else if (line.starts_with("started_filetime="))
                    m.started = std::strtoull(line.c_str() + 17, nullptr, 10);
                else if (line.starts_with("started_display="))
                    m.display = line.substr(16);
            }
            return m;
        }

        std::filesystem::path ReportQueue()
        {
            wchar_t buf[32768]{};
            const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(std::size(buf)));
            if (!n || n >= std::size(buf))
                return {};
            return std::filesystem::path(buf) / "REDEngine" / "ReportQueue";
        }

        std::filesystem::path NewestReportFolder(std::uint64_t after)
        {
            const auto root = ReportQueue();
            std::error_code ec;
            if (root.empty() || !std::filesystem::is_directory(root, ec))
                return {};

            std::filesystem::path best;
            std::uint64_t bestTime = 0;
            for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
                if (!e.is_directory(ec))
                    continue;
                const auto t = PathWriteTime(e.path());
                if (t >= after && t > bestTime) {
                    best = e.path();
                    bestTime = t;
                }
            }
            return best;
        }

        std::string ReadTextCapped(const std::filesystem::path& p)
        {
            std::ifstream in(p, std::ios::binary);
            if (!in)
                return {};
            std::string s;
            s.resize(static_cast<std::size_t>((std::min<std::uintmax_t>)(
                std::filesystem::file_size(p), kTextCap)));
            in.read(s.data(), static_cast<std::streamsize>(s.size()));
            s.resize(static_cast<std::size_t>(in.gcount()));
            if (in.peek() != EOF)
                s += "\n... [truncated by CrashLogger] ...\n";
            return s;
        }

        std::filesystem::path FindCaseInsensitive(const std::filesystem::path& dir, std::string wanted)
        {
            std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (!e.is_regular_file(ec))
                    continue;
                auto n = e.path().filename().string();
                std::transform(n.begin(), n.end(), n.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (n == wanted)
                    return e.path();
            }
            return {};
        }

        void WritePostmortem(const std::filesystem::path& markerPath, const Marker& marker)
        {
            const auto outPath = LogWriter::GetLogDir() /
                std::format("postmortem_{}_pid{}.log", LogWriter::MakeFileTimestamp(), marker.pid);
            std::ofstream out(outPath, std::ios::out | std::ios::trunc);
            if (!out)
                return;

            out << "Cyberpunk 2077 Crash Logger — PREVIOUS SESSION POSTMORTEM\n";
            out << std::format("Previous PID: {}\n", marker.pid);
            out << std::format("Previous start: {}\n", marker.display.empty() ? "unknown" : marker.display);
            out << "Detection: the session marker survived but that process is no longer running.\n"
                   "This means the previous run did not reach a clean ExitProcess/TerminateProcess(0).\n"
                   "It covers hard kills and crash paths that can bypass an in-process exception handler.\n\n";

            const std::uint64_t after = marker.started > kCorrelationSlack
                ? marker.started - kCorrelationSlack : 0;
            const auto reportDir = NewestReportFolder(after);
            out << "REDENGINE REPORT QUEUE\n";
            out << std::format("  root: {}\n", ReportQueue().empty() ? "(LOCALAPPDATA unavailable)" : ReportQueue().string());

            if (reportDir.empty()) {
                out << "  No report folder newer than the previous session start was found.\n"
                       "  This is consistent with a hard kill, driver/process termination, raw fail-fast,\n"
                       "  or a crash occurring before REDEngineErrorReporter produced its artifacts.\n\n";
            } else {
                out << std::format("  correlated folder: {}\n", reportDir.string());
                out << "  files:\n";
                std::error_code ec;
                for (const auto& e : std::filesystem::directory_iterator(reportDir, ec)) {
                    if (!e.is_regular_file(ec))
                        continue;
                    const auto size = e.file_size(ec);
                    out << std::format("    - {} ({} bytes)\n", e.path().filename().string(), ec ? 0 : size);
                }
                out << "\n";

                const auto stack = FindCaseInsensitive(reportDir, "stacktrace.txt");
                if (!stack.empty()) {
                    out << "--- REDEngine stacktrace.txt ---\n";
                    out << ReadTextCapped(stack) << "\n--- end stacktrace.txt ---\n\n";
                }

                const auto crashInfo = FindCaseInsensitive(reportDir, "CrashInfo.json");
                if (!crashInfo.empty()) {
                    out << "--- REDEngine CrashInfo.json ---\n";
                    out << ReadTextCapped(crashInfo) << "\n--- end CrashInfo.json ---\n\n";
                }
            }

            out << "TRACE POINTER\n"
                   "  Also inspect CrashLogger_trace.log for the final first-chance exception/resource\n"
                   "  breadcrumbs from the previous process.\n\n"
                   "-- end of previous-session postmortem --\n";
            out.flush();

            std::error_code ec;
            std::filesystem::remove(markerPath, ec);
            REX::INFO("SessionRecovery: previous abnormal session -> {}", outPath.string());
        }

        void CreateCurrentMarker(const std::filesystem::path& sessions)
        {
            const DWORD pid = GetCurrentProcessId();
            s_currentMarker = sessions / std::format("{}.active", pid);
            std::ofstream out(s_currentMarker, std::ios::out | std::ios::trunc);
            if (!out) {
                s_currentMarker.clear();
                return;
            }
            out << "version=1\n";
            out << std::format("pid={}\n", pid);
            out << std::format("started_filetime={}\n", FileTimeNow());
            out << std::format("started_display={}\n", LogWriter::MakeDisplayTimestamp());
            out.flush();
        }
    }

    void Init()
    {
        try {
            const auto sessions = LogWriter::GetLogDir() / "sessions";
            std::error_code ec;
            std::filesystem::create_directories(sessions, ec);

            for (const auto& e : std::filesystem::directory_iterator(sessions, ec)) {
                if (!e.is_regular_file(ec) || e.path().extension() != ".active")
                    continue;
                const Marker marker = ReadMarker(e.path());
                if (!marker.pid || ProcessAlive(marker.pid))
                    continue;
                try {
                    WritePostmortem(e.path(), marker);
                } catch (const std::exception& ex) {
                    REX::WARN("SessionRecovery: stale-session analysis failed for {}: {}",
                        e.path().string(), ex.what());
                }
            }

            CreateCurrentMarker(sessions);
            if (!s_currentMarker.empty())
                REX::INFO("SessionRecovery: armed {}", s_currentMarker.string());
        } catch (const std::exception& e) {
            REX::WARN("SessionRecovery: init failed: {}", e.what());
        }
    }

    void MarkAbnormal() noexcept
    {
        s_abnormal.store(true, std::memory_order_relaxed);
    }

    void MarkCleanExit() noexcept
    {
        if (s_abnormal.load(std::memory_order_relaxed))
            return;
        try {
            if (s_currentMarker.empty())
                return;
            std::error_code ec;
            std::filesystem::remove(s_currentMarker, ec);
            s_currentMarker.clear();
        } catch (...) {
            // Process is exiting; the marker remaining merely yields a conservative
            // postmortem on next launch, which is preferable to risking shutdown.
        }
    }

    void OnProcessDetach() noexcept
    {
        if (s_abnormal.load(std::memory_order_relaxed) || s_currentMarker.empty())
            return;
        // Raw Win32 only: DllMain owns the loader lock here. No filesystem traversal,
        // formatting or allocation. A failed deletion is harmless and will be
        // reported conservatively as an unclean session next launch.
        DeleteFileW(s_currentMarker.c_str());
    }
}

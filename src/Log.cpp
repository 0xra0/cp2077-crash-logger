#include "PCH.h"

namespace REX
{
    namespace
    {
        std::filesystem::path g_selfDir;
        std::filesystem::path g_logPath;
        std::mutex            g_mtx;
    }

    void InitLog(void* selfModule) noexcept
    {
        try {
            wchar_t buf[MAX_PATH]{};
            if (GetModuleFileNameW(static_cast<HMODULE>(selfModule), buf, MAX_PATH)) {
                g_selfDir = std::filesystem::path(buf).parent_path();
                g_logPath = g_selfDir / L"CrashLogger.log";
            }
        } catch (const std::exception&) {}
    }

    const std::filesystem::path& SelfDir() noexcept { return g_selfDir; }

    namespace detail
    {
        void Write(std::string_view level, std::string_view msg) noexcept
        {
            // Always mirror to the debugger/Wine console — this survives even when
            // the filesystem write fails, and shows up in PROTON_LOG output.
            try {
                const auto dbg = std::format("[CrashLogger] [{}] {}\n", level, msg);
                OutputDebugStringA(dbg.c_str());
            } catch (...) {}

            if (g_logPath.empty())
                return;

            try {
                std::lock_guard lock(g_mtx);
                std::ofstream out(g_logPath, std::ios::app);
                if (!out)
                    return;
                // Local time, not UTC: this log only earns its keep when it can be
                // lined up against the game's own logs, which are all local.
                //
                // Built from localtime_s rather than std::chrono::current_zone().
                // current_zone() needs the timezone database and THROWS when it cannot
                // be loaded — which is exactly what happens under Wine with the static
                // CRT. The catch below then swallowed it, so every INFO/WARN/ERROR this
                // logger ever emitted was silently discarded, including the errors
                // reporting that other components had failed. A logger that can lose
                // its own failure reports is worse than no logger.
                const auto time = std::chrono::system_clock::to_time_t(
                    std::chrono::system_clock::now());
                std::tm tm{};
                localtime_s(&tm, &time);
                out << std::format("[{:04}-{:02}-{:02} {:02}:{:02}:{:02}] [{}] {}\n",
                    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                    tm.tm_hour, tm.tm_min, tm.tm_sec, level, msg);
            } catch (const std::exception&) {}
        }
    }
}

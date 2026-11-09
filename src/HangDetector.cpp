#include "PCH.h"
#include "HangDetector.h"
#include "Breadcrumbs.h"
#include "Frameworks.h"
#include "LoadOrder.h"
#include "LogWriter.h"
#include "Modules.h"
#include "ResourceMonitor.h"
#include "StackWalker.h"
#include "ThreadInfo.h"
#include "ThreadSnapshot.h"

namespace HangDetector
{
    namespace
    {
        constexpr int kMaxReports   = 6;    // never run away, however bad the session
        constexpr int kSamples      = 3;    // stack samples per hang
        constexpr int kSampleGapMs  = 900;
        constexpr int kCooldownSecs = 30;   // after recovery, before arming again
        constexpr int kStallSeconds = 10;   // CET heartbeat older than this = frame loop stuck

        std::atomic<int> s_reports{ 0 };

        // ---- finding the game window ---------------------------------------
        struct FindCtx { DWORD pid; HWND best; };

        BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lp)
        {
            auto* ctx = reinterpret_cast<FindCtx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != ctx->pid || !IsWindowVisible(hwnd))
                return TRUE;
            RECT r{};
            if (!GetWindowRect(hwnd, &r))
                return TRUE;
            // The render window is the big one; tool-tips and IME helpers are not.
            if ((r.right - r.left) < 320 || (r.bottom - r.top) < 240)
                return TRUE;
            ctx->best = hwnd;
            return FALSE;
        }

        HWND FindGameWindow()
        {
            FindCtx ctx{ GetCurrentProcessId(), nullptr };
            EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
            return ctx.best;
        }

        // ---- is the pump answering? ----------------------------------------
        bool PumpResponds(HWND hwnd, UINT timeoutMs)
        {
            DWORD_PTR result = 0;
            // SMTO_ABORTIFHUNG returns immediately if the window is already known
            // hung; the timeout covers the case where it is only just going under.
            return SendMessageTimeoutW(hwnd, WM_NULL, 0, 0,
                                       SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                       timeoutMs, &result) != 0;
        }

        // ---- one stack sample of the main thread ---------------------------
        //
        // Strictly two phases. While the target is suspended this does nothing but
        // Unwind(), which is documented to take no lock and allocate nothing.
        // Calling the allocating, dbghelp-using Walk() here instead would risk a
        // deadlock: if the suspended thread happened to hold the CRT heap lock or
        // dbghelp's, we would block forever waiting for a thread only we can
        // resume — a freeze reporter that freezes the game. Symbolication happens
        // after ResumeThread, off the critical window.
        constexpr std::size_t kMaxFrames = 64;

        std::vector<StackFrame> SampleMainThread(DWORD tid)
        {
            std::uint64_t addrs[kMaxFrames]{};
            std::size_t   count = 0;

            HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                                  FALSE, tid);
            if (!h)
                return {};

            if (SuspendThread(h) != static_cast<DWORD>(-1)) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(h, &ctx))
                    count = StackWalker::Unwind(&ctx, addrs, kMaxFrames);
                ResumeThread(h);   // always, even if the unwind produced nothing
            }
            CloseHandle(h);

            if (count == 0)
                return {};
            try {
                return StackWalker::Annotate(addrs, count);
            } catch (const std::exception&) {
                return {};
            }
        }

        // ---- CP2077-specific attribution -----------------------------------
        // Most mods here are not native code. .archive, redscript and CET Lua mods
        // can never appear as a module on a stack, so "no mod module on the stack"
        // must not be reported as "no mod involved" — it is the normal case even
        // when a mod is entirely at fault.
        std::string AttributeStack(const std::vector<StackFrame>& frames,
                                   const std::vector<ModuleInfo>& modules)
        {
            std::string out;
            std::vector<std::string> seen;
            bool cet = false;

            for (const auto& f : frames) {
                const auto* m = Modules::FindModule(f.address, modules);
                if (!m)
                    continue;
                auto lower = m->name;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.find("cyber_engine_tweaks") != std::string::npos)
                    cet = true;
                if (!m->isModPlugin)
                    continue;
                auto label = std::format("{} [{}]", m->name, m->kind);
                if (std::find(seen.begin(), seen.end(), label) == seen.end())
                    seen.push_back(std::move(label));
            }

            if (seen.empty()) {
                out += "  No mod DLL on the stack.\n";
            } else {
                out += "  Mod plugins on the stack (possible culprits):\n";
                for (const auto& s : seen)
                    out += std::format("    {}\n", s);
            }

            if (cet) {
                out += "  Cyber Engine Tweaks is on the stack: the freeze is inside CET's Lua VM.\n"
                       "    Which CET mod cannot be read from a native stack — LuaJIT compiles to\n"
                       "    anonymous machine code with no symbols. Use the per-mod Lua profile\n"
                       "    (luaprof-report.txt) captured alongside this report.\n";
            }
            out += "  NOTE: .archive, redscript and CET Lua mods are not DLLs and can never\n"
                   "        appear here. An empty list does not clear the mod list.\n";
            return out;
        }

        // Age of a file in seconds. file_time_type's own clock avoids clock_cast,
        // which is not what we want to be relying on inside a freeze reporter.
        std::int64_t FileAgeSeconds(const std::filesystem::path& p, std::error_code& ec)
        {
            const auto t = std::filesystem::last_write_time(p, ec);
            if (ec)
                return -1;
            return std::chrono::duration_cast<std::chrono::seconds>(
                       std::filesystem::file_time_type::clock::now() - t).count();
        }

        // The second hang signal. The message pump can go on answering WM_NULL while the
        // frame loop is stuck: two freezes on 2026-09-15 ran for minutes with this
        // watchdog armed and never tripped it. CrashBreadcrumb, a CET mod, writes and
        // flushes a line from onUpdate every second, menus included, so its file's mtime
        // is a frame heartbeat that needs no hook into the game. A save-load frame was
        // measured at 6.9 s, which is why kStallSeconds is 10 and not kHangSeconds.
        std::filesystem::path HeartbeatPath()
        {
            try {
                wchar_t exe[MAX_PATH]{};
                if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
                    return {};
                return std::filesystem::path(exe).parent_path() / "plugins" /
                    "cyber_engine_tweaks" / "mods" / "CrashBreadcrumb" / "breadcrumb.log";
            } catch (...) {
                return {};
            }
        }

        // Pull the "--- by mod" ranking out of a luaprof report.
        std::string ByModSection(const std::filesystem::path& p)
        {
            std::ifstream in(p);
            if (!in)
                return {};
            std::string line, out;
            bool inMods = false;
            int  taken = 0;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.find("--- by mod") != std::string::npos) { inMods = true; continue; }
                // The freeze report says so explicitly when Lua was not involved;
                // that is a finding, not an absence, so carry it through.
                if (line.find("no Lua samples during the freeze") != std::string::npos)
                    out += "  " + line + "\n";
                if (inMods) {
                    if (line.starts_with("---"))
                        break;
                    if (line.empty())
                        continue;
                    out += "  " + line + "\n";
                    if (++taken >= 12)
                        break;
                }
            }
            return out;
        }

        // If the CET Lua profiler mod is installed, fold its ranking into the report —
        // it is the only thing that can name a specific Lua mod.
        //
        // Two files, and picking the wrong one is worse than printing nothing:
        //   luaprof-freeze.txt  written FROM the profiler callback while frozen, and
        //                       scoped to the samples since the last frame.
        //   luaprof-report.txt  written from onUpdate — which DOES NOT RUN during a
        //                       freeze, so at hang time it is by definition stale and
        //                       describes the play that preceded the freeze.
        // Prefer the freeze file, and always print the age so a stale ranking can
        // never be read as a live one.
        std::string LuaProfileExcerpt()
        {
            try {
                wchar_t exe[MAX_PATH]{};
                if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
                    return {};
                const auto dir = std::filesystem::path(exe).parent_path() /
                    "plugins" / "cyber_engine_tweaks" / "mods" / "luaprof";

                std::error_code ec;
                const auto freeze = dir / "luaprof-freeze.txt";
                const auto age    = FileAgeSeconds(freeze, ec);
                if (!ec && age >= 0 && age <= 60) {
                    auto body = ByModSection(freeze);
                    if (!body.empty())
                        return std::format(
                            "  scope: THE FREEZE ITSELF (samples since the last frame, {}s old)\n{}",
                            age, body);
                }

                const auto session = dir / "luaprof-report.txt";
                std::error_code ec2;
                const auto sage = FileAgeSeconds(session, ec2);
                if (ec2 || sage < 0)
                    return {};
                auto body = ByModSection(session);
                if (body.empty())
                    return {};
                return std::format(
                    "  scope: WHOLE SESSION, WRITTEN {}s AGO — this predates the freeze and\n"
                    "         ranks the play before it. Do NOT read it as the cause.\n{}",
                    sage, body);
            } catch (const std::exception&) {
                return {};
            }
        }

        std::string RawModuleOffset(std::uint64_t address, const std::vector<ModuleInfo>& modules)
        {
            const auto* m = Modules::FindModule(address, modules);
            if (!m)
                return {};
            return std::format("{}+0x{:X}{}", m->name, address - m->base,
                m->isModPlugin ? std::format(" [{}]", m->kind) : "");
        }

        // ---- the report ------------------------------------------------------
        void WriteHangReport(const std::vector<std::vector<StackFrame>>& samples,
                             DWORD tid, int unresponsiveSecs, const char* why)
        {
            const auto dir = LogWriter::GetLogDir();
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const auto path = dir / std::format("hang_{}.log", LogWriter::MakeFileTimestamp());

            std::ofstream out(path);
            if (!out)
                return;

            const auto modules      = Modules::GetAll();
            const auto otherThreads = ThreadSnapshot::Capture(tid, 32, 16);

            out << "Cyberpunk 2077 Crash Logger — HANG REPORT\n";
            // localtime_s, not chrono::current_zone(): the latter needs the timezone
            // database and THROWS when it cannot be loaded, which is what happens under
            // Wine with the static CRT. It aborted the report on this very line — 45
            // bytes on disk, every field that mattered missing, and nothing anywhere
            // saying so. A reporter must not depend on something that can throw.
            out << std::format("Timestamp: {}\n", LogWriter::MakeDisplayTimestamp());
            out << std::format("Unresponsive for: {}s ({})\n", unresponsiveSecs, why);
            out << std::format("Thread: {}\n\n", ThreadInfo::Describe(tid));

            out << "WHAT THIS IS\n"
                   "  The process is alive and running; the game stopped making progress. This is a\n"
                   "  freeze, not a crash — nothing faulted, so there is no exception, no faulting\n"
                   "  address and no minidump. The evidence is where the main thread is, below.\n\n";

            // Did the stack move between samples? That is the deadlock/livelock split.
            bool identical = samples.size() > 1;
            for (std::size_t i = 1; i < samples.size() && identical; ++i) {
                if (samples[i].size() != samples[0].size()) { identical = false; break; }
                for (std::size_t f = 0; f < samples[i].size(); ++f)
                    if (samples[i][f].address != samples[0][f].address) { identical = false; break; }
            }

            out << "VERDICT\n";
            if (samples.empty() || samples[0].empty())
                out << "  Could not sample the main thread (suspend or unwind failed).\n\n";
            else if (identical)
                out << "  Stack IDENTICAL across all samples — blocked, not spinning.\n"
                       "  Consistent with a deadlock or a wait that never completes.\n\n";
            else
                out << "  Stack MOVED between samples — the thread is executing, not blocked.\n"
                       "  Consistent with a runaway loop or work that is simply enormous.\n\n";

            out << "ATTRIBUTION\n";
            if (!samples.empty())
                out << AttributeStack(samples[0], modules);
            out << "\n";

            if (auto lua = LuaProfileExcerpt(); !lua.empty()) {
                out << "CET LUA PROFILE (per-mod CPU, from luaprof)\n" << lua << "\n";
            }

            for (std::size_t i = 0; i < samples.size(); ++i) {
                out << std::format("MAIN THREAD STACK — sample {} of {}\n", i + 1, samples.size());
                if (samples[i].empty()) {
                    out << "  (unavailable)\n\n";
                    continue;
                }
                for (std::size_t f = 0; f < samples[i].size(); ++f) {
                    const auto& fr = samples[i][f];
                    out << std::format("  [{:>3}] 0x{:016X}", f, fr.address);
                    if (!fr.symbolName.empty())
                        out << std::format("  {}+0x{:X}", fr.symbolName, fr.symbolDisplacement);
                    if (!fr.moduleName.empty())
                        out << std::format("  [{}+0x{:X}]", fr.moduleName, fr.address - fr.moduleBase);
                    out << "\n";
                    if (!fr.sourceFile.empty())
                        out << std::format("          {}:{}\n", fr.sourceFile, fr.sourceLine);
                }
                out << "\n";
            }

            out << std::format("OTHER THREADS AT HANG ({} captured of {} candidates{})\n",
                otherThreads.threads.size(), otherThreads.candidates,
                otherThreads.truncated ? ", truncated" : "");
            if (otherThreads.failed)
                out << std::format("  {} thread(s) could not be sampled.\n", otherThreads.failed);
            for (const auto& t : otherThreads.threads) {
                bool modTouched = false;
                for (auto addr : t.addresses) {
                    const auto* m = Modules::FindModule(addr, modules);
                    if (m && m->isModPlugin) { modTouched = true; break; }
                }
                out << std::format("  THREAD {}{}\n", t.description, modTouched ? "  [MOD DLL ON STACK]" : "");
                for (std::size_t f = 0; f < t.addresses.size(); ++f) {
                    const auto addr = t.addresses[f];
                    out << std::format("    [{:>2}] 0x{:016X}", f, addr);
                    if (auto where = RawModuleOffset(addr, modules); !where.empty())
                        out << std::format("  {}", where);
                    out << "\n";
                }
            }
            if (otherThreads.threads.empty())
                out << "  (none captured)\n";
            out << "\n";

            out << "RESOURCE HISTORY\n" << ResourceMonitor::Report() << "\n";
            out << "MODDING STACK\n" << Frameworks::Report() << "\n";

            out << "RECENT BREADCRUMBS\n";
            for (const auto& b : Breadcrumbs::Recent())
                out << "  " << b << "\n";
            out << "\n";

            if (LoadOrder::Available()) {
                const auto& lo = LoadOrder::Entries();
                std::size_t on = 0;
                for (const auto& e : lo) on += e.enabled ? 1 : 0;
                out << std::format("MODS (content inventory — {} listed, {} active)\n", lo.size(), on);
                for (const auto& e : lo)
                    out << std::format("  {}{}\n", e.name, e.enabled ? "" : "  (inactive)");
            } else {
                out << "MODS (content inventory)\n  (unavailable)\n";
            }
            out << "\n-- end of hang report --\n";

            REX::INFO("Hang report: {}", path.string());
        }

        // ---- watchdog --------------------------------------------------------
        DWORD WINAPI Watchdog(LPVOID)
        {
            HWND hwnd = nullptr;
            for (int i = 0; i < 120 && !hwnd; ++i) {   // window may not exist yet
                hwnd = FindGameWindow();
                if (!hwnd)
                    Sleep(1000);
            }
            if (!hwnd) {
                REX::WARN("HangDetector: no game window found — hang detection disabled.");
                return 0;
            }
            REX::INFO("HangDetector: watching window {} (threshold {}s).",
                      static_cast<void*>(hwnd), kHangSeconds);

            const DWORD mainTid = ThreadInfo::MainThreadId();
            const auto heartbeat = HeartbeatPath();
            int unresponsive = 0;
            bool heartbeatSeen = false;   // no CrashBreadcrumb, or CET not up yet: pump signal only

            while (s_reports.load() < kMaxReports) {
                Sleep(1000);

                if (!IsWindow(hwnd)) {           // window went away: game shutting down
                    hwnd = FindGameWindow();
                    if (!hwnd)
                        return 0;
                    continue;
                }

                // A breadcrumb.log left by the previous session is old, so the stall
                // test only arms once this session has been seen writing it.
                std::error_code hbEc;
                std::int64_t hbAge = -1;
                if (!heartbeat.empty())
                    hbAge = FileAgeSeconds(heartbeat, hbEc);
                const bool hbValid = !heartbeat.empty() && !hbEc;
                if (hbValid && hbAge <= 2)
                    heartbeatSeen = true;
                const bool stalled = heartbeatSeen && hbValid && hbAge >= kStallSeconds;

                if (!stalled) {
                    if (PumpResponds(hwnd, 900)) {
                        unresponsive = 0;
                        continue;
                    }
                    if (++unresponsive < kHangSeconds)
                        continue;
                }

                // Confirmed hang.
                const int secs = stalled ? static_cast<int>(hbAge) : unresponsive;
                const char* why = stalled
                    ? "frame stall: CET heartbeat stopped, message pump still answering"
                    : "message pump did not answer";
                s_reports.fetch_add(1);
                Breadcrumbs::Log("HANG detected (%s) - sampling main thread",
                                 stalled ? "frame stall" : "pump");
                REX::WARN("HangDetector: {} for {}s — capturing.", why, secs);

                std::vector<std::vector<StackFrame>> samples;
                for (int i = 0; i < kSamples; ++i) {
                    samples.push_back(SampleMainThread(mainTid));
                    if (i + 1 < kSamples)
                        Sleep(kSampleGapMs);
                }

                try {
                    WriteHangReport(samples, mainTid, secs, why);
                } catch (const std::exception& e) {
                    REX::ERROR("HangDetector: report failed: {}", e.what());
                }

                // Wait for recovery, then cool down, so one long freeze is not
                // reported over and over.
                while (IsWindow(hwnd)) {
                    std::error_code ec;
                    const std::int64_t age = heartbeat.empty() ? 0 : FileAgeSeconds(heartbeat, ec);
                    const bool beating = !heartbeatSeen || ec || age <= 2;
                    if (beating && PumpResponds(hwnd, 900))
                        break;
                    Sleep(1000);
                }
                Breadcrumbs::Log("HANG recovered");
                Sleep(kCooldownSecs * 1000);
                unresponsive = 0;
            }

            REX::INFO("HangDetector: report cap ({}) reached — watchdog stopping.", kMaxReports);
            return 0;
        }
    }

    void Start()
    {
        if (HANDLE h = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr))
            CloseHandle(h);
        else
            REX::ERROR("HangDetector: could not start watchdog thread.");
    }
}

#include "PCH.h"
#include "Frameworks.h"

namespace Frameworks
{
    namespace
    {
        std::string s_report;

        // RED4ext plugins that are *frameworks* — other mods depend on them, so a
        // fault or version mismatch in one of these explains failures far away from
        // itself. Everything else in red4ext/plugins is a gameplay mod.
        constexpr std::string_view kFrameworkPlugins[] = {
            "ArchiveXL", "TweakXL", "Codeware", "RedFileSystem", "RedData",
            "RedHttpClient", "RedHotTools", "RedMemoryDump", "RedSocket",
            "input_loader", "mod_settings", "audioware", "RadioExt",
        };

        std::filesystem::path GameRoot()
        {
            wchar_t exe[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
                return {};
            return std::filesystem::path(exe).parent_path().parent_path().parent_path();
        }

        std::string FileVersionOf(const std::filesystem::path& p)
        {
            const auto s = p.string();
            DWORD dummy = 0;
            DWORD size  = GetFileVersionInfoSizeA(s.c_str(), &dummy);
            if (!size)
                return {};
            std::vector<std::byte> buf(size);
            if (!GetFileVersionInfoA(s.c_str(), 0, size, buf.data()))
                return {};
            VS_FIXEDFILEINFO* ffi = nullptr;
            UINT len = 0;
            if (!VerQueryValueA(buf.data(), "\\", reinterpret_cast<void**>(&ffi), &len) || !ffi)
                return {};
            return std::format("{}.{}.{}.{}", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                                              HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
        }

        std::vector<std::string> ReadLines(const std::filesystem::path& p, std::size_t cap = 20000)
        {
            std::vector<std::string> lines;
            std::ifstream in(p);
            if (!in)
                return lines;
            std::string l;
            while (lines.size() < cap && std::getline(in, l)) {
                if (!l.empty() && l.back() == '\r')
                    l.pop_back();
                lines.push_back(std::move(l));
            }
            return lines;
        }

        std::filesystem::path NewestMatching(const std::filesystem::path& dir, std::string_view prefix)
        {
            std::error_code ec;
            std::filesystem::path best;
            std::filesystem::file_time_type bestT{};
            if (!std::filesystem::is_directory(dir, ec))
                return {};
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (!e.is_regular_file(ec))
                    continue;
                if (!e.path().filename().string().starts_with(prefix))
                    continue;
                const auto t = e.last_write_time(ec);
                if (best.empty() || t > bestT) { best = e.path(); bestT = t; }
            }
            return best;
        }

        bool Contains(const std::string& hay, std::string_view needle)
        {
            return hay.find(needle) != std::string::npos;
        }

        // ---- redscript -------------------------------------------------------
        // The important one. redscript compiles every r6/scripts mod at launch; if
        // that fails the game still starts and every redscript mod silently does
        // nothing. Nothing in a native stack or a module list shows this.
        void ReportRedscript(std::string& out, const std::filesystem::path& root)
        {
            const auto log = root / "r6" / "logs" / "redscript_rCURRENT.log";
            std::error_code ec;
            if (!std::filesystem::is_regular_file(log, ec)) {
                out += "  redscript : no r6/logs/redscript_rCURRENT.log — did it run?\n";
                return;
            }

            const auto lines = ReadLines(log);
            bool complete = false, saved = false;
            int  errors = 0, warnings = 0;
            std::vector<std::string> firstErrors;

            for (const auto& l : lines) {
                if (Contains(l, "Compilation complete"))            complete = true;
                if (Contains(l, "Output successfully saved"))       saved = true;
                if (Contains(l, "[ERROR")) {
                    ++errors;
                    if (firstErrors.size() < 5) firstErrors.push_back(l);
                }
                else if (Contains(l, "[WARN")) ++warnings;
            }

            if (complete && saved)
                out += std::format("  redscript : OK — compiled and saved ({} warning(s))\n", warnings);
            else if (complete && !saved)
                out += "  redscript : *** compiled but the output was NOT saved — mods are NOT active ***\n";
            else
                out += std::format("  redscript : *** COMPILE DID NOT COMPLETE ({} error(s)) — every "
                                   "redscript mod is inactive ***\n", errors);
            for (const auto& e : firstErrors)
                out += std::format("              {}\n", e.substr(0, 160));
        }

        // ---- RED4ext ---------------------------------------------------------
        void ReportRED4ext(std::string& out, const std::filesystem::path& root)
        {
            const auto log = NewestMatching(root / "red4ext" / "logs", "red4ext-");
            if (log.empty()) {
                out += "  RED4ext   : no loader log found\n";
                return;
            }
            const auto lines = ReadLines(log);
            std::string ver, product, filever;
            int problems = 0;
            std::vector<std::string> firstProblems;

            for (const auto& l : lines) {
                if (ver.empty() && Contains(l, "is initializing")) {
                    const auto a = l.find("(v");
                    const auto b = l.find(')', a == std::string::npos ? 0 : a);
                    if (a != std::string::npos && b != std::string::npos)
                        ver = l.substr(a + 2, b - a - 2);
                }
                if (product.empty() && Contains(l, "Product version:"))
                    product = l.substr(l.find("Product version:") + 17);
                if (filever.empty() && Contains(l, "File version:"))
                    filever = l.substr(l.find("File version:") + 14);
                if (Contains(l, "[error") || Contains(l, "[warning")) {
                    ++problems;
                    if (firstProblems.size() < 6) firstProblems.push_back(l);
                }
            }

            out += std::format("  RED4ext   : v{}  |  game {}  |  file {}\n",
                               ver.empty() ? "?" : ver,
                               product.empty() ? "?" : product,
                               filever.empty() ? "?" : filever);
            if (problems) {
                out += std::format("              {} error/warning line(s) in the loader log:\n", problems);
                for (const auto& p : firstProblems)
                    out += std::format("              {}\n", p.substr(0, 160));
            }
        }

        // ---- CET -------------------------------------------------------------
        void ReportCET(std::string& out, const std::filesystem::path& root)
        {
            const auto dir = root / "bin" / "x64" / "plugins" / "cyber_engine_tweaks";
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec)) {
                out += "  CET       : not installed\n";
                return;
            }
            out += std::format("  CET       : v{}\n",
                FileVersionOf(root / "bin" / "x64" / "plugins" / "cyber_engine_tweaks.asi"));

            // scripting.log is where a broken Lua mod announces itself, over and
            // over. A five-figure count of identical tracebacks is a mod aborting
            // every frame, which is a performance bug as much as a correctness one.
            const auto slog = dir / "scripting.log";
            if (!std::filesystem::is_regular_file(slog, ec))
                return;
            const auto lines = ReadLines(slog);
            int errors = 0;
            std::vector<std::string> firstErrors;
            for (const auto& l : lines) {
                if (Contains(l, "[error]") || Contains(l, "Error:") || Contains(l, "stack traceback")) {
                    ++errors;
                    if (firstErrors.size() < 5) firstErrors.push_back(l);
                }
            }
            if (errors) {
                out += std::format("              {} Lua error line(s) in scripting.log:\n", errors);
                for (const auto& e : firstErrors)
                    out += std::format("              {}\n", e.substr(0, 160));
            }
        }

        // ---- plugin inventory -------------------------------------------------
        void ReportPlugins(std::string& out, const std::filesystem::path& root)
        {
            std::error_code ec;
            const auto dir = root / "red4ext" / "plugins";
            if (!std::filesystem::is_directory(dir, ec))
                return;
            std::vector<std::string> fw, mods;
            for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
                if (!e.is_directory(ec))
                    continue;
                const auto name = e.path().filename().string();
                const bool isFw = std::any_of(std::begin(kFrameworkPlugins), std::end(kFrameworkPlugins),
                    [&](std::string_view f) { return _stricmp(f.data(), name.c_str()) == 0; });
                (isFw ? fw : mods).push_back(name);
            }
            std::sort(fw.begin(), fw.end());
            std::sort(mods.begin(), mods.end());

            // Frameworks are called out separately because other mods depend on them:
            // a fault in one explains failures nowhere near itself.
            out += std::format("  frameworks in red4ext/plugins ({}): ", fw.size());
            for (std::size_t i = 0; i < fw.size(); ++i)
                out += (i ? ", " : "") + fw[i];
            out += std::format("\n  other RED4ext plugins ({}): ", mods.size());
            for (std::size_t i = 0; i < mods.size(); ++i)
                out += (i ? ", " : "") + mods[i];
            out += "\n";
        }
    }

    void Init()
    {
        try {
            const auto root = GameRoot();
            if (root.empty()) {
                s_report = "  (game root not found)\n";
                return;
            }
            std::string out;
            ReportRED4ext(out, root);
            ReportRedscript(out, root);
            ReportCET(out, root);
            ReportPlugins(out, root);
            s_report = std::move(out);
            REX::INFO("Frameworks: captured modding-stack health.");
        } catch (const std::exception& e) {
            s_report = std::format("  (framework probe failed: {})\n", e.what());
        }
    }

    const std::string& Report() { return s_report; }
}

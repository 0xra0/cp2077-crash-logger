#pragma once

// Minimal stand-in for CommonLibSF's REX logging.
//
// The Starfield build got REX::INFO/WARN/ERROR from CommonLibSF. An ASI has no
// SDK at all, so this provides the same three entry points with the same
// std::format signature — which is why not one of the 15 call sites in the
// ported sources had to change.
//
// NOTE: <wingdi.h> #defines ERROR to 0. PCH.h #undefs it after <windows.h> and
// before this header, exactly as the Starfield build did.

#include <format>
#include <string>
#include <string_view>
#include <filesystem>

namespace REX
{
    namespace detail
    {
        // Appends one timestamped line to CrashLogger.log next to this DLL.
        // Never throws; a logging failure must never take the game down.
        void Write(std::string_view level, std::string_view msg) noexcept;
    }

    // Records the module handle so the log lands beside the .asi. Call from DllMain.
    void InitLog(void* selfModule) noexcept;

    // Directory this DLL lives in (…/bin/x64/plugins). Empty until InitLog runs.
    const std::filesystem::path& SelfDir() noexcept;

    template <class... A>
    void INFO(std::format_string<A...> f, A&&... a) noexcept
    {
        try { detail::Write("INFO", std::format(f, std::forward<A>(a)...)); } catch (...) {}
    }

    template <class... A>
    void WARN(std::format_string<A...> f, A&&... a) noexcept
    {
        try { detail::Write("WARN", std::format(f, std::forward<A>(a)...)); } catch (...) {}
    }

    template <class... A>
    void ERROR(std::format_string<A...> f, A&&... a) noexcept
    {
        try { detail::Write("ERROR", std::format(f, std::forward<A>(a)...)); } catch (...) {}
    }
}

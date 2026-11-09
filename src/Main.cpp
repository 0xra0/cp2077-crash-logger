#include "PCH.h"
#include "AddressLibrary.h"
#include "Breadcrumbs.h"
#include "CrashHandler.h"
#include "Frameworks.h"
#include "HangDetector.h"
#include "LoadOrder.h"
#include "ResourceMonitor.h"
#include "SessionRecovery.h"
#include "LogWriter.h"
#include "SystemInfo.h"
#include "ThreadInfo.h"

// -----------------------------------------------------------------------------
// ASI entry point (Cyberpunk 2077).
//
// The Starfield build used SFSE_PLUGIN_PRELOAD / SFSE_PLUGIN_LOAD. CP2077 has no
// equivalent two-phase plugin API, so the split is reproduced by hand:
//
//   * DllMain          — the PRELOAD equivalent. Ultimate ASI Loader (bin/x64/
//                        version.dll) loads .asi files very early, before RED4ext
//                        and before cyber_engine_tweaks.asi, so installing the
//                        handler here really does cover the other plugins.
//   * deferred thread  — the LOAD equivalent. Everything that is not needed to
//                        catch a crash runs here.
//
// Why the split matters: DllMain runs under the loader lock. Installing a VEH,
// opening a file and reading our own module list are all safe there; enumerating
// GPUs, walking the mod tree and rescanning every module's IAT are not, and would
// risk a deadlock against whatever else the loader is doing.
//
// The deferred work is a backstop, not the mechanism: CrashHandler registers with
// ntdll!LdrRegisterDllNotification (which Wine implements), so modules that arrive
// after us are patched as they land. OnPostLoad() only catches anything that slipped
// through before the notification was registered.
// -----------------------------------------------------------------------------

namespace
{
    DWORD WINAPI DeferredInit(LPVOID)
    {
        // Phase 1 — everything that only describes the machine and the install.
        // Runs immediately, because these are the fields a crash report is missing
        // if it arrives early, and early is exactly when this game crashes: during
        // a save load or world attach. Waiting would reliably produce reports with
        // "(inventory unavailable)" on the very crashes worth reporting.
        // It is a directory walk over ~1300 entries reading no file contents, on a
        // background thread — cheap enough not to disturb the load it overlaps.
        try {
            SessionRecovery::Init(); // stale session marker + native REDEngine ReportQueue recovery
            SystemInfo::Init();       // OS/CPU/GPU/Wine (memory is sampled at crash time)
            ResourceMonitor::Start(); // pressure history survives OOM/resource-exhaustion crashes
            AddressLibrary::Init();   // inert on CP2077 for now; records why
            LoadOrder::Init();        // .archive / .xl / tweaks / input / CET / redscript
            Frameworks::Init();       // modding-stack health: did redscript compile?
            LogWriter::WriteStartupLog();
            // Freezes are not crashes: nothing faults, so nothing is dispatched to
            // the handler above. The watchdog is the only thing that will ever see one.
            HangDetector::Start();
            REX::INFO("CrashLogger ready. Crash logs -> {}", LogWriter::GetLogDir().string());
        } catch (const std::exception& e) {
            REX::ERROR("CrashLogger: startup capture failed: {}", e.what());
        }

        // Phase 2 — the IAT rescan, which is the only part that actually needs the
        // other plugins to be loaded. The DLL-notification callback registered in
        // DllMain is already handling them as they arrive, so this is only a
        // backstop for anything mapped before that callback went live.
        Sleep(15000);
        try {
            CrashHandler::OnPostLoad();
            Breadcrumbs::Log("deferred init complete - game running");
        } catch (const std::exception& e) {
            REX::ERROR("CrashLogger: post-load rescan failed: {}", e.what());
        }
        return 0;
    }
}

BOOL APIENTRY DllMain(HMODULE a_module, DWORD a_reason, LPVOID a_reserved)
{
    if (a_reason == DLL_PROCESS_DETACH) {
        if (a_reserved)
            SessionRecovery::OnProcessDetach();
        return TRUE;
    }
    if (a_reason != DLL_PROCESS_ATTACH)
        return TRUE;

    DisableThreadLibraryCalls(a_module);

    REX::InitLog(a_module);           // log beside the .asi; needed by everything below
    Breadcrumbs::Init();              // early trace + first-chance logger
    ThreadInfo::Init();               // records this (the main) thread, watches thread names
    CrashHandler::Install();          // SEH filter + VEH guard + fail-fast IAT hooks
    Breadcrumbs::Log("crash handler installed (DllMain)");

    // Not std::thread: that can pull in CRT machinery we do not want to touch
    // under the loader lock. The thread does not start running until DllMain returns.
    if (HANDLE h = CreateThread(nullptr, 0, DeferredInit, nullptr, 0, nullptr))
        CloseHandle(h);

    return TRUE;
}

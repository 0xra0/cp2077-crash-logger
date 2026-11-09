#pragma once

// -----------------------------------------------------------------------------
// Frameworks — what the modding stack itself says about its own health.
//
// The module list proves a DLL is mapped. It cannot say whether RED4ext refused a
// plugin for a version mismatch, or whether the redscript compile failed — and a
// failed redscript compile is the quiet one: the game starts, every redscript mod
// silently does nothing, and nothing on a native stack hints at it.
//
// Each framework already writes that verdict to its own log. This reads them once
// and folds the answers into the report:
//
//   r6/logs/redscript_rCURRENT.log   did the last compile succeed?
//   red4ext/logs/red4ext-*.log       RED4ext + game version, plugin load failures
//   .../cyber_engine_tweaks.log      CET init
//   .../scripting.log                CET Lua errors
//
// Everything is parsed at startup and cached as strings. Nothing here touches the
// filesystem on the crash path — the same discipline the rest of the reporter keeps.
// -----------------------------------------------------------------------------

namespace Frameworks
{
    // Detect installed frameworks and parse their logs. Deferred-init thread only.
    void Init();

    // Pre-rendered report section. Safe to call on the crash path (returns a cached
    // string; performs no I/O).
    const std::string& Report();
}

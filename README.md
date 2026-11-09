# CrashLogger for Cyberpunk 2077 (`cp2077-crash-logger`)

[![Status](https://img.shields.io/badge/status-WIP%20%2F%20On--Hold-yellow.svg)](https://github.com/0xra0/cp2077-crash-logger)
[![Game Version](https://img.shields.io/badge/Cyberpunk%202077-v2.1x%20--%20v2.3x-blue.svg)](https://www.cyberpunk.net/)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20(Proton%2FWine)-purple.svg)](#requirements--compatibility)
[![Language](https://img.shields.io/badge/language-C%2B%2B23-orange.svg)](#building-from-source)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-green.svg)](LICENSE)
[![Latest Release](https://img.shields.io/github/v/release/0xra0/cp2077-crash-logger?include_prereleases&color=brightgreen)](https://github.com/0xra0/cp2077-crash-logger/releases)

A high-fidelity, self-contained native **ASI crash logger and hang/freeze detector** for **Cyberpunk 2077** (REDengine 4), designed from the ground up for both **Linux / Steam Deck (Proton / Wine)** and **Windows**.

Derived from the lineage of [`sf-crash-logger`](https://github.com/0xra0/sf-crash-logger) and engineered specifically around Cyberpunk 2077's internals, memory layout, and modding ecosystem.

---

> [!IMPORTANT]
> **Project Status: Work in Progress (WIP) / Not Actively Developing**  
> This project is currently in a functional preview/prototype state (v0.2.0). The core mechanics—including exception handling, x64 unwind-data stack walking, address-library symbolication, hang watchdog, resource tracking, and postmortem recovery—are fully implemented, verified, and operational.  
> However, **active ongoing feature development is currently paused / on hold**. The repository is published to provide the Cyberpunk 2077 community and modders with a reliable crash investigation tool and an open reference codebase. Pull requests, issue reports, community maintenance, and forks are warmly welcomed!

---

## Table of Contents

- [Why CrashLogger?](#why-crashlogger)
- [Key Features](#key-features)
- [Architecture & Design](#architecture--design)
  - [Reverse-Engineered Address Library](#address-library-decoded)
  - [Wine / Proton Optimization](#wine--proton-first-class-support)
  - [Dual-Signal Hang & Freeze Watchdog](#hang-detection-freezes-not-crashes)
- [Requirements & Compatibility](#requirements--compatibility)
- [Installation](#installation)
  - [Prerequisites (Ultimate ASI Loader)](#1-install-ultimate-asi-loader)
  - [Installing CrashLogger](#2-install-crashlogger)
  - [Mod Managers (Vortex / MO2)](#mod-manager-notes-vortex--mod-organizer-2)
  - [Verifying Installation](#verifying-installation)
  - [Uninstallation](#uninstallation)
- [Log Files & Output](#log-files--output)
- [How to Read & Interpret Reports](#how-to-read--interpret-reports)
- [Building from Source](#building-from-source)
- [Known Limitations](#known-limitations)
- [Contributing](#contributing)
- [License & Disclaimers](#license--disclaimers)

---

## Why CrashLogger?

Under Linux / Proton / Wine, native crashes in Cyberpunk 2077 notoriously produce an investigation black hole:
* **RED4ext** logs nothing useful or dies before flushing buffers.
* **`REDEngineErrorReporter`** fails to generate a `.dmp` minidump file.
* The game's native `CrashInfo.json` typically provides only a vague world location coordinate rather than a stack trace.
* DbgHelp-based stack walkers frequently collapse under Wine/Proton.

On Windows, complex multi-mod setups (mixing RED4ext plugins, Cyber Engine Tweaks Lua scripts, Redscript, ArchiveXL, and TweakXL) often suffer from hard freezes or unhandled exceptions where standard error reports fail to isolate which mod or engine subsystem triggered the failure.

**CrashLogger fills this void.** Loaded as an early-stage ASI plugin via [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader), it captures fatal exceptions, memory faults, and engine stalls before the game aborts. Reports are self-contained, human-readable, and symbolicated **without requiring WinDbg, PDB symbols, or post-processing tools**.

---

## Key Features

- **Symbolized Stack Unwinding via Address Library**  
  Integrates a reverse-engineered parser for Cyberpunk 2077's address database (`cyberpunk2077_addresses.json` cache). Resolves raw instruction pointers to game function IDs and offsets (e.g. `Cyberpunk2077.exe+0x1420B0 [ID: 844424+0x30]`) with **99.973% verified accuracy**.
- **First-Class Proton / Wine Support**  
  Uses native x64 unwind metadata (`RUNTIME_FUNCTION`) directly instead of depending on DbgHelp. Explicitly compensates for Wine's `SetThreadStackGuarantee` behaviors and captures host Linux kernel telemetry.
- **Dual-Signal Hang / Freeze Detector**  
  Monitors both main-window message pump responsiveness (`WM_NULL` / `SMTO_ABORTIFHUNG`) and frame loop heartbeats via CET `CrashBreadcrumb`. On a freeze, it executes a deadlock-safe triple-sample unwinding under thread suspension to classify the freeze as **Blocked / Deadlocked** vs. **Executing / Runaway Loop**.
- **Full Mod & Framework Inventory**  
  Automatically catalogues your active modding environment:
  - **RED4ext Plugins** (`red4ext/plugins/`)
  - **Cyber Engine Tweaks (CET) Mods** (`bin/x64/plugins/cyber_engine_tweaks/mods/`, distinguishing active mods from disabled ones lacking `init.lua`)
  - **Redscript Mods** (`r6/scripts/`)
  - **Content Archives** (`archive/pc/mod/*.archive` in exact alphabetical load order)
  - **CET Lua VM Profiling** (integrates with `luaprof` to report top Lua consumers when CET is on the stack)
- **All-Thread Snapshot Analysis**  
  On crash, unwinds up to 48 background worker threads (24 frames each). Identifies worker-thread faults and cross-thread deadlocks, highlighting any thread carrying mod code.
- **Postmortem Recovery for Hard Kills**  
  Tracks session lifecycles via `sessions/<pid>.active`. If the process terminates abnormally (e.g. `__fastfail`, driver crash, OOM kill, or forced termination), the subsequent launch writes a `postmortem_<timestamp>_pid<pid>.log`, automatically harvesting and correlating native `ReportQueue` artifacts (`stacktrace.txt`, `CrashInfo.json`).
- **Resource Pressure Watchdog**  
  Continuous 5-second background sampling of system commit charge, available physical RAM, private working set, handle counts, and disk space. Records warning breadcrumbs on spikes and embeds resource snapshots in crash reports.
- **Resilient Rolling Trace Buffer**  
  Maintains `CrashLogger_trace.log` using allocation-free raw Win32 `WriteFile` calls. Survives severe heap corruption and abrupt termination when conventional loggers fail.
- **Zero Runtime Dependencies**  
  Built with a statically linked MSVC C++ Runtime (`/MT`). Imports only fundamental OS libraries (`KERNEL32`, `USER32`, `VERSION`, `dbghelp`).

---

## Architecture & Design

### Address Library: Decoded

Cyberpunk 2077's address database is Ultimate ASI Loader's binary cache of the game's 132 MB `bin/x64/cyberpunk2077_addresses.json`, rather than Bethesda's VersionLib format. The format was reverse-engineered and validated at **99.973% accuracy** over 60,000 records:

```
offset 0        int64        magic = -11
offset 8        int64        count           (921,876 on patch 2.31)
offset 16       int64        mask  = 0x1FFFFF  -> 2^21 slots
offset 24       byte[16]     reserved
offset 40       byte[2^21]   secondary index
offset 40+2^21  uint64[2^21] slots; non-zero => (RVA << 32) | id32
```

* **Slot Occupancy:** The byte array at offset 40 is not a Swiss-table control block; slot occupancy is simply evaluated as `value != 0`.
* **Lookups:** The hash function is bypassed in favor of sorting all slots by RVA at initialization and performing binary searches (`std::lower_bound`), yielding fast and exact address-to-ID resolutions.
* **Version Verification:** The cache filename encodes the executable file version across four 16-bit lanes:
  $$\text{VersionKey} = (\text{major} \ll 48) \mid (\text{minor} \ll 32) \mid (\text{build} \ll 16) \mid \text{revision}$$
  For instance, `3.0.80.51928` resolves to `844424935426776`. If a patch mismatch is detected, the database is safely bypassed to prevent false symbolication.

### Wine / Proton First-Class Support

Proton execution requires specialized handling:
1. **Unwind Tables over DbgHelp:** DbgHelp stack walking routinely fails or truncates under Proton's Wine environment. CrashLogger parses the PE `.pdata` section directly using Win64 SEH unwind codes (`RtlLookupFunctionEntry` / `RtlVirtualUnwind`).
2. **Late-Loading Plugin Hooks:** Registers with `ntdll!LdrRegisterDllNotification` (fully implemented in Wine). Mod DLLs loaded dynamically after initialization still have their Import Address Tables (IAT) hooked.
3. **Environment Recognition:** Probes for `wine_get_version` in `ntdll.dll` to record the Wine / Proton version and queries the underlying Linux host kernel.

### Hang Detection: Freezes, Not Crashes

A crash is an event handed to you by the OS; a freeze is the *absence* of events.
- **Message Pump Signal:** Sends `WM_NULL` with `SMTO_ABORTIFHUNG` to the main window. If the message loop fails to respond within 6 seconds (`kHangSeconds`), the watchdog triggers.
- **Heartbeat Signal:** If the CET mod `CrashBreadcrumb` is present, the watchdog monitors `breadcrumb.log` (updated every frame). If stalled for $> 10$ seconds (`kStallSeconds`), a freeze report is recorded even if the message pump is still nominally answering.
- **Deadlock-Safe Unwinding:** Suspending a thread while it holds CRT heap or DbgHelp locks can permanently deadlock the process. CrashLogger splits the unwind pipeline:
  1. `Unwind()` runs under suspension without heap allocations or mutexes.
  2. The thread is resumed immediately.
  3. `Annotate()` symbolicates the captured frame addresses safely.

---

## Requirements & Compatibility

| Component | Requirement |
|---|---|
| **Game Version** | Cyberpunk 2077 v2.1x – v2.3x (Phantom Liberty compatible) |
| **Operating System** | Windows 10/11 (x64) **or** Linux (Proton 8+, Proton Experimental, Wine 8+, Steam Deck / SteamOS) |
| **Required Loader** | [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (`version.dll` in `bin/x64/`) |
| **Mod Frameworks** | Fully compatible alongside RED4ext, Cyber Engine Tweaks (CET), Redscript, ArchiveXL, TweakXL |

---

## Installation

### 1. Install Ultimate ASI Loader
CrashLogger requires Ultimate ASI Loader so it can load early enough to intercept other mod plugins.
1. Download `Ultimate-ASI-Loader_x64.zip` from the [Ultimate ASI Loader Releases](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases).
2. Extract the file `version.dll` into your game's executable directory:
   ```
   <Cyberpunk 2077>/bin/x64/version.dll
   ```
   *(Note: If you already use Cyber Engine Tweaks or another mod that uses `version.dll`, Ultimate ASI Loader may already be installed).*

### 2. Install CrashLogger
1. Download the latest release from the [GitHub Releases](https://github.com/0xra0/cp2077-crash-logger/releases) page:
   - `CrashLogger.asi` (or the archive `cp2077-crash-logger-v0.2.0.zip`).
2. Place `CrashLogger.asi` into your game's `plugins` folder:
   ```
   <Cyberpunk 2077>/bin/x64/plugins/CrashLogger.asi
   ```
   *(Create the `plugins` folder inside `bin/x64/` if it does not already exist).*

#### Directory Structure Verification
Your directory structure should look like this:
```
Cyberpunk 2077/
└── bin/
    └── x64/
        ├── Cyberpunk2077.exe
        ├── version.dll                 <-- Ultimate ASI Loader
        ├── plugins/
        │   ├── CrashLogger.asi         <-- This tool
        │   └── cyber_engine_tweaks.asi (optional)
        └── CrashLogger/                <-- Generated automatically on first launch
            ├── CrashLogger.log
            └── CrashLogger_trace.log
```

### Mod Manager Notes (Vortex / Mod Organizer 2)
- **Vortex:** Ensure the mod is deployed to the root game directory or configured as an ASI plugin targeting `bin/x64/plugins/`.
- **Mod Organizer 2 (Root Builder):** Ensure `CrashLogger.asi` is mapped to `Root/bin/x64/plugins/CrashLogger.asi`.

### Verifying Installation
1. Launch Cyberpunk 2077 to the main menu and exit.
2. Navigate to `<Cyberpunk 2077>/bin/x64/CrashLogger/`.
3. Open `CrashLogger.log`. You should see an entry confirming initialization:
   ```
   [2026-09-18 16:11:02] CrashLogger v0.2.0 loaded — crash logs -> <path>/bin/x64/CrashLogger
   ```

### Uninstallation
Simply delete `CrashLogger.asi` from `<Cyberpunk 2077>/bin/x64/plugins/`. No other game files are modified.

---

## Log Files & Output

All output is saved to:
```
<Cyberpunk 2077>/bin/x64/CrashLogger/
```

| File Name | Description |
|---|---|
| `CrashLogger.log` | One-line launch confirmation and startup summary verifying the ASI loaded. |
| `CrashLogger_trace.log` | Continuous rolling trace breadcrumbs: first-chance SEH events, memory pressure alerts, and lifecycle markers. Written via unbuffered, zero-allocation Win32 calls. Survives corrupted heaps. |
| `crash_<timestamp>.log` | Full crash report containing fault details, registers, symbolized callstack, mod attribution, memory maps, and background threads. |
| `hang_<timestamp>.log` | Freeze diagnostic report. Contains freeze verdict (`BLOCKED` vs `EXECUTING`), 3 main-thread stack samples, and CET Lua VM profiler rankings. |
| `postmortem_<timestamp>_pid<pid>.log` | Next-launch recovery log written after an abnormal termination or hard kill. Correlates `%LOCALAPPDATA%\REDEngine\ReportQueue` crash info and dumps. |
| `sessions/<pid>.active` | Small temporary session marker. Deleted on clean game shutdown. |
| `crash_<timestamp>.dmp` | Standard Windows minidump file compatible with WinDbg / Visual Studio. |

---

## How to Read & Interpret Reports

### Crash Report Breakdown

A typical `crash_<timestamp>.log` report contains the following key sections:

```
CrashLogger v0.2.0 (Cyberpunk 2077 3.0.80.51928)
Exception: 0xC0000005 (EXCEPTION_ACCESS_VIOLATION)
Fault Address: Cyberpunk2077.exe+0x0142A9B0
Fault Details: Read access violation at address 0x0000000000000038 (near-null dereference)

REGISTERS:
  RAX: 0x0000000000000000 (near null)
  RBX: 0x00007FF7D492A000 [Cyberpunk2077.exe]
  ...

CALL STACK:
  [0] Cyberpunk2077.exe+0x0142A9B0 [ID: 844424+0x38]
  [1] red4ext.dll+0x00012A40       [RED4ext]
  [2] Cyberpunk2077.exe+0x00512800 [ID: 812940+0x50]
  [3] KERNEL32.DLL+0x00017034
  [4] ntdll.dll+0x000526A1

ATTRIBUTION & MODS ON STACK:
  * red4ext.dll [RED4ext Plugin Loader]

ACTIVE MOD INVENTORY:
  [Archive] 00_CustomBody.archive
  [Archive] 01_ClothingMod.archive
  [CET]     AppearanceMenuMod (enabled)
  [Redscript] Codeware
```

1. **Exception & Fault Address:** Tells you the nature of the crash (e.g. `0xC0000005` null pointer, `0xC0000374` heap corruption, `0xC0000409` fail-fast).
2. **Call Stack:** Displays the execution flow at the moment of failure. Addresses within `Cyberpunk2077.exe` include `[ID: <id>+<disp>]` resolving directly into game symbols.
3. **Attribution:** Highlights whether any third-party DLL (`RED4ext`, `CET`, or other ASI plugins) appears in the crash stack frames.
4. **Mod Inventory:** Lists your installed archives in alphabetical order, enabled/disabled CET mods, and Redscript scripts for full environment context.

### Sharing Logs for Support
When asking for help in modding communities or reporting an issue to mod authors:
- Upload the relevant `crash_<timestamp>.log` or `hang_<timestamp>.log`.
- Attach `CrashLogger_trace.log` if the crash resulted in a hard freeze or silent desktop exit.
- Check and redact any personal directory paths (e.g. containing your Windows or Linux username) if privacy is desired.

---

## Building from Source

### Prerequisites
- **C++ Compiler:** Supporting C++23 (`/std:c++latest`).
- **Build System:** CMake 3.21+ and Ninja.
- **Toolchain:** MSVC (Visual Studio 2022) on Windows, or MSVC via Wine (`msvc-wine`) on Linux.

### Building on Linux (using `msvc-wine`)
1. Ensure your MSVC toolchain is accessible via Wine (see `msvc-wine-toolchain.cmake`).
2. Configure and build:
   ```bash
   cmake -G Ninja -S . -B build \
     -DCMAKE_TOOLCHAIN_FILE=./msvc-wine-toolchain.cmake \
     -DCMAKE_BUILD_TYPE=Release
   ninja -C build
   ```
3. The resulting `CrashLogger.asi` binary will be generated in `build/`.

### Building on Windows (Native MSVC)
1. Open a Visual Studio x64 Native Tools Command Prompt.
2. Run:
   ```cmd
   cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release
   ninja -C build
   ```
   *(Or generate a Visual Studio solution using `cmake -G "Visual Studio 17 2022" -A x64 -B build`).*

### Key Compiler Flags
- `/EHa` (Asynchronous Exception Handling): Required to catch hardware access violations reliably inside `__try` / `__except` blocks without losing structured exception records.
- `/MT` (Static CRT): Enforces static linking against the C++ runtime. Prevents silent load failures if a dynamic runtime DLL is absent or corrupted.

---

## Known Limitations

- **Early Process Startup Crashes:** If a crash occurs in the first milliseconds of process startup before Ultimate ASI Loader initializes `CrashLogger.asi`, the report may indicate `captured at plugin load`.
- **Raw `__fastfail` (`int 29h`):** Fail-fast instructions bypass user-mode exception handlers by OS design. CrashLogger mitigates this via its postmortem session recovery (`sessions/<pid>.active`), surfacing the abnormal termination and collecting REDengine crash logs on the subsequent launch.

---

## Contributing

While the original author is not actively developing new features, community contributions, fixes, and improvements are very welcome!

1. Fork the repository.
2. Create your feature or bugfix branch (`git checkout -b feature/my-feature`).
3. Commit your changes (`git commit -m 'Add support for new feature'`).
4. Push to your branch (`git push origin feature/my-feature`).
5. Open a Pull Request.

---

## License & Disclaimers

### License
This project is licensed under the **GNU General Public License v3.0** (GPLv3). See the [LICENSE](LICENSE) file for the full license text.

### Disclaimers & Trademarks
- **Cyberpunk 2077** is a registered trademark of **CD PROJEKT S.A.**
- This project is an independent open-source tool and is **not affiliated with, endorsed by, or supported by CD PROJEKT RED**.
- Special thanks to the creators and maintainers of [`sf-crash-logger`](https://github.com/0xra0/sf-crash-logger), **Ultimate ASI Loader**, **RED4ext**, **Cyber Engine Tweaks**, and the broader Cyberpunk 2077 modding community.

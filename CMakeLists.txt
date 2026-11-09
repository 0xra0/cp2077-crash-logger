cmake_minimum_required(VERSION 3.21)
project(CrashLogger VERSION 0.2.0 LANGUAGES CXX)

# Built as an ASI: Ultimate ASI Loader (bin/x64/version.dll) loads *.asi from
# bin/x64/plugins/ very early — before RED4ext and before cyber_engine_tweaks.asi.
# That ordering is the whole point: the handler has to be installed before the
# plugins whose crashes it is meant to catch.
add_library(CrashLogger SHARED
    src/AddressLibrary.cpp
    src/Breadcrumbs.cpp
    src/CrashContext.cpp
    src/CrashHandler.cpp
    src/CrashSignature.cpp
    src/Frameworks.cpp
    src/HangDetector.cpp
    src/LoadOrder.cpp
    src/Log.cpp
    src/LogWriter.cpp
    src/Main.cpp
    src/Modules.cpp
    src/RTTIReader.cpp
    src/ResourceMonitor.cpp
    src/SessionRecovery.cpp
    src/StackWalker.cpp
    src/SystemInfo.cpp
    src/ThreadInfo.cpp
    src/ThreadSnapshot.cpp
)

target_include_directories(CrashLogger PRIVATE include)
target_precompile_headers(CrashLogger PRIVATE include/PCH.h)

set_target_properties(CrashLogger PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "CrashLogger"
    SUFFIX ".asi"          # what the ASI loader scans for
    PREFIX ""
)

# Static CRT. The dynamic runtime would add MSVCP140 / VCRUNTIME140 /
# api-ms-win-crt-* to the imports, and a DLL that fails to resolve an import is
# never loaded at all — silently. For a crash logger that is the worst possible
# failure: it would go missing precisely when the install is in a bad state
# (rebuilt prefix, missing redist, moved game). Statically linked, the only
# imports left are DLLs Windows/Wine always provides.
set_property(TARGET CrashLogger PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

# WIN32_LEAN_AND_MEAN / NOMINMAX are defined in PCH.h; defining them here too
# just produces C4005 redefinition warnings on every translation unit.
target_compile_definitions(CrashLogger PRIVATE
    CRASHLOGGER_VERSION="${PROJECT_VERSION}"
    UNICODE
    _UNICODE
)

# /EHa — asynchronous exception handling.
#
# Carried over from the Starfield build for the same reason it was needed there:
# under the default /EHsc a __try/__except block does not reliably catch a
# hardware access violation, which would silently disable every guarded read in
# the crash handler — the failure mode being a crash logger that itself dies on
# the crash it was written to report.
#
# /EHa also makes `catch (...)` swallow SEH exceptions, which is why the codebase
# uses typed `catch (const std::exception&)` throughout. Do not "tidy" those.
target_compile_options(CrashLogger PRIVATE
    /EHa
    /W4
    /permissive-
    /Zc:preprocessor
    /utf-8
    /std:c++latest
)

target_link_libraries(CrashLogger PRIVATE
    dbghelp
    psapi
    version
    shell32
    ole32
    user32
)

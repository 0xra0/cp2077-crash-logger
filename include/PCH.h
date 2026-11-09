#pragma once

#include <cassert>
#include <cstdint>
#include <ctime>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The Starfield build included RE/Starfield.h and SFSE/SFSE.h here. This port is
// a plain ASI and links against no game SDK at all, so both are gone. Nothing in
// the crash handler needed them: the project was already built with
// no_struct_use, i.e. it never reads a game struct or a game address.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef ERROR    // wingdi.h #defines ERROR 0; conflicts with REX::ERROR
#include <DbgHelp.h>
#include <Psapi.h>
#include <ShlObj.h>

#include "Log.h"    // REX::INFO/WARN/ERROR — must come after the #undef ERROR above

using namespace std::literals;

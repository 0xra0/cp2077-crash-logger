#pragma once

struct ModuleInfo
{
    std::string   name;
    std::string   path;
    std::uint64_t base{};
    std::uint32_t size{};
    std::string   version;

    // Starfield had exactly one mod-DLL location (Data/SFSE/Plugins), so a bool
    // sufficed. CP2077 has three that matter, and which one a module came from
    // changes what a crash in it means — so the kind is carried too.
    //   "RED4ext" — red4ext/plugins/<Name>/*.dll
    //   "ASI"     — bin/x64/plugins/*.asi   (cyber_engine_tweaks lives here)
    //   "loader"  — bin/x64/{version,winmm,...}.dll, the proxy that starts it all
    bool          isModPlugin{ false };
    std::string   kind;
};

namespace Modules
{
    // Returns all loaded modules, sorted by base address (required by FindModule).
    std::vector<ModuleInfo> GetAll();

    // Returns the module that contains the given address, or nullptr if none.
    // Requires `modules` to be base-sorted, as returned by GetAll(); binary search.
    // The returned pointer is valid for the lifetime of the passed-in vector.
    const ModuleInfo* FindModule(std::uint64_t address, const std::vector<ModuleInfo>& modules);
}

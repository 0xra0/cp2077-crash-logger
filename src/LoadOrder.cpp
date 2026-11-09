#include "PCH.h"
#include "LoadOrder.h"

// -----------------------------------------------------------------------------
// CP2077 port of the content-plugin inventory.
//
// Starfield read %LOCALAPPDATA%\Starfield\Plugins.txt — one file, already in load
// order. Cyberpunk has no such file: what a mod "is" is spread across four trees,
// and only one of them (red4ext/plugins) shows up in the module list at all. So
// the inventory is built by walking the install:
//
//   archive/pc/mod/*.archive   content mods. The closest analogue to .esm/.esp,
//                              and the ones the module list can never show.
//                              Loaded in filename order, so the index is real.
//   bin/x64/plugins/cyber_engine_tweaks/mods/<Name>/   CET Lua mods
//   r6/scripts/<Name>          redscript mods
//
// The `enabled` flag carries real information for CET mods: CET cannot load a mod
// whose folder has no init.lua, and that is exactly how this install's mod manager
// disables one — the folder stays, the entry point is removed. A crash report that
// lists a CET mod as present when it is actually inert would be actively misleading.
//
// Runs once on the deferred-init thread, never on the crash path.
// -----------------------------------------------------------------------------

namespace LoadOrder
{
    namespace
    {
        std::vector<Entry> s_entries;
        std::string        s_path;
        bool               s_available = false;

        // bin/x64/<exe> -> the game root two levels up.
        std::filesystem::path GameRoot()
        {
            wchar_t exe[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
                return {};
            return std::filesystem::path(exe).parent_path().parent_path().parent_path();
        }

        bool HasInitLua(const std::filesystem::path& dir)
        {
            std::error_code ec;
            for (const auto& f : std::filesystem::directory_iterator(dir, ec)) {
                if (!f.is_regular_file(ec))
                    continue;
                auto name = f.path().filename().string();
                std::transform(name.begin(), name.end(), name.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (name == "init.lua")
                    return true;
            }
            return false;
        }

        void CollectFiles(const std::filesystem::path& dir, const char* ext, const char* tag)
        {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec))
                return;
            std::vector<std::string> names;
            for (const auto& f : std::filesystem::directory_iterator(dir, ec)) {
                if (f.is_regular_file(ec) && f.path().extension() == ext)
                    names.push_back(f.path().filename().string());
            }
            std::sort(names.begin(), names.end());   // the game's own load order
            for (auto& n : names)
                s_entries.push_back(Entry{ std::format("{}{}", tag, n), true });
        }

        void CollectDirs(const std::filesystem::path& dir, const char* tag, bool checkInitLua)
        {
            std::error_code ec;
            if (!std::filesystem::is_directory(dir, ec))
                return;
            std::vector<std::pair<std::string, bool>> found;
            for (const auto& d : std::filesystem::directory_iterator(dir, ec)) {
                if (!d.is_directory(ec))
                    continue;
                const bool on = !checkInitLua || HasInitLua(d.path());
                found.emplace_back(d.path().filename().string(), on);
            }
            std::sort(found.begin(), found.end());
            for (auto& [n, on] : found)
                s_entries.push_back(Entry{ std::format("{}{}", tag, n), on });
        }
    }

    void Init()
    {
        try {
            const auto root = GameRoot();
            if (root.empty()) {
                REX::WARN("LoadOrder: could not locate the game root — inventory omitted.");
                return;
            }
            s_path = root.string();

            // One mod unit per thing a framework actually loads. Counting only
            // .archive missed almost half of this install: 1045 of the 2133 entries
            // in archive/pc/mod are ArchiveXL .xl files, and an .xl with no archive
            // beside it is still a mod — it is how node deletions and record edits
            // ship. r6/tweaks (TweakXL) and r6/input (input_loader) were missing
            // entirely, at 367 and 48 entries.
            CollectFiles(root / "archive" / "pc" / "mod", ".archive", "archive: ");
            CollectFiles(root / "archive" / "pc" / "mod", ".xl", "archiveXL: ");
            CollectFiles(root / "r6" / "tweaks", ".yaml", "tweak: ");
            CollectFiles(root / "r6" / "tweaks", ".yml", "tweak: ");
            CollectFiles(root / "r6" / "tweaks", ".xl", "tweak: ");
            CollectDirs(root / "r6" / "tweaks", "tweak: ", /*checkInitLua*/ false);
            CollectFiles(root / "r6" / "input", ".xml", "input: ");
            CollectDirs(root / "bin" / "x64" / "plugins" / "cyber_engine_tweaks" / "mods",
                        "CET: ", /*checkInitLua*/ true);
            CollectDirs(root / "r6" / "scripts", "reds: ", /*checkInitLua*/ false);

            s_available = true;

            std::size_t off = 0;
            for (const auto& e : s_entries)
                off += e.enabled ? 0 : 1;
            REX::INFO("LoadOrder: {} mod unit(s) under {} ({} inert).",
                      s_entries.size(), s_path, off);
        } catch (const std::exception& e) {
            REX::WARN("LoadOrder: inventory failed: {}", e.what());
        }
    }

    bool Available() { return s_available; }

    std::string SourcePath() { return s_path; }

    const std::vector<Entry>& Entries() { return s_entries; }
}

#include "PCH.h"
#include "AddressLibrary.h"

// -----------------------------------------------------------------------------
// CP2077 address-library decoder.
//
// The database is Ultimate ASI Loader's binary cache of the game's own 132 MB
// bin/x64/cyberpunk2077_addresses.json, kept at
//     bin/x64/plugins/address_library/version-<packed>.bin
//
// It is NOT the Bethesda versionlib format (uint32 tag of 1/2/5) the Starfield
// build parsed. Format, reverse-engineered and validated against the JSON at
// 99.973% over 60 000 records (0 missing; the 16 disagreements are all entries
// whose JSON symbol name is empty — aliases the cache folds together):
//
//   offset 0    int64  magic  = -11
//   offset 8    int64  count  = number of addresses (921876 for 2.31)
//   offset 16   int64  mask   = 0x1FFFFF (slot count 2^21, minus one)
//   offset 24   byte[16]      reserved
//   offset 40   byte[2^21]    secondary index — NOT a control array parallel to
//                             the slots below. Filtering on it drops a third of
//                             the real entries; occupancy is value != 0.
//   offset 40+2^21  uint64[2^21]  slots. Non-zero =>  (RVA << 32) | id32
//
// The id32 is the same decimal "hash" mod authors quote, so an address annotated
// "ID: 887623293+0x2A" is directly searchable and comparable across game versions.
//
// The filename encodes the exe's file version as four 16-bit lanes:
//     (major<<48) | (minor<<32) | (build<<16) | revision
// e.g. 3.0.80.51928 -> 844424935426776. That is checked against the running
// executable, so a database left behind from an older patch is refused rather
// than silently resolving every address to the wrong symbol.
//
// Only the reverse direction (address -> nearest id at or below) is needed, and
// that needs no knowledge of the table's hash function: every slot is enumerated
// once at load, sorted by RVA, and binary-searched.
// -----------------------------------------------------------------------------

namespace AddressLibrary
{
    namespace
    {
        struct Entry
        {
            std::uint32_t rva;
            std::uint32_t id;
        };

        std::vector<Entry> s_entries;      // sorted by rva
        std::string        s_status = "not initialised";
        std::uint64_t      s_gameBase = 0;
        std::uint64_t      s_gameSize = 0;

        // An address further than this past the nearest known one is reported as
        // unresolved rather than attributed to a distant symbol. With ~922k symbols
        // over a ~60 MB image the average gap is well under 100 bytes, so this is
        // generous by orders of magnitude and only suppresses nonsense.
        constexpr std::uint64_t kMaxDisplacement = 0x10000;

        std::string FileVersionOf(const std::filesystem::path& p, std::uint64_t& packed)
        {
            packed = 0;
            const auto s = p.string();
            DWORD dummy = 0;
            const DWORD size = GetFileVersionInfoSizeA(s.c_str(), &dummy);
            if (!size)
                return {};
            std::vector<std::byte> buf(size);
            if (!GetFileVersionInfoA(s.c_str(), 0, size, buf.data()))
                return {};
            VS_FIXEDFILEINFO* ffi = nullptr;
            UINT len = 0;
            if (!VerQueryValueA(buf.data(), "\\", reinterpret_cast<void**>(&ffi), &len) || !ffi)
                return {};
            const std::uint64_t a = HIWORD(ffi->dwFileVersionMS), b = LOWORD(ffi->dwFileVersionMS);
            const std::uint64_t c = HIWORD(ffi->dwFileVersionLS), d = LOWORD(ffi->dwFileVersionLS);
            packed = (a << 48) | (b << 32) | (c << 16) | d;
            return std::format("{}.{}.{}.{}", a, b, c, d);
        }
    }

    void Init()
    {
        try {
            wchar_t exeBuf[MAX_PATH]{};
            if (GetModuleFileNameW(nullptr, exeBuf, MAX_PATH) == 0) {
                s_status = "disabled (cannot locate the game executable)";
                REX::WARN("AddressLibrary: {}", s_status);
                return;
            }
            const std::filesystem::path exe = exeBuf;

            // Base and size of the game image, for RVA arithmetic later.
            MODULEINFO mi{};
            if (GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &mi, sizeof(mi))) {
                s_gameBase = reinterpret_cast<std::uint64_t>(mi.lpBaseOfDll);
                s_gameSize = mi.SizeOfImage;
            }

            std::uint64_t packed = 0;
            const auto verStr = FileVersionOf(exe, packed);
            if (!packed) {
                s_status = "disabled (game file version unreadable)";
                REX::WARN("AddressLibrary: {}", s_status);
                return;
            }

            const auto db = exe.parent_path() / "plugins" / "address_library"
                          / std::format("version-{}.bin", packed);
            std::error_code ec;
            if (!std::filesystem::is_regular_file(db, ec)) {
                s_status = std::format("disabled (no version-{}.bin for game {})", packed, verStr);
                REX::INFO("AddressLibrary: {}", s_status);
                return;
            }

            std::ifstream in(db, std::ios::binary);
            if (!in) {
                s_status = "disabled (database present but unreadable)";
                REX::WARN("AddressLibrary: {}", s_status);
                return;
            }

            std::int64_t magic = 0, count = 0, mask = 0;
            in.read(reinterpret_cast<char*>(&magic), 8);
            in.read(reinterpret_cast<char*>(&count), 8);
            in.read(reinterpret_cast<char*>(&mask), 8);
            if (!in || magic != -11 || mask <= 0 || count <= 0) {
                s_status = std::format("disabled (unrecognised header: magic {}, count {}, mask {})",
                                       magic, count, mask);
                REX::WARN("AddressLibrary: {}", s_status);
                return;
            }

            const std::uint64_t slots = static_cast<std::uint64_t>(mask) + 1;
            if (slots > (1ull << 26)) {           // sanity bound, ~64M slots
                s_status = "disabled (implausible slot count)";
                REX::WARN("AddressLibrary: {}", s_status);
                return;
            }

            // Skip the 16 reserved header bytes and the secondary index.
            in.seekg(40 + static_cast<std::streamoff>(slots), std::ios::beg);

            std::vector<std::uint64_t> raw(slots);
            in.read(reinterpret_cast<char*>(raw.data()),
                    static_cast<std::streamsize>(slots * sizeof(std::uint64_t)));
            if (!in) {
                s_status = "disabled (database truncated)";
                REX::WARN("AddressLibrary: {}", s_status);
                return;
            }

            s_entries.reserve(static_cast<std::size_t>(count) + 16);
            for (const auto v : raw) {
                if (!v)
                    continue;
                s_entries.push_back(Entry{ static_cast<std::uint32_t>(v >> 32),
                                           static_cast<std::uint32_t>(v & 0xFFFFFFFFull) });
            }
            std::sort(s_entries.begin(), s_entries.end(),
                      [](const Entry& a, const Entry& b) { return a.rva < b.rva; });

            s_status = std::format("version-{}.bin — game {} ({} ids)", packed, verStr, s_entries.size());
            REX::INFO("AddressLibrary: loaded {}", s_status);
        } catch (const std::exception& e) {
            s_entries.clear();
            s_status = std::format("disabled (load failed: {})", e.what());
            REX::ERROR("AddressLibrary: {}", s_status);
        }
    }

    bool Available() { return !s_entries.empty(); }

    std::string Status() { return s_status; }

    std::optional<Result> Resolve(std::uint64_t address)
    {
        if (s_entries.empty() || !s_gameBase)
            return std::nullopt;
        if (address < s_gameBase || (s_gameSize && address >= s_gameBase + s_gameSize))
            return std::nullopt;   // not in the game image: a plugin or system DLL

        const auto rva = static_cast<std::uint32_t>(address - s_gameBase);

        // Greatest entry with rva <= target.
        auto it = std::upper_bound(s_entries.begin(), s_entries.end(), rva,
                                   [](std::uint32_t v, const Entry& e) { return v < e.rva; });
        if (it == s_entries.begin())
            return std::nullopt;
        --it;

        const std::uint64_t disp = rva - it->rva;
        if (disp > kMaxDisplacement)
            return std::nullopt;

        return Result{ it->id, disp };
    }
}

// coop/mod_data/legacy_note.cpp -- see coop/mod_data/legacy_note.h.

#include "coop/mod_data/legacy_note.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/paths.h"

#include <atomic>
#include <cstddef>
#include <string>

namespace coop::legacy_note {
namespace {

constexpr size_t kEntries = 8;         // LegacyFile's enumerators
constexpr size_t kNamesPerEntry = 2;   // the identity key and the per-account key; the rest hold one

// Per entry, the hashes of the names already noted (0 = a free slot). A slot is claimed with a
// compare-and-swap, so a name is noted once whichever threads call.
std::atomic<uint32_t> g_noted[kEntries][kNamesPerEntry];

uint32_t HashOf(const wchar_t* name) {
    uint32_t h = 2166136261u;
    for (; *name; ++name) h = (h ^ static_cast<uint32_t>(*name)) * 16777619u;
    return h == 0 ? 1u : h;
}

// True for the call that claims `h` in `file`'s row.
bool Claim(LegacyFile file, uint32_t h) {
    std::atomic<uint32_t>* row = g_noted[static_cast<size_t>(file)];
    for (size_t i = 0; i < kNamesPerEntry; ++i) {
        uint32_t cur = row[i].load(std::memory_order_acquire);
        if (cur == h) return false;
        if (cur == 0 && row[i].compare_exchange_strong(cur, h, std::memory_order_acq_rel)) return true;
        if (cur == h) return false;  // another thread claimed this very name first
    }
    return false;
}

// True once `h` was claimed in `file`'s row: read only, so a name already noted costs two loads.
bool Noted(LegacyFile file, uint32_t h) {
    const std::atomic<uint32_t>* row = g_noted[static_cast<size_t>(file)];
    for (size_t i = 0; i < kNamesPerEntry; ++i)
        if (row[i].load(std::memory_order_acquire) == h) return true;
    return false;
}

}  // namespace

void Once(LegacyFile file, const wchar_t* name) {
    if (!name || !*name || static_cast<size_t>(file) >= kEntries) return;
    const uint32_t h = HashOf(name);
    // Absence is not latched (the file can appear later): only a name already noted skips the stat.
    if (Noted(file, h)) return;
    std::wstring path;
    if (!ue_wrap::paths::ExistsBesideExe(name, &path)) return;
    if (!Claim(file, h)) return;
    if (file == LegacyFile::IdentityKey) {
        UE_LOGW("identity: legacy identity beside the exe (%ls) -- ignoring; a new identity is in "
                "the mod's folder", path.c_str());
        return;
    }
    const std::wstring data = ue_wrap::paths::ModDataDir();
    if (data.empty()) {
        UE_LOGI("legacy %ls beside the exe -- ignoring; the mod has no data folder to hold it", name);
        return;
    }
    UE_LOGI("legacy %ls beside the exe -- ignoring; this data now lives in %ls", name, data.c_str());
}

void WarnIfNoDataFolder() {
    static std::atomic<bool> warned{false};
    if (!ue_wrap::paths::ModDataDir().empty() || warned.exchange(true)) return;
    UE_LOGW("data: the mod's data folder is missing (the identity line names the folder) -- every "
            "setting at its default, nothing saved this session");
}

}  // namespace coop::legacy_note

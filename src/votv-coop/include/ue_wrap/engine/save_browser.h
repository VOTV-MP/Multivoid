// ue_wrap/engine/save_browser.h -- VOTV save enumeration + creation. Engine-wrapper layer
// (principle 7): NO coop, network or gameplay state lives here. GAME THREAD ONLY unless a
// declaration says otherwise.
//
// A save is a folder under <SavedDir>/SaveGames/ (ue_wrap/engine/save_folder.h), listed when it
// holds a main.meta, as the game's slot menu lists it. A row is read off that metadata
// (save_metadata_C: mode, day, version, last saved): a few hundred bytes per save, so the scan reads
// it through the game's own CompressedSaveLoadLib on the game thread, never the world save.
//
// CreateNamedSave is the game's own create: lib_C::createSaveSlotTreeDirectory, which builds the
// folder with its metadata, its main and autosave slots and its persistent save.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::save_browser {

// One existing save, with the metadata a picker row needs (read off its main.meta).
struct SaveInfo {
    std::wstring slot;         // the save's folder name, what engine::LoadStorySave takes
    std::wstring displayName;  // the name the list shows (the folder name)
    int          mode = -1;    // save_metadata_C::gamemode_type, an enum_gamemode ordinal; -1 = unknown
    std::wstring modeLabel;    // "Story" / "Sandbox" / ... (display)
    int          day = 0;      // save_metadata_C::savedtime.Z + 1, as the slot row shows it
    std::wstring version;      // save_metadata_C::version
    bool         versionConflict = false;  // version differs from the running game's (GameVersion):
                                           // the case the game's own slot menu warns about
    int64_t      lastPlayedTicks = 0;  // save_metadata_C::lastSaved (FDateTime ticks, 100ns)
};

// Enumerate all existing top-level saves with metadata (synchronous convenience
// path -- the picker uses RefreshAsync instead). `out` is replaced, sorted
// newest-first. Returns false if the save system can't be resolved yet (the saved
// directory unresolved) -- `out` is then left empty. Game thread.
bool EnumerateSaves(std::vector<SaveInfo>& out);

// Create a brand-new, NAMED, mode-correct, PERSISTED save folder and return its name in
// `outSlot`. `mode` is an enum_gamemode ordinal (story=0), which the game stamps into the
// metadata from the GameInstance's mode. Returns false on a name collision (the folder already
// exists) or if the save system isn't resolvable. Game thread only. The caller enters gameplay
// via the SAME path as any existing save: engine::LoadStorySave(outSlot).
bool CreateNamedSave(const std::wstring& name, uint8_t mode, std::wstring& outSlot);

// The same creation, from a BASE name the caller does not need to have checked: probes
// "<base>", "<base> 2", "<base> 3" ... and creates the first one free.
//
// WHY IT EXISTS. `CreateNamedSave` refusing a taken name is correct -- a caller naming
// an exact slot deserves the truth rather than a silent rename. But the native hosting
// lane has no name field, so its base name is a CONSTANT, and a constant can be created
// exactly once: the second New Game a player ever hosted died on "already exists",
// aborted the boot and dropped them back on the server browser with no world made.
//
// Uniqueness is resolved HERE, against LIVE slots at the moment of creation, rather
// than by a picker filtering its save list: an enumeration a menu is holding can be
// minutes old, and a name that was free when it was CHOSEN is exactly the failure this
// closes. Game thread.
bool CreateNamedSaveUnique(const std::wstring& baseName, uint8_t mode, std::wstring& outSlot);

// True iff a save folder of that name already exists on disk. Game thread only. Used by the
// picker to validate a typed New-Game name live.
bool SlotExists(const std::wstring& slot);

// --- async cache for the render-thread picker (mirrors lobby_client) -------------

// Kick a scan on the game thread; the result is cached for CopySaves. Non-blocking, safe
// from the render thread. Coalesces (no overlapping scans). Call on picker-open / explicit refresh / after
// CreateNamedSave -- not from the per-frame ImGui draw (the coalesce prevents
// OVERLAP, not a re-post-every-frame churn).
void RefreshAsync();

// Copy the cached save list (render thread). Returns a revision counter that bumps
// on each COMPLETED scan, so the UI can detect "new data landed".
uint64_t CopySaves(std::vector<SaveInfo>& out);

// The running game's version as its own slot menu compares it against a save's Version:
// lib_C::gameVersion with an empty prefix and suffix, which is the project version.
// Resolved on the game thread by a scan; empty until one has. Any thread.
std::wstring GameVersion();

// One-line status for the picker footer ("Scanning..." / "N save(s)" / an error).
std::string Status();

}  // namespace ue_wrap::save_browser

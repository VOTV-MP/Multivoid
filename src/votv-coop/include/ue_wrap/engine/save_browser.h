// ue_wrap/engine/save_browser.h -- VOTV save enumeration + creation. Engine-wrapper layer
// (principle 7): NO coop, network or gameplay state lives here. The UFunction-driven
// parts -- create, exists, and the scan's stage A -- are GAME THREAD ONLY.
//
// EnumerateSaves lists <SavedDir>/SaveGames/*.sav -- directory from the native
// GetProjectSavedDirectory, subsaves excluded through lib_C::processSaveNameIntoSubsave
// -- and reads each row's metadata DIRECTLY from the file's GVAS tag stream
// (ue_wrap/gvas_meta), taking saveSlot_C CDO defaults for delta-omitted properties.
// Driving loadSlots instead means N calls to LoadGameFromSlot, a full 15-20 MB
// deserialize per save on the game thread, which froze the picker for seconds once
// real saves accumulated. Non-saveSlot_C slots and subsaves are excluded; b_* is the
// SANDBOX prefix and is never filtered.
//
// CreateNamedSave mirrors VOTV's create primitive: CreateSaveGameObject(saveSlot_C)
// + SaveGameToSlot(obj, "<prefix><name>", 0), a persisted-at-creation blank save.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::save_browser {

// One existing save, with the metadata a picker row needs (read off UsaveSlot_C).
struct SaveInfo {
    std::wstring slot;         // slot name (== the <slot>.sav filename minus extension)
    std::wstring displayName;  // slot minus the mode prefix (for the list)
    int          mode = -1;    // enum_gamemode ordinal (story=0, ...); -1 = unknown
    std::wstring modeLabel;    // "Story" / "Sandbox" / "Infinite" / ... (display)
    int          day = 0;      // UsaveSlot_C::Day
    int          points = 0;   // UsaveSlot_C::Points
    float        health = 0.f; // UsaveSlot_C::health
    float        maxHealth = 0.f;  // UsaveSlot_C::maxHealth
    std::wstring version;      // UsaveSlot_C::Version
    int64_t      lastPlayedTicks = 0;  // UsaveSlot_C::lastDate (FDateTime ticks, 100ns)
};

// Enumerate all existing top-level saves with metadata (synchronous convenience
// path -- the picker uses RefreshAsync instead). `out` is replaced, sorted
// newest-first. Returns false if the save system can't be resolved yet (saveSlot_C
// not loaded / dir unresolved) -- `out` is then left empty. Game thread.
bool EnumerateSaves(std::vector<SaveInfo>& out);

// Create a brand-new, NAMED, mode-correct, PERSISTED save and return its full slot
// name ("<prefix><name>") in `outSlot`. `mode` is an enum_gamemode ordinal (story=0).
// Returns false on name collision (slot already exists) or if the save system isn't
// resolvable. Game thread only. The caller enters gameplay via the SAME path as any
// existing save: engine::LoadStorySave(outSlot).
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

// True iff a save slot already exists on disk (UGameplayStatics::DoesSaveGameExist).
// Game thread only. Used by the picker to validate a typed New-Game name live.
bool SlotExists(const std::wstring& slot);

// --- async cache for the render-thread picker (mirrors lobby_client) -------------

// Kick a scan: stage A (dir/classify/CDO defaults) on the game thread, stage B
// (per-file GVAS metadata reads, mtime-cached) on a worker thread; the result is
// cached for CopySaves. Non-blocking, safe from the render thread. Coalesces (no
// overlapping scans). Call on picker-open / explicit refresh / after
// CreateNamedSave -- not from the per-frame ImGui draw (the coalesce prevents
// OVERLAP, not a re-post-every-frame churn).
void RefreshAsync();

// Copy the cached save list (render thread). Returns a revision counter that bumps
// on each COMPLETED scan, so the UI can detect "new data landed".
uint64_t CopySaves(std::vector<SaveInfo>& out);

// One-line status for the picker footer ("Scanning..." / "N save(s)" / an error).
std::string Status();

}  // namespace ue_wrap::save_browser

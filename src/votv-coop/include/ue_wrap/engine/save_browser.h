// ue_wrap/save_browser.h -- VOTV save enumeration + creation.
//
// Engine-wrapper layer (principle 7): NO coop / network / gameplay state lives here.
//   - EnumerateSaves (2026-07-11 rework): lists <SavedDir>/SaveGames/*.sav (dir via
//     the native GetProjectSavedDirectory; subsaves excluded via VOTV's own
//     lib_C::processSaveNameIntoSubsave) and reads each row's metadata DIRECTLY from
//     the file's GVAS tag stream (ue_wrap/gvas_meta) with saveSlot_C CDO defaults for
//     delta-omitted properties. The prior loadSlots drive did N x LoadGameFromSlot --
//     a full 15-20 MB deserialize per save ON THE GAME THREAD = a multi-second
//     picker-open freeze once real saves accumulated. Filter parity ground-truthed
//     against the native list 2026-07-11 (subsaves + non-saveSlot_C excluded; b_* is
//     the SANDBOX prefix, never filtered).
//   - CreateNamedSave mirrors VOTV's create primitive: CreateSaveGameObject(saveSlot_C)
//     + SaveGameToSlot(obj, "<prefix><name>", 0) -> a persisted-at-creation blank save.
//
// The ImGui Host-Game save picker (ui/host_save_picker) sits on top of this. The
// UFunction-driven parts (create/exists + the scan's stage A) are GAME THREAD ONLY.
// The async RefreshAsync/CopySaves/Status trio lets the render-thread picker trigger
// a scan + read a cached snapshot without blocking (mirrors coop/net/lobby_client).
//
// RE: research/findings/saves/votv-save-picker-{enumerate-load,create-new}-RE-2026-06-06.md.

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

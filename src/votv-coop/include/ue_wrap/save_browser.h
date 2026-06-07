// ue_wrap/save_browser.h -- native VOTV save enumeration + creation.
//
// Engine-wrapper layer (principle 7): drives VOTV's OWN save functions and reads
// UsaveSlot_C metadata. NO coop / network / gameplay state lives here.
//   - EnumerateSaves drives Uui_saveSlots_C::loadSlots (VOTV's own list builder) and
//     harvests its result arrays -- so the picker shows EXACTLY the saves VOTV's menu
//     would, with VOTV's own filtering, and the engine owns/frees loadSlots' transient
//     allocations (no manual TArray<FString> free on our side). RULE 2026-05-28
//     (follow the native shape) + RULE 1 (leak-free by construction).
//   - CreateNamedSave mirrors VOTV's create primitive: CreateSaveGameObject(saveSlot_C)
//     + SaveGameToSlot(obj, "<prefix><name>", 0) -> a persisted-at-creation blank save.
//
// The ImGui Host-Game save picker (ui/host_save_picker) sits on top of this. All the
// enumerate/create calls invoke UFunctions -> GAME THREAD ONLY. The async
// RefreshAsync/CopySaves/Status trio lets the render-thread picker trigger a scan +
// read a cached snapshot without blocking (mirrors coop/net/lobby_client's pattern).
//
// RE: research/findings/votv-save-picker-{enumerate-load,create-new}-RE-2026-06-06.md.

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

// Enumerate all existing top-level saves with metadata by driving VOTV's native
// loadSlots. `out` is replaced. Returns false if the save system can't be resolved
// (the save-slots widget isn't loaded yet) -- `out` is then left empty. Game thread.
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

// Post an EnumerateSaves onto the game thread; the result is cached for CopySaves.
// Non-blocking, safe from the render thread. Coalesces (no overlapping scans).
//
// CALL ON PICKER-OPEN / an explicit refresh / after CreateNamedSave ONLY -- NEVER from
// the per-frame ImGui draw. Each scan drives VOTV's loadSlots = N x LoadGameFromSlot,
// each parsing a .sav up to ~19 MB synchronously on the game thread. The internal
// coalesce prevents OVERLAPPING scans but NOT a re-post-every-frame hitch storm.
void RefreshAsync();

// Copy the cached save list (render thread). Returns a revision counter that bumps
// on each COMPLETED scan, so the UI can detect "new data landed".
uint64_t CopySaves(std::vector<SaveInfo>& out);

// One-line status for the picker footer ("Scanning..." / "N save(s)" / an error).
std::string Status();

}  // namespace ue_wrap::save_browser

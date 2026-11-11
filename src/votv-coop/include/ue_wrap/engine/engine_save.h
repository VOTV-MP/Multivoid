// ue_wrap/engine/engine_save.h -- the game's own save and travel verbs: loading a slot through its
// entry point, the fresh-game path, and the hook that fires when a save object is ready.
// Engine-wrapper layer (principle 7): each call marshals one UFunction call or one reflected field
// access, with no gameplay, network or coop state. Game thread unless a declaration says
// otherwise. Implementation: src/ue_wrap/engine/engine_save.cpp.

#pragma once

#include <string>
#include <cstdint>
#include <utility>
#include <vector>

namespace ue_wrap::engine {

// AmainGamemode_C::transition("/Game/menu"), the game's own travel verb (the short name does not
// resolve); needs no pause and works for a dead player. Hold game_thread::SetTransparentBypass over
// the travel so the detour does not stall the world teardown. Game thread.
bool ReturnToMainMenu();

// Load save folder `folderName` (SaveGames/<folderName>/) the way the game's slot menu does:
// main.sav, persistent.sav and main.meta through CompressedSaveLoadLib, setSaveSlotObject, an area
// slot swapped in when the metadata names one, the save's day, rules and mode put in force, then
// the starter map (or the area's level) opened with loadObjectsFromSlot set. `forceGameMode` >= 0
// writes that enum_gamemode ordinal instead of the metadata's gamemode_type. False until the player
// is in the loaded world, and while a file is missing or the load cannot dispatch. Game thread.
bool LoadStorySave(const wchar_t* folderName, int forceGameMode = -1);

// Drop the cached save objects so a second in-process LoadStorySave re-loads from disk. Game
// thread.
void ResetCachedSave();

// A fresh New Game in `gameMode` (an enum_gamemode ordinal): LoadStorySave with a blank saveSlot_C
// and save_metadata_C (CreateSaveGameObject) registered under no slot name, so the game's own save
// never writes it; a fresh client world holds only level-default props, so the host's snapshot
// mirrors onto it with nothing to reconcile away. Game thread.
bool StartFreshGame(int gameMode);

// The rules the next StartFreshGame writes into its blank metadata's rules before they are put in
// force: (game_rules key, value text) pairs, as game_rules::WriteSavedRule takes them. Consumed by
// the one fresh game that call creates, then cleared; a save loaded from a folder is never touched.
// Set before the call, from the thread that drives the boot; empty clears.
void SetFreshGameRules(const std::vector<std::pair<std::string, std::string>>& rules);

// ---- Save-object-ready hook ----
// Fires once per LoadStorySave / StartFreshGame, on the game thread, with the USaveGame* about to
// be registered: the inventory arrays are present and the world not yet built from them, so the
// game's own load builds the live inventory from what the hook leaves. engine.cpp fires it
// unconditionally; the hook is a no-op unless an inventory is pending. Null disarms.
using SaveObjectReadyHook = void(*)(void* saveSlotObject);

void SetSaveObjectReadyHook(SaveObjectReadyHook hook);

}  // namespace ue_wrap::engine

// ue_wrap/engine/engine_save.h -- the game's own save and travel verbs: loading a slot through its
// entry point, the fresh-game path, and the hook that fires when a save object is ready.
// Engine-wrapper layer (principle 7): each call marshals one UFunction call or one reflected field
// access, with no gameplay, network or coop state. Game thread unless a declaration says
// otherwise. Implementation: src/ue_wrap/engine/engine_save.cpp.

#pragma once

#include <string>
#include <cstdint>

namespace ue_wrap::engine {

// AmainGamemode_C::transition("/Game/menu"), the game's own travel verb (the short name does not
// resolve); needs no pause and works for a dead player. Hold game_thread::SetTransparentBypass over
// the travel so the detour does not stall the world teardown. Game thread.
bool ReturnToMainMenu();

// Load a save slot through the game's own entry (LoadGameFromSlot, setSaveSlotObject with
// loadObjects=true, then untitled_1; the save selects the mode). `forceGameMode` >= 0 writes that
// enum_gamemode ordinal instead of deriving it from the slot-name prefix, which the coop slot
// `zcoop_<pid>` lacks. False if the slot is missing or the load cannot dispatch. Game thread.
bool LoadStorySave(const wchar_t* slot, int forceGameMode = -1);

// Drop the cached USaveGame* and the GameMode latch so a second in-process LoadStorySave re-loads
// from disk. Game thread.
void ResetCachedSave();

// A fresh New Game: LoadStorySave with a blank save object (CreateSaveGameObject(saveSlot_C)); a
// fresh client world holds only level-default props, so the host's snapshot mirrors onto it with
// nothing to reconcile away. Game thread.
bool StartFreshGame(bool storyMode);

// ---- Save-object-ready hook ----
// Fires once per LoadStorySave / StartFreshGame, on the game thread, with the USaveGame* about to
// be registered: the inventory arrays are present and the world not yet built from them, so the
// game's own load builds the live inventory from what the hook leaves. engine.cpp fires it
// unconditionally; the hook is a no-op unless an inventory is pending. Null disarms.
using SaveObjectReadyHook = void(*)(void* saveSlotObject);

void SetSaveObjectReadyHook(SaveObjectReadyHook hook);

// The game encodes a save's mode only in the slot-name prefix (story "s_", infinite "i_", sandbox
// "b_", halloween "SPOOKY_", ambience "a_", solar "l_"); both functions wrap
// Uui_saveSlots_C::getSavePrefix on the CDO, which resolves lazily (false / -1 until the menu or a
// gameplay transition loads it). enum_gamemode ordinals are not the submenu order (story=0,
// infinite=1, sandbox=4). Game thread.

// The prefix for `mode` (a TEnumAsByte<enum_gamemode::Type>) into `out`.
bool GetSavePrefix(uint8_t mode, std::wstring& out);

// The ordinal of `slot` from its prefix (the longest match wins): 0..7, or -1.
int DeriveModeFromSlot(const wchar_t* slot);

}  // namespace ue_wrap::engine

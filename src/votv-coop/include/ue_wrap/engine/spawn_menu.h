// ue_wrap/spawn_menu.h -- open VOTV's sandbox prop-spawn menu (the Q-key menu).
//
// Engine-wrapper layer (principle 7). NO gameplay/network/coop state lives here;
// this just reproduces the engine path the Q key fires. The dev toggle that
// decides WHEN to open it (client-lock, ini flag) is coop/dev/spawn_menu_unlock.
//
// RE ground truth (research/findings/world-systems/votv-spawnmenu-storymode-RE-2026-06-14.md):
// the Q key fires mainPlayer's UFunction
//   InpActEvt_spawnmenu_K2Node_InputActionEvent_2(FKey Key)   [the PRESSED edge]
// which forwards into ExecuteUbergraph_mainPlayer @12077. That open block has NO
// enum_gamemode gate -- its ONLY guards are IsValid(activeInterface) (don't open
// over another UI) and lib.isBuoyant (don't open while swimming). The spawnmenu
// widget + propRenderer exist in every gamemode (created unconditionally at
// propProcessor startup). So opening the menu in STORY mode is exactly: invoke
// that same UFunction. No asset edit (RULE 3), no GameMode flip (which would
// perturb weapons/flight/backrooms/save-paths -- those branch on gamemode==4).
//
// Why call the input UFunction rather than re-create the widget ourselves:
// it reuses the game's own open logic verbatim (SetVisibility + SetInputMode_
// GameAndUIEx + Button_search + spawnmenu.opened()) INCLUDING the activeInterface
// / isBuoyant guards, so the menu behaves identically to a real Q press and can
// never double-spawn the (already-existing) widget.

#pragma once

namespace ue_wrap::spawn_menu {

// Open the prop-spawn menu exactly as pressing Q does in sandbox, for the given local
// mainPlayer_C pawn (`localPlayer` -- the caller injects it; this engine layer does not
// resolve the local player, per principle 7). Resolves + caches the widget / UFunctions
// on first call. Returns false (and logs) if `localPlayer` is null, or the widget can't
// be resolved. GAME THREAD ONLY (drives ProcessEvent + UMG + input mode). Subject to the
// game's own guards: a no-op if another interface is already open or the player is
// swimming (matching vanilla Q).
bool Open(void* localPlayer);

// Close the spawn menu for `localPlayer`: the INVERSE of Open(). Restores
// SetInputMode_GameOnly + hides the mouse cursor (the load-bearing un-stick -- Open()
// leaves the player in GameAndUI/cursor mode, so without a close the player can no longer
// interact with the world), then collapses the widget + runs its closed() teardown if
// present. Reproduces vanilla's close block (ExecuteUbergraph_mainPlayer @11555) natively.
// Idempotent + safe to call when already closed. GAME THREAD ONLY.
bool Close(void* localPlayer);

// Toggle open<->closed from GROUND TRUTH (the widget's live Visibility byte), so the
// Q key both opens AND dismisses the menu and a player can never be stranded in the
// open state. Self-correcting if the game closed the menu via its own path. The dev
// Q-key watcher calls this and injects `localPlayer`. GAME THREAD ONLY.
bool Toggle(void* localPlayer);

}  // namespace ue_wrap::spawn_menu

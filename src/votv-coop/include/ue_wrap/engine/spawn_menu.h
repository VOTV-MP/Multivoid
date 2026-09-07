// ue_wrap/engine/spawn_menu.h -- open VOTV's sandbox prop-spawn menu, the Q-key menu. The
// engine-wrapper layer (principle 7): no gameplay, network or coop state. The dev toggle deciding
// WHEN to open it lives in coop/dev/spawn_menu_unlock.
//
// The game's own Q handler cannot be replayed in story mode. Its open block is guarded on two
// things: activeInterface must be null, so the menu never stacks over another UI, and
// lib_C::isBuoyant must return true. That one is not what its name suggests -- it is a capability
// check, answering `hasWeapon` normally and, when the gamemode is flying, comparing a key file on
// disk against a derived string. In story mode it is false, so the block bails -- and one branch of
// it quits the game outright, which is reason enough not to drive it.
//
// So this opens the menu itself, on the widget the game already created: honour the activeInterface
// guard, set the widget visible through the NATIVE UWidget::SetVisibility, run the widget's own
// opened(), then SetInputMode_GameAndUIEx and show the cursor so it is clickable. No asset edit
// (RULE 3), and no gamemode flip -- that branch drives weapons, flight, the backrooms, save paths.

#pragma once

namespace ue_wrap::spawn_menu {

// Open the prop-spawn menu for the given local mainPlayer_C pawn (`localPlayer` -- the caller
// injects it; this engine layer does not resolve the local player, per principle 7). Resolves
// and caches the widget and UFunctions on first call. Returns false, and logs, if `localPlayer`
// is null, if the widget cannot be resolved, or if another interface is already open -- the one
// vanilla guard this reproduces. GAME THREAD ONLY (drives ProcessEvent, UMG and the input
// mode).
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

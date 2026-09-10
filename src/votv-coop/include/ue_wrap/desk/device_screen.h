// ue_wrap/desk/device_screen.h -- standalone engine access for the ENTERABLE screen devices (base
// computers and terminals) and the mainPlayer interface fields that gate them. Principle-7
// engine-wrapper layer: class resolves, the activeInterface discriminator, the aim-field
// clear/restore primitive, and the reflected force-exit. NO network logic -- coop::device_occupancy
// drives the busy/deny mirror through here.
//
// Deny granularity is the WHOLE device actor. Component-level gating (the desk coords cluster
// alone, the radar panel as against its alarm lever) waits on the lookAtComponent versus
// HitResult.Component equivalence being validated hands-on.

#pragma once

#include <string>

namespace ue_wrap::device_screen {

// Resolve the mainPlayer interface offsets and setActiveInterface, and latch once they are in.
// The 7 widget and 8 device CLASSES are deliberately not resolved here: each fills from a live
// instance's ClassOf at the interaction edge, which is walk-free and safe for a device bought
// late, since you cannot aim at or enter one that does not exist. Polling for them instead
// name-walked the whole object array every two seconds forever for anything unbought.
// Idempotent; game thread.
//
// The enterable census is CLOSED at 8 device assets. Five of them show ONE SHARED widget instance
// each -- the desk-coords atlas, the SAT console, the radar, the reactor, and the gamemode.laptop
// ui_laptop that the base laptop shares with every portable PC -- so their claim is per-WIDGET, a
// single key. Transformer panels and arcade props carry per-instance widgets, and get per-instance
// keys from the quantized-position identity, the same shape the turbine PosKey uses.
bool EnsureResolved();

// The inside-a-device discriminator: mainPlayer.activeInterface, null when not inside any screen.
// Null player or unresolved offset -> null. This is a POLL and not a hook because every enter and
// exit call the game makes is an EX_LocalVirtualFunction, which ProcessEvent never sees.
void* ReadActiveInterface(void* player);

// Classify an ACTIVE INTERFACE widget into its cross-peer claim key:
//   ui_consolesAtlas_C -> "desk"   (the 4-screen main desk; coords is the
//                                   only enterable screen of it)
//   ui_console_C       -> "sat"    (ALL SAT consoles share this one widget)
//   ui_radar_C         -> "radar"
//   ui_reactor_C       -> "reactor"
//   ui_laptop_C        -> "laptop" (base laptop + every portable PC -- ONE
//                                   shared widget = ONE claim, REQUIRED: two
//                                   peers in "different" laptops would type
//                                   into the same screen)
//   uiwindow_transformerScreens_C -> "tfm_<posKey of the owning panel>"
//   ui_arcade_invaders_C          -> "arc_<posKey of the owning prop>"
// Empty for any other widget (clipboard / prop inventory / draw paper /
// texture picker ride the same plumbing but are NOT devices).
std::wstring ClassifyWidgetClaimKey(void* widget);

// Classify an AIMED ACTOR into the claim key its E-press would enter (the
// deny-gate side): analogDScreenTest_C -> "desk", panel_SATconsole_C ->
// "sat", panel_radar_C -> "radar", panel_reactor_C -> "reactor", laptop_C /
// prop_portablePc_C -> "laptop", transformerMGPanel_C -> "tfm_<posKey>",
// prop_arcade_C -> "arc_<posKey>". Empty for non-device actors.
std::wstring ClassifyDeviceActorClaimKey(void* actor);

// Null the local player's aim fields (lookAtActor + HitResult.Actor weakptr) for the CURRENT
// InpActEvt_use dispatch, so the native enter chain no-ops on its own icast guards, remembering the
// prior values. That engine-dispatched PRE is the deny seam, the same shape the door's HostAuth
// gate uses. One slot -- a PRE/POST pair within one game-thread dispatch. Returns false if the
// offsets are unresolved, meaning nothing was cleared.
bool ClearAimForDispatch(void* player);

// Restore the fields ClearAimForDispatch saved. Safe to call when nothing is cleared (no-op).
// Every POST and early-exit path must call this FIRST, or the clear leaks past its dispatch
// and the player can aim at nothing.
void RestoreAim();

// True while a ClearAimForDispatch save is outstanding (leak-heal check).
bool HasClearedAim();

// Force the local player OUT of its active interface: reflected
// setActiveInterface(null, ...) -- the game's own forced-exit path
// (ragdollMode uses it; restores input mode, cursor, FOV, movement and
// BROADCASTs exitInterface). Game thread. Returns false if the function or
// player is unresolved.
bool ForceExitInterface(void* player);

}  // namespace ue_wrap::device_screen

// ue_wrap/device_screen.h -- standalone engine access for the ENTERABLE
// screen devices (base computers / terminals) and the mainPlayer interface
// fields that gate them. Principle-7 engine-wrapper layer: class resolves,
// the activeInterface discriminator, the aim-field clear/restore primitive,
// and the reflected force-exit. NO network logic -- coop::device_occupancy
// drives the busy/deny mirror through here.
//
// RE (votv-base-computers-RE-2026-06-11.md): the enterable census is CLOSED
// at 8 device assets; every enter/exit call is EX_LocalVirtualFunction
// (ProcessEvent-INVISIBLE), so detection is polling
// `mainPlayer.activeInterface @0x07E0` (null = not inside) and the deny seam
// is the engine-dispatched `InpActEvt_use` PRE (the door HostAuth precedent:
// null the aim fields for one dispatch and the whole native enter chain
// no-ops on its own icast guards). Five devices show ONE SHARED widget
// instance each (desk-coords atlas / SAT console / radar / reactor / the
// gamemode.laptop ui_laptop shared by the base laptop AND every portable
// PC) -- their claim is per-WIDGET, a single key; transformer panels and
// arcade props carry per-instance widgets -- per-instance keys via the
// quantized-position identity (the turbine PosKey shape).
//
// v1 deny granularity: the WHOLE device actor (the RE doc's sanctioned v1
// simplification). Component-level gating (desk coords cluster only, radar
// panel-vs-alarm-lever) is the documented refinement once the
// lookAtComponent-vs-HitResult.Component equivalence is validated hands-on.

#pragma once

#include <string>

namespace ue_wrap::device_screen {

// Resolve the mainPlayer interface offsets + the 7 enterable widget classes +
// the 8 device actor classes + setActiveInterface. Operational once the
// player offsets and AT LEAST the player class resolve (widget/device classes
// keep lazily resolving -- several only load with their sublevel). Idempotent;
// game thread.
bool EnsureResolved();

// The inside-a-device discriminator: mainPlayer.activeInterface (null when
// not inside any screen). Null player / unresolved offset -> null.
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

// Null the local player's aim fields (lookAtActor + HitResult.Actor weakptr)
// for the CURRENT InpActEvt_use dispatch so the native enter chain no-ops on
// its own icast guards, remembering the prior values. One slot -- PRE/POST
// pair within one game-thread dispatch (the door Active-gate shape).
// Returns false if the offsets are unresolved (nothing cleared).
bool ClearAimForDispatch(void* player);

// Restore the fields ClearAimForDispatch saved. Safe to call when nothing is
// cleared (no-op). Every POST/early-exit path must call this FIRST (the door
// leak lesson, audit IMP-3).
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

// coop/prop_sound.h -- world-space prop interaction sounds for REMOTE players' grabs and
// throws (receiver-side synthesis).
//
// The native cues run ONLY inside the local actor's input chain, so a remote peer never hears
// them: the E-grab click and the throw whoosh are PlaySound2D and actor-only by the game's own
// design, and prop_C.thrown() is a no-op on the base class (uber @465 POP->ret), so driving it
// alone produces no audio at all. The receiver therefore plays the SAME cooked cues at the
// synced edges -- the flashlight-click shape, and MTA's precedent that remote-entity sounds are
// the game's own cues played locally at synced events. Each declaration below names its cue,
// the native volume and pitch, and where the bytecode carries it.
//
// One deliberate divergence: the whoosh is spatialized with att_default rather than played 2D,
// because it is a "look at me" event. Every LMB throw plays `swing` regardless of prop type, so
// one whoosh implementation covers Aprop_C and clumps alike. Game thread only (reflection +
// PlaySoundAtLocation).

#pragma once

#include "ue_wrap/core/types.h"

namespace coop::prop_sound {

// The FIXED E-action click (`use`, natively 2D grabber-only at 0.25)
// spatialized at the grabbed prop -- the dominant "I grabbed something"
// feedback every grab plays. Pairs with PlayGrabSound at the GRAB-IN edge.
void PlayUseClick(void* propActor);

// The grabbed prop's per-material pickup cue at the prop's location.
// prop_C reads its cached physSoundData; plain-Actor grabs (the clump)
// resolve root material -> physmat -> the same physSound row. Silent when
// the material has no soft row (native parity).
void PlayGrabSound(void* propActor);

// The native throw whoosh (swing, vol 0.8 / pitch 1.05), spatialized at the
// released prop. Caller applies the throw-vs-drop speed gate.
void PlayThrowWhoosh(void* propActor);

// The material IMPACT "thud" a trash pile makes when it lands / re-piles, spatialized at the
// landed pile (vol 1.0 / pitch 1.0 / att_default -- the flesh_impact PlaySoundAtLocation params
// the chipPile uses for its own impact reaction). The chipPile/clump BP plays NO dedicated land
// sound (shovelDig_Cue is the recycle-to-scrap action, flesh_impact_Cue a damage reaction): the
// native "land thud" is the physics-material IMPACT cue, since lib_C::physSound(physmat)
// returns {step, impact, soft} and this reads the `impact` row -- the sibling of the `soft` row
// PlayGrabSound already uses. Silent when the material has no impact row (native parity, same as
// the grab soft-miss). Receiver side of the host-authoritative kToPile LAND convert, a genuine
// clump->pile edge only and never an idempotent echo.
void PlayLandSound(void* propActor);

// The inventory-collect BLIP (inventory_Cue -- natively PlaySound2D, 2D and
// collector-only) spatialized at a REMOTE collector's broadcast position
// (vol 1.0 / pitch 1.1, the native @659 values; att_default). Receiver side
// of ReliableKind::InventoryPickup (coop/inventory_pickup_sync). `worldCtx`
// = any live actor in the world (typically the local player).
void PlayInventoryBlipAt(void* worldCtx, const ue_wrap::FVector& loc);

// The game's own save-denied/failed click (button_keypad_deny -- the desk play-screen deny
// @23927 and the keypad deny, vol 0.5 / pitch 1.0), played at the LOCAL player whose action was
// denied, which today is the device-occupancy busy deny. `playerActor` = the denied local
// mainPlayer_C.
void PlayDenyClick(void* playerActor);

}  // namespace coop::prop_sound

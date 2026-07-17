// coop/prop_sound.h -- world-space prop interaction sounds for REMOTE
// players' grabs and throws (receiver-side synthesis).
//
// WHY (hands-on 2026-06-11 "object pick up / throw -- no sounds from puppet"
// + research/findings/player-puppet/votv-puppet-sounds-RE-2026-06-11.md): the native
// sounds run ONLY inside the local actor's input chain, so a remote peer
// never hears them:
//   E-GRAB CLICK (hands-on round 2, "always the same unique single sound"):
//     useAction plays the FIXED SoundWave `use` via PlaySound2D vol 0.25 /
//     pitch 1.0 right after pickupObject (@2181 + every successful E branch)
//     -- 2D, grabber-only. THE dominant grab feedback.
//   MATERIAL SOFT (the secondary grab-variant thud): the grabbed prop's
//     per-material "soft" cue (physSoundData.soft_30, lazily cached from the
//     lib_C::physSound DataTable) at the grab point, vol 0.5 / pitch 1.0 /
//     att_default (mainPlayer uber @100337) -- local input chain only.
//   THROW: the LMB throw plays SoundWave /Game/audio/effects/swing via
//     PlaySound2D vol 0.8 / pitch 1.05 (@16057 grabbed / @16826 held) --
//     2D + thrower-only BY NATIVE DESIGN (other players never hear a throw
//     even in principle). prop_C.thrown() is a no-op on the base class
//     (uber @465 POP->ret), so DrivePropThrown alone produces NO audio.
//
// The receiver therefore plays the SAME cooked cues at the synced edges
// (the flashlight-click shape -- MTA precedent: remote-entity sounds are
// the game's own cues played locally at synced events):
//   PlayGrabSound  -- GRAB-IN edge: the prop's own soft_30 cue at the prop.
//                     Null cue -> silent (native parity with the @100067
//                     DataTable row-miss gate). Non-Aprop_C (clump) -> silent.
//   PlayThrowWhoosh - GRAB-OUT edge past the throw speed gate: swing at the
//                     released prop, spatialized with att_default (an
//                     IMPROVEMENT over native 2D -- intentional, the whoosh
//                     is a "look at me" event).
//
// Supersedes coop/clump_throw_sound (RULE 2): that module played a GUESSED
// asset (object_throw) for the clump only; the byte-exact RE proves every
// LMB throw plays `swing` regardless of prop type, so ONE whoosh
// implementation covers Aprop_C and clumps alike.
//
// Game thread only (reflection + PlaySoundAtLocation).

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

// The material IMPACT "thud" a trash pile makes when it lands / re-piles,
// spatialized at the landed pile (vol 1.0 / pitch 1.0 / att_default -- the
// flesh_impact PlaySoundAtLocation params the chipPile uses for its own impact
// reaction). RE (2026-07-01, docs/piles/re-artifacts): the chipPile/clump BP
// plays NO dedicated land sound (shovelDig_Cue = the recycle-to-scrap action;
// flesh_impact_Cue = a damage/hit reaction) -- the native "land thud" is the
// physics-material IMPACT cue: lib_C::physSound(physmat) returns
// {step, impact, soft}, and this reads the `impact` row -- the sibling of the
// `soft` row PlayGrabSound already uses. Silent when the material has no impact
// row (native parity, same as the grab soft-miss). Receiver side of the
// host-authoritative kToPile LAND convert (a genuine clump->pile edge only, not
// an idempotent echo).
void PlayLandSound(void* propActor);

// The inventory-collect BLIP (inventory_Cue -- natively PlaySound2D, 2D and
// collector-only) spatialized at a REMOTE collector's broadcast position
// (vol 1.0 / pitch 1.1, the native @659 values; att_default). Receiver side
// of ReliableKind::InventoryPickup (coop/inventory_pickup_sync). `worldCtx`
// = any live actor in the world (typically the local player).
void PlayInventoryBlipAt(void* worldCtx, const ue_wrap::FVector& loc);

// The game's own save-denied/failed click (button_keypad_deny, the desk
// play-screen deny @23927 + the keypad deny -- vol 0.5 / pitch 1.0), played
// at the LOCAL player whose action was denied (v63 device-occupancy busy
// deny). `playerActor` = the denied local mainPlayer_C.
void PlayDenyClick(void* playerActor);

}  // namespace coop::prop_sound

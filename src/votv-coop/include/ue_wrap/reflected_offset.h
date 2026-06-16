// ue_wrap/reflected_offset.h -- reflection-resolved BP property offsets.
//
// VOTV's BP-cooked classes recompile every patch; struct layouts shift
// when a new property is added or one is reordered. Hardcoded offsets in
// sdk_profile.h are the version-coupled surface — when the VOTV update
// lands, every hardcoded `+ P::off::mainPlayer_grabbing_actor` is a
// landmine.
//
// THIS HEADER is the resilience layer for BP-cooked properties: each
// accessor below resolves its (class, field) pair via reflection at
// first use and caches the result. As long as VOTV doesn't rename the
// field, the offset auto-adapts to layout drift across patches.
//
// Returns -1 if the class hasn't loaded yet OR the field was renamed.
// Callers SHOULD null-check the result before using as a pointer offset.
//
// Re-resolution on failure: the cache memorizes ONLY on success. If a
// call fires before the BP class is loaded, the next call retries — so
// late-loading BP content (loaded on first gameplay-level transition)
// still gets resolved without the call site having to retry.
//
// Engine-stable offsets (UObject internals, FProperty/FField chain
// fields, USceneComponent::AttachParent, etc.) stay hardcoded in
// sdk_profile.h — they don't shift across BP recooks. Only the
// BP-cooked surface (mainPlayer_C, AnimBP_*) lives here.

#pragma once

#include <cstdint>

namespace ue_wrap::reflected_offset {

// mainPlayer_C field accessors (VOTV BP -- recook-volatile).
int32_t MainPlayer_heavyGrab();
int32_t MainPlayer_grabHandle();
int32_t MainPlayer_grabTimeline();
int32_t MainPlayer_grabbing_actor();
int32_t MainPlayer_grabbing_component();
int32_t MainPlayer_grabsHeavy();
int32_t MainPlayer_grabLen();
int32_t MainPlayer_Heavy();
// holding_actor is the non-PhysicsHandle "held" pointer used by chipPile/
// clump morph (toClump() writes the spawned clump address here, NOT
// grabbing_actor; their pickup path doesn't go through UPhysicsHandle).
// Probe-confirmed 2026-05-27; the PropPose-emit branch reads this as a
// fallback when grabbing_actor is null.
int32_t MainPlayer_holding_actor();
// The actor the local player is currently aiming at (lookAtActor @0x0AA0). On an
// E-press (InpActEvt_use) this is the door/interactable being used -- read it in the
// proven-firing InpActEvt_use observer to identify the door for host-authoritative sync.
int32_t MainPlayer_lookAtActor();
// Radial-menu confirm fields (kerfur menu verb detection on the client InpActEvt_use seam).
int32_t MainPlayer_releaseEToUse();  // 0x0E88: the "release E to use" radial confirm flag
int32_t MainPlayer_actionIndex();    // 0x0A98: the highlighted radial-menu option index
// Ragdoll/faint DISPLAY state (vitals pillar Inc2b, 2026-05-31). isRagdoll is
// the AnimBP gate flipped by ragdollMode() -- set by ANY ragdoll cause (manual
// C-key InpActEvt_ragdoll_..._25, exhaustion faint(), KO). `dead` is the death
// bool; the sender excludes death-ragdolls (death = native SP menu flow, ends
// the session -- NOT a synced display state). Both recook-volatile BP fields.
int32_t MainPlayer_isRagdoll();
int32_t MainPlayer_dead();
// v63 device occupancy: activeInterface @0x07E0 is THE inside-a-device
// discriminator (null = not inside any screen; set/cleared ONLY by
// setActiveInterface). HitResult @0x0744 is the player's aim FHitResult --
// the enter chain icasts its Actor weakptr (+0x68), which the deny gate
// nulls for one InpActEvt_use dispatch.
int32_t MainPlayer_activeInterface();
int32_t MainPlayer_HitResult();

// AnimBlueprint_kerfurOmega_regular_C field accessors (VOTV BP).
int32_t AnimBP_kerfur_walkSpeed();
int32_t AnimBP_kerfur_Pawn();
int32_t AnimBP_kerfur_Controller();
int32_t AnimBP_kerfur_Movement();
int32_t AnimBP_kerfur_Character();
int32_t AnimBP_kerfur_animWalkAlpha();
int32_t AnimBP_kerfur_animWalkRate();
int32_t AnimBP_kerfur_lookingAtPlayer();
int32_t AnimBP_kerfur_kerfur();
int32_t AnimBP_kerfur_walkSpeedMultiplier();
int32_t AnimBP_kerfur_spd();
int32_t AnimBP_kerfur_useLegIK();
int32_t AnimBP_kerfur_removeArms();
int32_t AnimBP_kerfur_headLookAt();
int32_t AnimBP_kerfur_isFace();
// v39 head-look sync: `lookAt` (FVector @0x2D90) = the WORLD location the head/neck
// FAnimNode_LookAt nodes aim at; `customLookAt` (bool @0x2E49) gates BUA's per-tick
// auto-recompute (set true on a mirror to make a streamed lookAt stick). See
// research/findings/votv-kerfur-headlook-AnimBP-RE-and-coop-sync-2026-06-07.md.
int32_t AnimBP_kerfur_lookAt();
int32_t AnimBP_kerfur_customLookAt();

}  // namespace ue_wrap::reflected_offset

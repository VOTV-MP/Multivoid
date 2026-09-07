// ue_wrap/core/reflected_offset.h -- reflection-resolved BP property offsets.
//
// VOTV's BP-cooked classes recompile every patch, and a struct layout shifts whenever a
// property is added or reordered, so a hardcoded offset into that surface is a landmine the
// next update trips. Each accessor below resolves its (class, field) pair through reflection
// at first use and caches it, so the offset follows layout drift for as long as the field
// keeps its name. -1 means the class has not loaded yet, or the field was renamed; a caller
// checks the result before using it as an offset. The cache memorises only on SUCCESS, so a
// call that fires before the BP class loads is retried by the next one and late-loading
// content resolves without the call site owning a retry.
//
// Engine-stable offsets -- UObject internals, the FProperty and FField chain, a component's
// attach parent -- stay hardcoded in sdk_profile.h, since a BP recook does not move them.
// Only the BP-cooked surface (mainPlayer_C, the kerfur AnimBP) lives here, and no number
// does: naming one is the coupling this header exists to remove.

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
// holding_actor is the hotbar HAND ITEM pointer, and updateHold is what writes it: it
// destroys the previous item, spawns the new one from the equip data and attaches it to
// `weapon`. A chipPile or clump grab does not touch it -- the grab path writes
// grabbing_actor from the aim hit result -- so the pose emit reads holding_actor only as a
// fallback when grabbing_actor is null, for any other carry that might use it.
int32_t MainPlayer_holding_actor();
// The actor the local player is aiming at. On an E-press (InpActEvt_use) this is the door or
// interactable being used; the use observer reads it there to name the door for the
// host-authoritative sync.
int32_t MainPlayer_lookAtActor();
// Radial-menu confirm fields (kerfur menu verb detection on the client InpActEvt_use seam).
int32_t MainPlayer_releaseEToUse();  // the "release E to use" radial confirm flag
int32_t MainPlayer_actionIndex();    // the highlighted radial-menu option index
// Ragdoll and faint DISPLAY state. isRagdoll is the AnimBP gate that ragdollMode() flips, so
// any cause raises it -- the manual key, an exhaustion faint, a knockout. `dead` is the death
// bool, and the sender excludes a death ragdoll: death runs the native single-player flow and
// ends the session, so it is not a synced display state. Both are recook-volatile BP fields.
int32_t MainPlayer_isRagdoll();
int32_t MainPlayer_dead();
// Device occupancy. activeInterface is THE inside-a-device discriminator -- null means inside
// no screen -- and setActiveInterface is its only writer in the whole Blueprint, so a clear
// comes through the same call. HitResult is the player's aim FHitResult, whose actor weak
// pointer the enter chain casts; the deny gate nulls that for one InpActEvt_use dispatch.
int32_t MainPlayer_activeInterface();
int32_t MainPlayer_HitResult();

// AnimBlueprint_kerfurOmega_regular_C field accessors (VOTV BP).
int32_t AnimBP_kerfur_walkSpeed();
int32_t AnimBP_kerfur_Pawn();
int32_t AnimBP_kerfur_Controller();
int32_t AnimBP_kerfur_Movement();
int32_t AnimBP_kerfur_animWalkAlpha();
int32_t AnimBP_kerfur_animWalkRate();
int32_t AnimBP_kerfur_lookingAtPlayer();
int32_t AnimBP_kerfur_kerfur();
int32_t AnimBP_kerfur_walkSpeedMultiplier();
int32_t AnimBP_kerfur_spd();
int32_t AnimBP_kerfur_useLegIK();
int32_t AnimBP_kerfur_removeArms();
int32_t AnimBP_kerfur_isFace();
// Head-look sync. `lookAt` is the WORLD location the head and neck FAnimNode_LookAt nodes aim
// at; `customLookAt` gates the AnimBP's per-tick auto-recompute, and is set true on a mirror
// so a streamed lookAt sticks.
int32_t AnimBP_kerfur_lookAt();
int32_t AnimBP_kerfur_customLookAt();

}  // namespace ue_wrap::reflected_offset

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

}  // namespace ue_wrap::reflected_offset

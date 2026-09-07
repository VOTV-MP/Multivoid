// coop/player/skin_effects.h -- the native effect rig for builtin kerfur skins.
//
// A census skin is a MESH; the matching kerfur VARIANT ACTOR carries the rest of the look -- the
// animated RT face screen (kerfusFace_C scene-capture), the alive glow (belly point light + 14
// joint-life particles + the 'ag' emissive texture makeSentient sets), mynet's static-electricity
// rig and step bursts, keljoy's squeak footsteps. A player body dressed in the mesh alone shows the
// raw material atlas on the face screen and none of the effects, so after client_model applies a
// builtin skin's mesh this module rebuilds the variant's cosmetic identity on the player body,
// data-driven from the game's own classes.
//
// Rigs are keyed by body actor -- a puppet or the local pawn -- and torn down on a skin change or a
// body destroy. The kerfusFace actor is a separate world actor and MUST be destroyed with its
// owner. Every entry point is game-thread only.

#pragma once

#include "ue_wrap/core/types.h"

#include <string>

namespace coop::skin_effects {

// Build (or rebuild on change) the effect rig for `skinName` on `bodyActor`. Called by
// client_model::ApplySkinToBody after the mesh lands; a non-builtin skin (dr_kel, converter paks)
// tears any previous rig down and builds nothing. Idempotent for an unchanged skin with a live rig.
//
// What it builds:
//   - the base kerfurOmega_C SCS rig plus the variant class's own (ue_wrap/scs_rig: particles,
//     point light, decals, eff_* audio), TEMPLATE-FAITHFUL: a node the game authors dormant -- the
//     makeSentient-only joint-life sparks and lifeLight, bAutoActivate/bVisible false -- stays OFF.
//     Force-enabling them was the "pink blast" regression.
//   - the face: spawn the game's own kerfusFace_C, deferred, with `type` stamped pre-BeginPlay
//     exactly as kerfurOmega.makeFace does, read its `dynmat` (the 256x256 scene-capture RT
//     material its BeginPlay generates) and set it into the mesh's faceMaterialIndex slot. Only for
//     the four omega bodies whose mesh really has the screen slot: fmi=1, Type 0/1/2 =
//     blue/pink/green.
void Apply(void* bodyActor, const std::string& skinName);

// Tear the rig down before the body actor dies (destroys the face actor;
// components go down with the actor). RemotePlayer::Destroy calls this right
// before DestroyActor.
void OnBodyDestroyed(void* bodyActor);

// Hide/show the rig's scene components -- the ragdoll flop hides the kel
// meshes (remote_player_ragdoll), and floating sparks/light over a hidden
// body give the trick away. Audio keeps playing (the peer is still there).
void SetRigVisible(void* bodyActor, bool visible);

// The volume the DEFAULT step (lib_C::step via votv_lib::CharacterStep) should
// run at for this body: `fallback` normally, 0 when a REPLACE-mode rig (mynet)
// is active -- the native mynet passes volume 0 to lib step, muting the
// surface footstep while keeping the trace/water/friction side effects. The
// puppet's step dispatch feeds this straight into CharacterStep.
float DefaultStepVolume(void* bodyActor, float fallback);

// Fire the skin's step FX (footstep sound, mynet burst) for a step that JUST landed -- the caller
// owns the stride gate. Puppets call this from the SAME footsteps_.StepDue verdict that dispatches
// lib_C::step, so the skin sound lands exactly on the native step; two independent accumulators
// drift apart and the audible step doubles. No-op without step FX.
//
// The FX mirror the variant's own step routing. A REPLACE variant (mynet) calls lib_C::step with
// volume 0, which mutes the default surface footstep, and plays its own sound instead -- boltrix at
// the actor location, volume 1, att_default -- plus the eff_mynetEmitterStep burst. An ADDITIVE
// variant (keljoy) overrides no step at all: the base class's stepped() runs, layering
// footstepSound over the audible default at volume/4 and pitch volume/2 + 1, where volume is
// clamp(MaxWalkSpeed / 400, 0.5, 2) as lib_C::step scales it.
void OnStep(void* bodyActor, const ue_wrap::FVector& pos);

// Own-body variant WITH the stride gate built in: the LOCAL player's native
// stride lives in mainPlayer's BP tick (EX_CallMath, invisible to hooks), so
// the coop layer runs its own gate over the wire-pose samples. The REPLACE
// sound layer is suppressed here: the game's own default step cannot be muted
// on the local body (EX-invisible), so adding the replacement sound would
// stack the exact double the mode exists to avoid; the visual burst still
// fires. Game thread.
void TickStride(void* bodyActor, const ue_wrap::FVector& pos, float speedCmS,
                bool grounded);

}  // namespace coop::skin_effects

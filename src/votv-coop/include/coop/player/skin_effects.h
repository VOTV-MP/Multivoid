// coop/player/skin_effects.h -- the native effect rig for builtin kerfur skins.
//
// WHY (user report 2026-07-03, screenshots): a census skin is a MESH; the
// matching kerfur VARIANT ACTOR carries the rest of the look -- the animated
// RT face screen (kerfusFace_C scene-capture), the alive glow (belly point
// light + 14 joint-life particles + the 'ag' emissive texture makeSentient
// sets), mynet's static-electricity rig and step bursts, keljoy's squeak
// footsteps. A player body dressed in the mesh alone shows the raw material
// atlas on the face screen and none of the effects.
//
// WHAT: after client_model applies a builtin skin's mesh, this module rebuilds
// the variant's cosmetic identity on the player body, data-driven from the
// game's own classes (RE: docs in research/pak_re, kerfurOmega*.json 2026-07-03):
//   - base kerfurOmega_C SCS rig + the variant class's own SCS rig
//     (ue_wrap/scs_rig: particles / point light / decals / eff_* audio),
//     TEMPLATE-faithful: nodes the game authors dormant (the makeSentient-only
//     joint-life sparks + lifeLight, bAutoActivate/bVisible=false) stay OFF --
//     force-enabling them was the 2026-07-03 "pink blast" regression;
//   - the face: spawn the game's own kerfusFace_C (deferred, `type` stamped
//     pre-BeginPlay exactly like kerfurOmega.makeFace), read its `dynmat`
//     (the 256x256 scene-capture RT material its BeginPlay gen()erates), set
//     it into the mesh's faceMaterialIndex slot -- ONLY for the four omega
//     bodies whose mesh really has the screen slot (census: fmi=1, Type
//     0/1/2 = blue/pink/green);
//   - step FX at the shared stride gate (puppet_footsteps::Stride::StepDue),
//     mirroring the variant's own step routing (bytecode, 2026-07-03):
//     REPLACE variants (mynet) call lib_C::step with volume 0 -- the default
//     surface footstep is MUTED -- and play their own sound (boltrix at the
//     actor location, vol 1, att_default) + the eff_mynetEmitterStep burst;
//     ADDITIVE variants (keljoy) keep the default step and layer footstepSound
//     on top with the native stepped() math: scaled = clamp(MaxWalkSpeed/400,
//     0.5, 2) * volume, sound attached to the body at scaled/4 volume,
//     scaled/2+1 pitch (kerfurOmega ubergraph stepped region).
//
// Lifecycle: rigs are keyed by body actor (puppet or local pawn) and torn
// down on skin change / body destroy; the kerfusFace actor is a separate
// world actor and MUST be destroyed with its owner. All entry points are
// game-thread only.

#pragma once

#include "ue_wrap/core/types.h"

#include <string>

namespace coop::skin_effects {

// Build (or rebuild on change) the effect rig for `skinName` on `bodyActor`.
// Called by client_model::ApplySkinToBody after the mesh lands; a non-builtin
// skin (dr_kel / converter paks) tears any previous rig down and builds
// nothing. Idempotent for an unchanged skin with a live rig.
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

// Fire the skin's step FX (footstep sound / mynet burst) for a step that
// JUST landed -- the caller owns the stride gate. Puppets call this from the
// SAME footsteps_.StepDue verdict that dispatches lib_C::step, so the skin
// sound lands exactly on the native step (perf audit W2: two independent
// accumulators drifted apart = doubled audible steps). No-op without step FX.
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

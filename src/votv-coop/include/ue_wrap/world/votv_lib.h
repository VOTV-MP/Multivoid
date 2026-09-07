// ue_wrap/world/votv_lib.h -- thunks into VOTV's shared BP function library (lib_C).
//
// Engine-wrapper layer (principle 7): resolves the lib_C CDO + UFunctions once
// and exposes typed calls. No coop/network state.

#pragma once

namespace ue_wrap::votv_lib {

// Dispatch lib_C::step(Character, ...) -- THE shared footstep verb every VOTV
// walker funnels into (the local player's tick accumulator at mainPlayer uber
// @70973 and kerfurOmega.step both call it). It sphere-traces to the ground,
// picks the surface-material step cue (foot_def/grass/metal/snow/water/...),
// SpawnSoundAttached's it with att_default + conc_footsteps, and fires the
// steppedOn world reactions -- fully spatialized, possession-agnostic, nothing
// for us to re-implement. Silent on its own when airborne (no ground hit).
// Args mirror the local player's call (Z_offset=0, pitch 1.0, speedVolume=400)
// but for two. `volume` is the caller's: the BP multiplies it onto its internal
// clamp(MaxWalkSpeed/speedVolume, 0.5, 2.0) loudness, and the puppet stride
// emitter passes its own tuning. `callActor` is the character, where the local
// player's call passes null; lib_C tests it for int_objects and calls its
// `stepped`, which mainPlayer_C implements as an empty event. Returns false
// until lib_C resolves. Game thread (ProcessEvent).
bool CharacterStep(void* character, float volume);

}  // namespace ue_wrap::votv_lib

// ue_wrap/windturbine.h -- standalone engine access for the giant map wind
// turbines (Awindturbine_C : Aactor_save_C). Principle-7 engine-wrapper layer:
// class resolve + the six driver-float field accesses. NO network logic --
// coop::turbine_sync owns the mirror and talks through here.
//
// The turbine is a per-tick SERVO (RE: votv-wind-turbines-RE-2026-06-11.md):
// `rot` integrates +/-1 deg/s toward the directionalWind direction, `targetRot`
// := rot, `headRotation` spring-chases targetRot and is applied to the nacelle
// pivot every tick; blades accumulate `alpha_blades` at a rate scaled by
// `bladesMomentum` and the BeginPlay-RNG `mult`. All six are PLAIN FLOATS the
// BP tick consumes -- a mirror writes them raw and the native tick does the
// rest (no verbs, no engine calls).

#pragma once

#include <cstdint>

namespace ue_wrap::windturbine {

// Resolve the windturbine_C UClass + the six field offsets (reflected, with
// the RE-documented Alpha 0.9.0-n fallbacks). Idempotent; false until the BP
// class is loaded (caller retries). Game thread.
bool EnsureResolved();

// True iff `obj`'s class is windturbine_C or a subclass. Cheap super-walk.
bool IsTurbine(void* obj);

// The six driver floats (see the header note / the payload field docs).
struct State {
    float headRotation = 0.f;
    float targetRot = 0.f;
    float rot = 0.f;
    float alphaBlades = 0.f;
    float bladesMomentum = 0.f;
    float mult = 1.f;
};

// Read/write `turbine`'s driver floats. False on null / unresolved. The write
// is raw field stores only -- the turbine's own tick applies them. Game thread.
bool ReadState(void* turbine, State& out);
bool WriteState(void* turbine, const State& st);

}  // namespace ue_wrap::windturbine

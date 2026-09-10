// ue_wrap/devices/windturbine.h -- standalone engine access for the giant map wind turbines
// (Awindturbine_C : Aactor_save_C). Principle-7 engine-wrapper layer: class resolve plus the six
// driver-float field accesses. NO network logic -- coop::turbine_sync owns the mirror and talks
// through here.
//
// The turbine is a per-tick SERVO. `rot` integrates at 1 deg/s toward the directionalWind
// direction, signed by which side of the wind the turbine currently faces; `targetRot` follows
// `rot`; `headRotation` spring-chases `targetRot` and is written to the `axis_room` pivot; blades
// accumulate `alpha_blades` at a rate scaled by `bladesMomentum`, which itself springs toward the
// wind's combined strength and speed over ten, and by the BeginPlay-random `mult`. All six are
// PLAIN FLOATS the tick consumes, so a mirror writes them raw and the native tick does the rest --
// no verbs, no engine calls. The tick integrates whether or not anyone is looking; only the APPLY
// to the pivots is gated on the turbine being near the camera or recently rendered.

#pragma once

#include <cstdint>

namespace ue_wrap::windturbine {

// Resolve the windturbine_C UClass + the six field offsets (reflected, with hard-coded
// fallbacks for the targeted game build). Idempotent; false until the blueprint class is loaded,
// which the caller retries. Game thread.
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

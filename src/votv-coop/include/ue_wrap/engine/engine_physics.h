// ue_wrap/engine/engine_physics.h -- a primitive component's rigid body: simulation on or off,
// whether it simulates, and its linear and angular velocity. Engine-wrapper layer (principle 7):
// each call is one reflected UFunction call on the component, with a stack parameter frame and no
// allocation, so the held-prop drive can make one every tick. Each UFunction resolves once; one
// that does not resolve is said once and its call is a no-op from then on. Game thread.
// Implementation: src/ue_wrap/engine/engine_physics.cpp.

#pragma once

namespace ue_wrap::engine {

// UPrimitiveComponent::SetSimulatePhysics. Null-safe.
void SetComponentSimulatePhysics(void* component, bool simulate);

// SetPhysicsLinearVelocity (cm/s) and SetPhysicsAngularVelocityInDegrees, replacing the body's own
// (bAddToCurrent false). A kinematic body ignores both: switch its simulation on first. Null-safe.
void SetComponentLinearVelocity(void* component, float vx, float vy, float vz);
void SetComponentAngularVelocity(void* component, float wx, float wy, float wz);

// IsSimulatingPhysics: USceneComponent's UFunction, which UPrimitiveComponent overrides natively.
// False for null or when it did not resolve.
bool IsComponentSimulatingPhysics(void* component);

}  // namespace ue_wrap::engine

// coop/grab_observer.h -- physics-prop grab/release/throw observers.
//
// Stage 1 of [[project-physics-object-pickup]]. Engine-native UFunction
// observers on UPhysicsHandleComponent / UPhysicsConstraintComponent /
// UPrimitiveComponent + BP-Timeline observers on mainPlayer_C. Logs the
// pickup/per-tick-drive/release path for the wire layer.
//
// Idempotent: Install() may be called every NetPumpTick; once successfully
// installed it becomes a no-op (the lifecycle/inventory/NPC installers each
// own their own retry loop similarly).
//
// Principle 7: this is gameplay-network logic (the observers feed wire
// payloads) so it lives under coop/, NOT under ue_wrap/.

#pragma once

namespace coop::grab_observer {

// Try to install all grab observers. Resolves the four UClasses
// (UPhysicsHandleComponent, UPhysicsConstraintComponent, UPrimitiveComponent,
// mainPlayer_C); if any aren't loaded yet, logs a warning and returns -- the
// caller (NetPumpTick) retries on the next tick. Eager-warms the PHC.Release
// resolver cache used by ue_wrap::engine::ReleaseMainPlayerGrabIfHolding so
// the first wire-received PropDestroy on a held prop can resolve immediately.
void Install();

}  // namespace coop::grab_observer

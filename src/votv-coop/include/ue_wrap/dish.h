// ue_wrap/dish.h -- standalone engine access for the satellite dishes
// (Adish_C, level-placed immortal actors) and the gamemode's dishs array.
// Principle-7 engine-wrapper layer -- NO network logic; coop/signal_catch_sync
// drives the v70 consume replay through here.
//
// RE (votv-stolas-signal-catch-RE-2026-06-12.md SS2): mainGamemode.dishs
// (@0x0330, TArray<Adish_C*>) is index-sorted by the level-authored
// dish.Index; the catch chain calls startMovingTo(lookAt) on EVERY dish with
// ONE shared relative vector -- startMovingTo gates on isMoving (a moving
// dish ignores new targets), then writes lookAt := param + ActorLocation
// (absolute) and runs the native slew (random 1-12 s delay, axis timelines,
// arrival -> activeDishes bookkeeping -> gamemode.checkFordDishes ->
// dishesStop broadcast -> the desk arms formDownload). So replaying ONE
// relative vector per peer reproduces the whole theater natively.

#pragma once

#include "ue_wrap/engine.h"  // FVector

#include <cstdint>

namespace ue_wrap::dish {

// Resolve mainGamemode_C.dishs + Adish_C fields/verbs (throttled lazy retry).
// Game thread.
bool EnsureResolved();

// Number of live dishes in gamemode.dishs (0 if unresolved / no world).
int32_t Count();

// Count of dishes currently slewing (isMoving). The v70 catch detector uses
// the RISING edge of this between two 1 Hz polls as the catch-success
// signature: dishes start moving ONLY from the catch chain's startMovingTo
// fan-out (expiry deletes a row with no dish movement; a failed ping moves
// no dish). Returns -1 if unresolved.
int32_t MovingCount();

// The exact startMovingTo argument of the in-flight slew, recovered as
// (dish.lookAt - dish.ActorLocation) from the first live MOVING dish --
// the BP computes one vector for all dishes, so any moving dish carries it.
// False if no dish is moving (caller sends slewValid=0).
bool ReadSlewFromMovingDish(ue_wrap::FVector& out);

// Reflected startMovingTo(lookAt=slew) on every live dish (the catch chain's
// @9494 fan-out). The BP itself skips already-moving dishes. Returns the
// number of dishes the call dispatched on. Game thread.
int32_t StartMovingAll(const ue_wrap::FVector& slew);

}  // namespace ue_wrap::dish

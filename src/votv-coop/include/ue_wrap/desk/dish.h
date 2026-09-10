// ue_wrap/desk/dish.h -- standalone engine access for the satellite dishes
// (Adish_C, level-placed immortal actors), the gamemode's dishs/activeDishes
// arrays, and the two dish tickers (ticker_disher_C / ticker_dishUncalib_C).
// Principle-7 engine-wrapper layer -- NO network logic; coop/signal_catch_sync
// and coop/dish_sync (the L4 pose/arm/calibration lanes) drive it through here.
//
// mainGamemode.dishs (TArray<Adish_C*>) is index-sorted by the level-authored
// dish.Index; the catch chain calls startMovingTo(lookAt) on EVERY dish with ONE
// shared relative vector -- startMovingTo gates on isMoving (a moving dish ignores
// new targets), then writes lookAt := param + ActorLocation (absolute) and runs
// the native slew (random 1-12 s delay, axis frame-loop, arrival -> activeDishes
// bookkeeping -> gamemode.checkFordDishes -> dishesStop broadcast -> the desk arms
// formDownload). The dish pose lives on TWO components in the RELATIVE frame:
// axis_Z.RelativeRotation.Yaw + axis_Y.RelativeRotation.ROLL; writes must go
// through K2_SetRelativeRotation, because raw field writes do not render.

#pragma once

#include "ue_wrap/engine/engine.h"  // FVector

#include <cstdint>
#include <string>

namespace ue_wrap::dish {

// Resolve mainGamemode_C.dishs/activeDishes + Adish_C fields/verbs/components
// + the ticker classes (throttled lazy retry). Game thread.
bool EnsureResolved();

// Number of live dishes in gamemode.dishs (0 if unresolved / no world).
int32_t Count();

// Per-dish diagnostic snapshot: the commanded TARGET (lookAt, absolute) + the
// slew flag, per live dish. lookAt is the SETTLED discriminator (readable while
// isMoving=true), so a HOST-vs-CLIENT diff can compare aim targets even mid-slew.
// Read-only (desk_diag). Fills up to `cap` entries; returns the count written.
struct DishState {
    int32_t index = 0;
    float   lookAtX = 0, lookAtY = 0, lookAtZ = 0;  // absolute commanded target
    bool    isMoving = false;
};
int32_t ReadAllDishStates(DishState* out, int32_t cap);

// Count of dishes currently slewing (isMoving). Returns -1 if unresolved.
// (No longer the catch-success signature -- the detector rides the
// coord_signalData identity tuple since L4; this stays for the host's pose
// sweep + settle-tail edge.)
int32_t MovingCount();

// The exact startMovingTo argument of the in-flight slew, recovered as
// (dish.lookAt - dish.ActorLocation) from the first live MOVING dish --
// the BP computes one vector for all dishes, so any moving dish carries it.
// False if no dish is moving (caller sends slewValid=0).
bool ReadSlewFromMovingDish(ue_wrap::FVector& out);

// Reflected startMovingTo(lookAt=slew) on every live dish (the catch chain's
// @9494 fan-out). The BP itself skips already-moving dishes. Returns the
// number of dishes the call dispatched on. Game thread. HOST-side only since
// L4 (the client sim is parked; a client never slews from wire).
int32_t StartMovingAll(const ue_wrap::FVector& slew);

// ---- L4 pose/state surface -------------------------------------------------

// One mirrorable row: the two RELATIVE pose channels + the flags the client
// panel reads. yawZ = axis_Z.RelativeRotation.Yaw; rollY =
// axis_Y.RelativeRotation.Roll (raw component READS -- reads don't need the
// K2 pipeline).
struct DishRow {
    int32_t index = 0;
    bool    isMoving = false;
    float   yawZ = 0.f, rollY = 0.f;
    float   calibration = 0.f;
};
int32_t ReadAllRows(DishRow* out, int32_t cap);

// Mirror-write the pose: K2_SetRelativeRotation(axis_Z, {0,0,yaw}) +
// (axis_Y, {roll,0,0}) -- the native loop's own frame + channel zeroing.
// Game thread.
bool WritePose(int32_t index, float yawZ, float rollY);

// Raw isMoving write (a plain bool, natively raw-EX_LetBool-written by the
// slew loop + stop() -- NOT a setter-managed field).
bool WriteIsMoving(int32_t index, bool moving);

// Reflected Adish_C::stop() (Public|BlueprintCallable; 2 flag writes). The
// caller owns the stale-set cleanup (cues + activeDishes).
bool StopDish(int32_t index);

// Audio cues (satellite_move_Cue / satellite_Cue): reflected UActorComponent
// Activate/Deactivate + IsActive. A mid-slew kill leaves the looping cues
// Active forever -- the park owns deactivation.
bool DeactivateCues(int32_t index);
bool ActivateMoveCue(int32_t index);
// True if EITHER satellite cue reads Active (the cue-reconciler probe).
// ok=false when the read itself failed (unresolved / dead component).
bool AnyCueActive(int32_t index, bool& ok);

// gamemode.activeDishes (TArray<bool>): the console ping gate
// ("Satellites are active") + checkFordDishes' arm gate. Raw element
// writes (natively raw-written at slew start @3197 + the arrival end chain).
bool ReadActiveDish(int32_t index, bool& out);
bool WriteActiveDish(int32_t index, bool active);

// dish.calibration (float). Raw write (the field is natively raw-written by the slew-loop
// decay, the ui_console calibrate machine, the tool and the virus event).
bool WriteCalibration(int32_t index, float v);

// dish.techName (FString) for identity logging ("[dish] 3 'Bonna'").
// Returns L"?" when unresolved/empty.
std::wstring TechName(int32_t index);

// Reflected mainGamemode.checkFordDishes() -- the native arm/display tail
// (gate Contains(activeDishes,true) -> ret; all-false -> dishesStop broadcast
// + camera aim + objectRenderer.begin() + signalFound). The L4 client ARM
// apply calls it with activeDishes pre-cleared so the gate passes.
bool CallCheckFordDishes();

// ---- L4 ticker surface -----------------------------------------------------
// Both tickers are gamemode-BeginPlay singletons (one per world per peer).
// disher: one-shot self-re-arming K2_SetTimerDelegate("do") chain -- kill =
// K2_ClearTimer, restore = PE ReceiveBeginPlay (the native initializer:
// gamemode gate -> BindDelegate(local) -> Random(1800,3600) ->
// SetTimerDelegate -> parent; bytecode-verified re-fire-safe).
// dishUncalib: actor tick -- kill/restore = SetActorTickEnabled, zero
// staleness (interval re-rolled at the top of every tick).

// Current live ticker instances (nullptr when not yet spawned / unresolved).
// Fresh instances after a level reload get fresh pointers -- the caller keys
// its park latch on these.
void* DisherInstance();
void* UncalibInstance();

bool ParkDisher(void* inst);     // K2_ClearTimer(inst, "do")
bool RestoreDisher(void* inst);  // PE ReceiveBeginPlay(inst)
bool ParkUncalib(void* inst);    // SetActorTickEnabled(false)
bool RestoreUncalib(void* inst); // SetActorTickEnabled(true)

}  // namespace ue_wrap::dish

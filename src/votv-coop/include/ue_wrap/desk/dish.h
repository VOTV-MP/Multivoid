// ue_wrap/desk/dish.h -- standalone engine access for the satellite dishes
// (Adish_C, level-placed immortal actors), the gamemode's dishs/activeDishes
// arrays, and the dish-moving ticker (ticker_disher_C).
// Principle-7 engine-wrapper layer -- NO network logic; coop/signal_catch_sync,
// coop/dish_sync and coop/download_arm_sync (the pose, calibration and arm lanes) drive it.
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

// ---- L4 pose/state surface -------------------------------------------------

// One mirrorable row: the two RELATIVE pose channels + the flags the client
// panel reads. yawZ = axis_Z.RelativeRotation.Yaw; rollY =
// axis_Y.RelativeRotation.Roll (raw component READS -- reads don't need the
// K2 pipeline).
struct DishRow {
    int32_t index = 0;
    bool    isMoving = false;
    float   yawZ = 0.f, rollY = 0.f;
};
int32_t ReadAllRows(DishRow* out, int32_t cap);

// One dish's precision by its gamemode.dishs index.
struct DishCalibration {
    int32_t index = 0;
    float   value = 0.f;
};
// Every live dish's calibration: the precision's one read, apart from the pose rows above. Game
// thread.
int32_t ReadCalibrations(DishCalibration* out, int32_t cap);

// Mirror-write the pose: K2_SetRelativeRotation(axis_Z, {0,0,yaw}) +
// (axis_Y, {roll,0,0}) -- the native loop's own frame + channel zeroing.
// Game thread.
bool WritePose(int32_t index, float yawZ, float rollY);

// Raw isMoving write (a plain bool, natively raw-EX_LetBool-written by the
// slew loop + stop() -- NOT a setter-managed field).
bool WriteIsMoving(int32_t index, bool moving);

// Audio cues (satellite_move_Cue / satellite_Cue): reflected UActorComponent
// Activate/Deactivate + IsActive. A mid-slew kill leaves the looping cues
// Active forever -- the park owns deactivation.
bool DeactivateCues(int32_t index);

// A slew's sounds, as its own body plays them, for a copy whose body does not run: the hum at its volume, the motor on and
// off, the clunk an axis stops with, the hum's fade and the last one-shot, with the hum off.
bool StartHum(int32_t index);
bool SetMotor(int32_t index, bool on);
bool PlayAxisStop(int32_t index);
bool SetFade(int32_t index, float volume);
bool PlayStopAndEndHum(int32_t index);

// The `turnoff` timeline's length, the fade a stop runs, in seconds; 0 when it does not resolve.
float FadeSeconds(int32_t index);

// Whether the hum and the motor read active, each `false` when unread.
void ReadCues(int32_t index, bool& hum, bool& motor);
// True if EITHER satellite cue reads Active (the cue-reconciler probe).
// ok=false when the read itself failed (unresolved / dead component).
bool AnyCueActive(int32_t index, bool& ok);

// gamemode.activeDishes (TArray<bool>): the console ping gate
// ("Satellites are active") + checkFordDishes' arm gate. Raw element
// writes (natively raw-written at slew start @3197 + the arrival end chain).
bool ReadActiveDish(int32_t index, bool& out);
bool WriteActiveDish(int32_t index, bool active);

// dish.calibration (float). Raw write, as its nine Blueprint writers do: the dish's own precision
// loss in its slew loop, its loadData, a lightning strike's hit, the desk's virus event, the cheat
// menu, the SAT console's calibration, the toolgun's calibration tool and the uncalibrator
// (ue_wrap/desk/dish_writers), and a level trigger (trigger_fakeLmaos).
bool WriteCalibration(int32_t index, float v);

// A component of dish `index` that a hit on the dish can name (its yaw axis), or null. Game thread.
void* HitComponent(int32_t index);

// dish.techName (FString) for identity logging ("[dish] 3 'Bonna'").
// Returns L"?" when unresolved/empty.
std::wstring TechName(int32_t index);

// Every dish's hash code as one number: FNV-1a 64 over each `hashcode` (the nine-line text
// generteHashcode writes), taken in gamemode.dishs order, never by a class walk, so a dish of a
// subclass counts where the level placed it. Two peers with equal digests hold the same codes.
// `dishes` is the array's length, `filled` how many entries carry a code of a sane length (a dead
// entry and an oversize one are mixed as their own markers and not counted). False while the
// gamemode or the field is unresolved. Read-only; game thread.
struct HashDigest {
    uint64_t digest = 0;
    int32_t  dishes = 0;
    int32_t  filled = 0;
};
bool ReadHashDigest(HashDigest& out);

// One dish's `hashcode` by its gamemode.dishs index. ReadHashcode leaves `out` empty for an empty
// code, and is false while the dish or the field is unresolved or the text is past the bound;
// WriteHashcode replaces the field and frees the buffer it replaced (field_io). Game thread.
inline constexpr int32_t kMaxHashcodeChars = 4096;
bool ReadHashcode(int32_t index, std::wstring& out);
bool WriteHashcode(int32_t index, const std::wstring& code);

// The gamemode.dishs index `dish` sits at, or -1 (not in the array, or the array unresolved).
int32_t IndexOf(void* dish);

// The dish at a gamemode.dishs index, or null (out of range, dead, or the array unresolved).
void* DishByIndex(int32_t index);

// Reflected mainGamemode.checkFordDishes() -- the native arm/display tail
// (gate Contains(activeDishes,true) -> ret; all-false -> dishesStop broadcast
// + camera aim + objectRenderer.begin() + signalFound). The L4 client ARM
// apply calls it with activeDishes pre-cleared so the gate passes.
bool CallCheckFordDishes();

// mainGamemode.setPrec(): the average of every dish's calibration, written into the desk's
// DL_precMult, from which the desk's download computes its rate every tick. A 10 s looping timer
// runs it on every peer. The function's name, for a watch.
inline constexpr const wchar_t* kSetPrec = L"setPrec";
// Reflected setPrec() on the live gamemode. Game thread.
bool CallSetPrec();

// ---- L4 ticker surface -----------------------------------------------------
// The disher is a gamemode-BeginPlay singleton (one per world per peer): a
// one-shot self-re-arming K2_SetTimerDelegate("do") chain -- kill =
// K2_ClearTimer, restore = PE ReceiveBeginPlay (the native initializer:
// gamemode gate -> BindDelegate(local) -> Random(1800,3600) ->
// SetTimerDelegate -> parent; bytecode-verified re-fire-safe).

// The current live ticker instance (nullptr when not yet spawned / unresolved).
// A fresh instance after a level reload gets a fresh pointer -- the caller keys
// its park latch on it.
void* DisherInstance();

bool ParkDisher(void* inst);     // K2_ClearTimer(inst, "do")
bool RestoreDisher(void* inst);  // PE ReceiveBeginPlay(inst)

}  // namespace ue_wrap::dish

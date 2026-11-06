// ue_wrap/devices/cremator.h -- reflected access to VOTV's cremator (cremator_C, an Aactor_save_C): the flags of
// its lever run, its door and firebox, the arrays of what it holds, and the calls that drive it. What the
// machine reads and does, and nothing about who may ask it to (principle 7). Game thread.
//
// Every read takes the cremator and answers false for another class, a class not loaded, or a field that did not
// resolve; the pointer is live by the caller's contract (an index entry, a gate callback's object).
#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>
#include <string>

namespace ue_wrap::cremator {

// True once cremator_C is loaded. Game thread.
bool EnsureResolved();

// True iff the actor's class IS cremator_C, exactly: a pointer compare against the class kept for this world.
bool IsCremator(void* obj);

// The Aactor_save_C key, the game's own (a local name until a lane's portable identity replaces it); "" for
// another class.
std::wstring GetKeyString(void* cremator);

// The action both the handle and the valve answer to: the machine's actionOptionIndex(player, hit, action, ...) pulls
// the handle, or turns the valve, at this value when the last look-at hit it; every other value is a look-at option.
inline constexpr uint8_t kPressAction = 4;

// The lever run: isActive (the burn is on), anim (the handle is mid-travel), isClosed (the door is shut and
// latched), lookingAt_handle and lookingAt_valve (the last look-at hit the handle, or the fuel valve), windup (0..1
// ramp), gasAmount.
bool ReadActive(void* cremator, bool& value);
bool ReadAnim(void* cremator, bool& value);
bool ReadClosed(void* cremator, bool& value);
bool ReadLookingAtHandle(void* cremator, bool& value);
bool ReadLookingAtValve(void* cremator, bool& value);
// Writes lookingAt_handle (a bit of a Blueprint bool; no setter exists). False for another class or a field that did
// not resolve. A dev drill's forced look-at, for a press made from beyond the look-at trace's reach.
bool WriteLookingAtHandle(void* cremator, bool value);
bool ReadWindup(void* cremator, float& value);
bool ReadGasAmount(void* cremator, float& value);

// What the machine holds: `actors` Num and `health[0]`. -1 and kNoHealth when unreadable or empty.
inline constexpr float kNoHealth = -999.f;
struct Machine {
    int32_t actors = -1;
    float health0 = kNoHealth;
};
Machine ReadMachine(void* cremator);

// The door (swingerDoor, a prop_swinger) and the firebox box component; null when unreadable or dead.
void* Door(void* cremator);
void* Firebox(void* cremator);
// The handle (`handle`, the static mesh component whose look-at sets lookingAt_handle) and its world location; null
// or false when unreadable or dead.
void* Handle(void* cremator);
bool HandleLocation(void* cremator, FVector& at);
// The burn volume (the `burning` box component whose begin-overlap collects an actor); null when unreadable or dead.
void* BurnVolume(void* cremator);
// Whether the burn volume overlaps `actor` now (the component's own IsOverlappingActor). False for another class, a
// volume that did not resolve, a dead actor, or a call that did not run.
bool OverlapsBurnVolume(void* cremator, void* actor);
// The door's `opened`.
bool ReadDoorOpened(void* cremator, bool& opened);
// The firebox component's world location.
bool FireboxLocation(void* cremator, FVector& at);

// The actor the machine's burn loop stands on, read out of the frame of a call the loop made. `callerFunction` and
// `callerLocals` are the calling frame's (script_gate::Call): true, with the loop's current actor, only when the
// call came from cremator_C's own ubergraph and its loop variable (CallFunc_Array_Get_Item) resolves. False, actor
// null, for any other caller or a variable that did not resolve (said once). Game thread.
bool LoopActor(void* callerFunction, const uint8_t* callerLocals, void*& actor);

// useLever(): the handle's pull, the run's start or reverse. False when the call did not run.
bool CallUseLever(void* cremator);

// If the door is open, shuts it with its own Close(); false when that call did not run (true for a cremator with
// no door). `wasOpened` says what it was.
bool ShutDoor(void* cremator, bool& wasOpened);
// If the door is shut, opens it with its own Open(false); false when that call did not run. `opened` is read after.
bool OpenDoor(void* cremator, bool& opened);

// The cremator whose firebox is nearest `from`, no distance cap; actor null when none with a firebox is loaded.
struct Nearest {
    FVector from;
    float best;  // the squared distance to the closest firebox
    void* actor;
    FVector at;
};
Nearest FindNearestFirebox(const FVector& from);

// The nearest cremator's firebox with the pose an order to stand in it carries: the machine, the firebox's location and
// the cremator's own rotation (so the arrival faces the way the machine does). Read in one place, for every caller that
// sends a player there. NoCremator: none with a firebox is loaded. RotationUnread: the
// machine's rotation did not read (`out.actor` and `out.at` are set).
struct FireboxMove {
    void*     actor = nullptr;
    FVector   at{};
    FRotator  rot{};
};
enum class MoveResult : uint8_t { Found, NoCremator, RotationUnread };
MoveResult FindFireboxMove(const FVector& from, FireboxMove& out);

}  // namespace ue_wrap::cremator

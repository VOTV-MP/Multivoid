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

// The lever run: isActive (the burn is on), anim (the handle is mid-travel), isClosed (the door is shut and
// latched), lookingAt_handle (the last look-at hit the handle), windup (0..1 ramp), gasAmount.
bool ReadActive(void* cremator, bool& value);
bool ReadAnim(void* cremator, bool& value);
bool ReadClosed(void* cremator, bool& value);
bool ReadLookingAtHandle(void* cremator, bool& value);
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
// The door's `opened`.
bool ReadDoorOpened(void* cremator, bool& opened);
// The firebox component's world location.
bool FireboxLocation(void* cremator, FVector& at);

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

}  // namespace ue_wrap::cremator

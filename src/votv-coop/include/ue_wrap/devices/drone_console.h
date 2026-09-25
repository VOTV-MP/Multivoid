// ue_wrap/devices/drone_console.h -- the garage console (AdroneConsole_C), the drone's call button.
//
// Its own file rather than a corner of ue_wrap/devices/drone: this subtree is one file per game
// class, and the console is its own actor with its own fields and its own verb. The drone it calls
// is ue_wrap/devices/drone.
//
// The console's one action option dispatches on what the presser is looking at: its keyboard runs
// drone.triggerFly(console), the call-or-send verb behind the "drone is active" line, while its
// other face toggles the drone's leaveAfter5min. It holds its drone as a LEVEL REFERENCE, so every
// peer's console points at that peer's own drone -- which is why a client's press reached only its
// own suppressed mirror. The lane that carries a client's press to the host is
// coop/interactables/drone_call_intent.

#pragma once

#include <cstdint>

namespace ue_wrap::drone_console {

// True iff `obj` is a droneConsole_C or a descendant. For a once-per-press call site, not for a
// shared verb's every dispatch: the class is one object-index lookup by name. Game thread.
bool IsConsole(void* obj);

// Every console of the running world. The console is baked into the level and no lane keys it, so a
// peer cannot name one over the wire; the host finds the one a sender stands at by asking its own
// world and then its own reach. Found through the object index (world_instances), with no walk of the
// object array. Fills up to `cap`, returns the count. Game thread.
int32_t LiveConsoles(void** out, int32_t cap);

// The console's lid: the keyboard is only pressable while it is open, which is the console's own
// gate on the verb. False when the field does not resolve, so an unreadable lid refuses.
bool IsLidOpen(void* console);

// The presser's cursor is on the keyboard rather than the console's other face: the discriminator
// the game itself uses to pick the verb, and it is LOCAL to whoever is looking.
bool IsCursorOnKeyboard(void* console);

// The presser's cursor is on the lid's latch, whose press toggles the lid. Local, like the keyboard's.
// Both flags are written when the console builds a presser's action options, not cleared when the
// presser looks away, so a reader that needs the trace's present answer asks the trace too.
bool IsCursorOnLid(void* console);

// A part of the console by the name its graph gives the component: `button_call` (the keyboard),
// `button_door` (the lid's latch) or `button_changeLeave` (the leave-timer face). Null on null, an
// unresolved name or a dead component. For a dev drill's aim only: each call walks the class's
// properties by name. Game thread.
void* PartOf(void* console, const wchar_t* name);

// Run the keyboard's own verb: drone.triggerFly(console) on the drone this console references.
// The drone's body owns every condition (a sack aboard, the radiotower, a sack on the pad), so
// this asks nothing and answers only whether the call was dispatched. Game thread.
bool TriggerFly(void* console);

}  // namespace ue_wrap::drone_console

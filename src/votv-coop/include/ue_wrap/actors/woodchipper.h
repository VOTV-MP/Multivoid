// ue_wrap/actors/woodchipper.h -- engine access for the woodchipper (woodchipper_C, a prop_C). Principle-7
// engine-wrapper layer: no network or coop state.
//
// The machine keeps `fuel` (0..100) and `active`. A gas can adds fuel through the chipper's gascanFuel, which ends in
// checkActive; the button is the use action (4) of its actionOptionIndex, which flips `active` only when the hit names
// the `button_del` box and calls checkActive when there is fuel; its own tick burns fuel while it spins and switches it
// off at 0. checkActive repaints the machine from `active` and the fuel (intake collision, particles, spin-up, sounds).
// `active` is a packed bool of the class, so it is read and written through its mask. A Blueprint class dies with the
// world that loaded it, so the class is known here by its name and the layout is read off the live class.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace ue_wrap::woodchipper {

// The chipper's use action: the button's branch of its actionOptionIndex.
inline constexpr uint8_t kButtonAction = 4;

// The tank's ceiling: gascanFuel asks the can for at most the room under it, and the tick clamps to it.
inline constexpr float kMaxFuel = 100.f;

// True iff `obj` is a woodchipper_C or a subclass, once the class is loaded. Game thread.
bool IsChipper(void* obj);

// Every live chipper of the world (not the class default object), in no particular order, handed to `fn`; returns the
// count. The callback must not create or destroy objects. Game thread.
size_t ForEachChipper(void (*fn)(void* ctx, void* chipper), void* ctx);

// The chipper's prop key, the cross-peer id of this machine; empty for an unkeyed chipper, a null `chipper`, or the
// key `None`. Game thread.
std::wstring KeyOf(void* chipper);

// The chipper's fuel and whether it is on. False, with the outs untouched, for anything that is not a live chipper
// or when the layout does not resolve. Game thread.
bool Read(void* chipper, float& fuel, bool& active);

// Direct writes of the two fields. The game's own bodies follow them (checkActive after a write that changes the
// machine's running state); a writer that wants the repaint calls CallCheckActive. False as Read. Game thread.
bool WriteFuel(void* chipper, float fuel);
bool WriteActive(void* chipper, bool active);

// The chipper's own checkActive. False when it did not resolve or run. Game thread.
bool CallCheckActive(void* chipper);

// The button's box (`button_del`), or null. Game thread.
void* ButtonComponent(void* chipper);

// A player's press of the button as the game runs it: actionOptionIndex(player, a hit naming button_del, 4). False
// when the chipper has no button or the call did not run. Game thread.
bool CallPress(void* chipper, void* player);

}  // namespace ue_wrap::woodchipper

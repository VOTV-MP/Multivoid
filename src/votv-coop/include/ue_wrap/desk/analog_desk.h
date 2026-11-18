// ue_wrap/desk/analog_desk.h -- reflected access to the signal desk's back plates (analogDScreenTest_C): the two
// arrays that hold them, their meshes and lids, their prop classes, and the desk's own calls that repaint them. What the
// desk reads and does, and nothing about who may ask it to (principle 7). Game thread.
//
// The desk has four plates, 0 top left, 1 bottom left, 2 top right (which the game never lets move), 3 bottom right.
// `panelsState` says whether each is on, `panelsUnscrew` whether each is unbolted; `updPanels` shows each by its state.
// Every read answers false or null for a null desk, a field that did not resolve, or an array shorter than four.
#pragma once

#include "ue_wrap/core/types.h"

#include <cstdint>

namespace ue_wrap::analog_desk {

inline constexpr int     kPlates = 4;
inline constexpr uint8_t kFixedPlate = 2;  // the top right plate, which the game never lets move

// The placed desk, live and not the class default; null while none is loaded.
void* Find();

// The plates as one byte: on in bits 0-3, unbolted in bits 4-7. False when either array is short.
bool ReadPlates(void* desk, uint8_t& bits);

// Writes both arrays from the byte of ReadPlates, with no repaint. False when either array is short.
bool WritePlateArrays(void* desk, uint8_t bits);

// updPanels(): the plates shown, collided and tilted by the arrays. False when the call did not run.
bool CallUpdPanels(void* desk);

// soundDetachPanel(location). False when the call did not run.
bool CallSoundDetach(void* desk, const FVector& at);

// Plate `i`'s mesh (the `panels` array) and its lid (lid1, lid2, lid3_no, lid4), the component a plate prop must touch
// to be set back; null when unread or dead.
void* PlateMesh(void* desk, int i);
void* Lid(void* desk, int i);

// The prop class a taken-off plate `i` becomes (prop_computerBackPanel_tl/bl/tr/br_C), and the plate a live actor's
// class is the prop of: -1 for any other actor.
const wchar_t* PlatePropClass(int i);
int PlateOfProp(void* actor);

// panelsProcess (a plate being set back) and panelTakeoff (a plate being taken off): the flags the desk's own overlap
// waits on. `fallback` when the desk or the field is unread.
bool ProcessOr(void* desk, bool fallback);
bool WriteTakeoff(void* desk, bool value);

// lookingAtPanel, the plate the look is on; -1 for none or unread.
int32_t LookingAtPanel(void* desk);

}  // namespace ue_wrap::analog_desk

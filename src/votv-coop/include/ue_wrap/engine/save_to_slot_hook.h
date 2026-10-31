// ue_wrap/engine/save_to_slot_hook.h -- the detour on UGameplayStatics::SaveGameToSlot, the one
// native function every save of the game funnels through.
// The Blueprint funnel above it (saveSlot_C::saveToSlot) is called Blueprint-to-Blueprint, which
// dispatches through ProcessInternal and never reaches the ProcessEvent detour, so the engine's
// own write function is the first place a save can be seen at all. MinHook takes one detour per
// target, so this file owns it and the two things a caller can want are separate seams:
//   * a GATE, asked before a world save is written, which may cancel it;
//   * a WRITTEN notice, given after the engine reported a world save written.
// Both see WORLD saves only -- a USaveGame that is a saveSlot_C. The meta save (save_main_C:
// keybinds, achievements, store) passes straight through and is reported to nobody.
// A world save is not always THE world: the save-slot menu writes regenerated and copied
// saveSlot_C objects of its own, and the join capture writes the live one to a scratch slot. The
// object and the slot name are handed over so the caller can tell; this layer does not.
// Engine-wrapper layer (principle 7): no session, no role, no coop state. Both seams run on the
// thread that called SaveGameToSlot, which for every save the game makes is the game thread.

#pragma once

namespace ue_wrap::save_to_slot_hook {

// A world save is about to be written. True lets the engine write it; false cancels it, and the
// engine call returns false -- "the save did not happen", which is what the Blueprint save flow
// shows the player. `slotName` is valid for the call only. A gate that FAULTS cancels the save:
// a process that cannot decide does not write.
using Gate = bool (*)(void* saveObject, const wchar_t* slotName);

// The engine reported a world save written. `slotName` is valid for the call only.
using Written = void (*)(void* saveObject, const wchar_t* slotName);

// Install the detour. Idempotent, and cheap once it has answered for good. False while
// saveSlot_C is not loaded yet -- without the class every save would read as "not a world save"
// and slip past the gate, so the detour is not armed before it -- call again later. A signature
// miss is logged once and is final.
bool Install();

// One gate; null clears. Any thread.
void SetGate(Gate gate);

// Up to kMaxWritten written subscribers, each called in the order added. Adding one already added is a
// no-op; false when the table is full. Any thread.
inline constexpr int kMaxWritten = 4;
bool AddWritten(Written written);

}  // namespace ue_wrap::save_to_slot_hook

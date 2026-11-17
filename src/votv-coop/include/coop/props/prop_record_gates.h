// coop/props/prop_record_gates.h -- a class's say over a prop's save record landing (coop/props/prop_save_data).
//
// The record lane carries every covered prop's state and knows nothing of what a class does with it. A
// class that has a rule about who may write its record, or about when a record may be applied, registers
// the rule here from its own Install (the shape of prop_save_data::DeclareClassOwnedElsewhere, and for the
// same reason: the rule is declared where the class's lane lives, and the generic lane names no class).
//   - an INTENT gate: the HOST asks it before a client's record lands on an actor of the class, with the
//     slot that sent it;
//   - an INTENT RECORD gate: the same ask with the client's record in hand, for a rule that reads what the record
//     says (where a stick is, what it names); asked at the intent's arrival and again from the park when it lands;
//   - an APPLY gate: asked before any record is written onto an actor of the class, on any peer; false
//     drops the record (it is not parked: a gate that wants a record to wait has no use for this one).
// A gate is asked for an actor of the class or below, and may be asked again for one record that did not
// land (one that fails to apply stays parked and is asked again), so a gate must be idempotent. A false answer
// is final for that record: it is dropped and never asked again. Game thread.

#pragma once

#include <cstdint>

namespace ue_wrap::save_record { struct SaveRecord; }

namespace coop::prop_record_gates {

using IntentGate = bool (*)(void* actor, uint8_t senderSlot);
using IntentRecordGate = bool (*)(void* actor, uint8_t senderSlot, const ue_wrap::save_record::SaveRecord& rec);
using ApplyGate  = bool (*)(void* actor);

// Idempotent per (class, function). The class is resolved by name when a record is judged, so a lane may
// register before its class is loaded.
void RegisterIntentGate(const wchar_t* className, IntentGate fn);
void RegisterIntentRecordGate(const wchar_t* className, IntentRecordGate fn);
void RegisterApplyGate(const wchar_t* className, ApplyGate fn);

// Every gate of a class the actor belongs to must agree. True when none names it.
bool IntentAllows(void* actor, uint8_t senderSlot);
bool IntentRecordAllows(void* actor, uint8_t senderSlot, const ue_wrap::save_record::SaveRecord& rec);
bool ApplyAllows(void* actor);

}  // namespace coop::prop_record_gates

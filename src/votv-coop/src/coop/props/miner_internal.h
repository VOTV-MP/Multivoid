// coop/props/miner_internal.h -- what the mining rig lane's files share. Private to coop/props.
//
// miner_sync.cpp: the install, the watches and the client's asks. miner_state.cpp: a rig's state, sent by the host and applied
// by a client. miner_judge.cpp: the host's judge of a client's ask. miner_record.cpp: the class gate on a client's record.

#pragma once

#include "ue_wrap/actors/miner.h"

#include <cstdint>

namespace coop::net {
class Session;
struct MinerPayload;
}  // namespace coop::net

namespace coop::miner_sync::detail {

coop::net::Session* SessionPtr();
bool IsHost();
bool IsClient();

// The rig's prop element id, 0 for none; the live rig an element id names, null for none or for an element that is not a
// rig.
uint32_t EidOf(void* rig);
void* RigOfEid(uint32_t eid);

// ---- miner_state.cpp ----

// HOST: the rig's state to `toSlot`, or to every peer when it is negative (which also makes it the last state said of that rig).
// Nothing is sent for a rig with no element id. A send that fails is counted.
void SendState(void* rig, int toSlot);

// HOST: the state of rig `eid` is owed to `slot` (its ask was refused); sent at the next tick, once for any number of asks.
void OweState(uint8_t slot, uint32_t eid);
void FlushOwed();

// HOST, the end of a rig's own tick: the rig's numbers against what was last said of it.
void OnRigTickPost(void* rig);

// HOST: every rig's state to `slot`; the rigs with no element id are counted and said.
void SendSnapshot(int slot);

// CLIENT: the host's state of a rig, applied to its copy now, or kept (the latest per rig) until the copy is here.
void ApplyOrPark(const coop::net::MinerPayload& p);

// CLIENT: a prop copy was bound to `eid`: the state waiting for it is applied to `actor`.
void OnBound(uint32_t eid, void* actor);

void ClearState();

// A client's ask that went unsent, and the host's answer heard, are counted here; the numbers print at the session's end.
void CountSendFailed();

// ---- miner_judge.cpp ----

// HOST: judge a client's op 2 or op 3.
void JudgeIntent(int senderSlot, const coop::net::MinerPayload& p);

// CLIENT: the host's answer, kept and said.
void HeardAnswer(const coop::net::MinerPayload& p);

void ClearJudge();

// ---- miner_record.cpp ----

// Register the class gate on a client's rig record, unless its red arm is set. Idempotent.
void RegisterRecordGate();

// HOST: whether any accepted record still waits to be seen on its rig; and the rig's numbers, which tell it (a record whose
// rig ended is dropped there).
bool RecordsWait();
void NoteRecordSeen(void* rig, const ue_wrap::miner::Fields& now);

void ClearRecord();

}  // namespace coop::miner_sync::detail

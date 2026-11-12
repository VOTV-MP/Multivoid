// coop/interactables/atv_host_sync.h -- what of an ATV is the host's beyond its rig, which stays with its driver
// (atv_sync.h): its fuel and health, its modules and its horn. A client's own act runs on its copy and is sent to the
// host, which does it on its own copy and sends every ATV's state back:
//   - a refuel or repair is the rise its copy shows, a gain the host adds within reach; a client driver only lowers them;
//   - a module fitted or taken off is written on the host's Modules, a refused fit refunded with its prop at the ATV;
//   - the seated driver's horn, which the host and every other peer play.
// The seated driver's lights, brake and turbo are not here: they ride its condition block's stateBits (atv_sync.h).
// The state goes when any of it changes, every 2 s and to a joiner; one sent before the host heard of a client's own act
// waits 1.5 s on that client. Game thread. Overview: docs/vehicles.md.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct AtvHostPayload;
}  // namespace coop::net

namespace coop::atv_host_sync {

// Store the session. Idempotent; called every load tick.
void Install(coop::net::Session* session);

// The poll and the inboxes, on either role. Game thread.
void Tick();

// A client's receiver: the host's state of an ATV, or its horn.
void OnState(const coop::net::AtvHostPayload& p);

// The host's receiver: a client's gain, module change or horn.
void OnRequest(int senderSlot, const coop::net::AtvHostPayload& p);

// Every ATV again for a joiner.
void QueueConnectBroadcastForSlot(int slot);

// The session ended: everything is forgotten.
void OnDisconnect();

// The state sequence of an ATV, per ATV and per session, compared by serial arithmetic ((int32)(a - b)). HOST: the
// `stateSeq` its newest AtvHostState carried, 0 until it has published one; the first is 1. The syncer's vitals reach the
// host only in a block whose ackSeq is this number.
uint32_t CurrentStateSeq(const std::wstring& key);

// CLIENT: the newest state sequence whose vitals this client APPLIED for `key`, 0 until the first. The client syncer's
// blocks carry it as ackSeq; NoteAppliedStateSeq keeps the newer of the two by serial arithmetic.
uint32_t AppliedStateSeq(const std::wstring& key);
void NoteAppliedStateSeq(const std::wstring& key, uint32_t seq);

// CLIENT: whether the open-request gate of `key` is shut, a request of this client's own on that ATV unanswered; the
// vitals of a state or a block are then not applied and no sequence is recorded.
bool VitalsGated(const std::wstring& key);

// [dev] The horns this peer played for another peer's press.
uint32_t HornsPlayed();

}  // namespace coop::atv_host_sync

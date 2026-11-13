// coop/interactables/atv_host_sync.h -- what of an ATV is the host's beyond its rig, which stays with its syncer
// (atv_sync.h): its fuel and health, its modules and its horn. The host's copy holds them. A client's own act runs on its
// copy and is asked of the host (atv_host_judge.h), which judges it, writes its own copy and answers; this module is the
// state side:
//   - the host's ONE publish point: when a write to the host copy changed its fuel, health, modules, empty or broken, the
//     ATV's state sequence is bumped and an AtvHostState goes to every client; nothing is sent on a timer;
//   - a joiner at its world-ready edge gets every ATV's state, that slot alone;
//   - a client applies a state to its copy of the ATV.
// The seated driver's lights, brake and turbo are not here: they ride its condition block's stateBits (atv_sync.h).
// Game thread. Overview: docs/vehicles.md.

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

// The session while it is connected, else null. Game thread.
coop::net::Session* LiveSession();

// Per tick, on either role: the host serves its requests (atv_host_judge.h); a world change forgets what was the old
// world's.
void Tick();

// A client's receiver: the host's state of an ATV.
void OnState(const coop::net::AtvHostPayload& p);

// Every ATV's state again to the joiner in `slot`, that slot alone (the connect replay's world-ready edge).
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

// What the state carries of an ATV: its vitals, the two marks and the module values.
struct Vitals {
    float   fuel = 0.f, health = 0.f;
    uint8_t flags = 0;                 // kAtvHostEmpty, kAtvHostBroken
    uint8_t count = 0;                 // how many of `modules` are fitted
    uint8_t modules[16] = {};
    bool operator==(const Vitals& o) const;
};

// The copy's vitals; false when the ATV, or its modules, do not read. Game thread.
bool ReadVitals(void* atv, Vitals& out);

// `out`'s key, state fields and sequence from `v` and the current sequence of `key`, under `opcode`. The caller sets
// what is its own (an answer's request id and verdict).
void FillState(const std::wstring& key, const Vitals& v, uint8_t opcode, coop::net::AtvHostPayload& out);

// HOST, the one publish point: when `atv`'s vitals now differ from `before`, which the caller read ahead of its write,
// bump the ATV's state sequence and send its state to every client (the sender of the request that caused it too: its
// open-request gate holds it until the answer). True when it sent. A call that changed nothing sends nothing. Game thread.
bool PublishIfChanged(const std::wstring& key, void* atv, const Vitals& before);

// HOST: a horn edge of `key`'s ATV to every client but `skipSlot` (-1: none).
void SendHorn(const std::wstring& key, bool pressed, int skipSlot);

// Our own writes to an ATV (a state applied, a request applied, a horn replayed) run inside one of these.
class ApplyScope {
public:
    ApplyScope();
    ~ApplyScope();
    ApplyScope(const ApplyScope&) = delete;
    ApplyScope& operator=(const ApplyScope&) = delete;
};

// [dev] The horns this peer played for another peer's press.
void NoteHornPlayed();
uint32_t HornsPlayed();

}  // namespace coop::atv_host_sync

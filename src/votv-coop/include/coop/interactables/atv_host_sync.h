// coop/interactables/atv_host_sync.h -- what of an ATV is the host's beyond its rig, which stays with its syncer
// (atv_sync.h): its fuel and health and its modules. The host's copy holds them. A client's own act runs on its
// copy and is asked of the host (atv_host_judge.h), which judges it, writes its own copy and answers; this module is the
// state side:
//   - the host's ONE publish point: when a write to the host copy changed its fuel, health, modules, empty or broken, the
//     ATV's state sequence is bumped and an AtvHostState goes to every client; nothing is sent on a timer;
//   - a joiner at its world-ready edge gets every ATV's state, that slot alone;
//   - a client applies a state to its copy of the ATV unless its open-request gate on that ATV is shut; a state for an ATV
//     it holds no copy of yet is kept (the newest per ATV, 8 ATVs) and applied at the copy's adoption.
// The seated driver's lights, brake, turbo and horn are not here: they ride its condition block's stateBits (atv_sync.h).
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
// open-request gate holds it until the answer). True when it sent. A call that changed nothing sends nothing; one that
// changed something sends, whatever an earlier state carried, since the sequence it bumps is what the clients' acks are
// compared with. A verb that runs inside another (playerUsedOn calls toolboxFix) is published once, by atv_gate at the
// outermost one's end. Game thread.
bool PublishIfChanged(const std::wstring& key, void* atv, const Vitals& before);

// Our own writes to an ATV (a state applied, a request applied, a horn replayed) run inside one of these.
class ApplyScope {
public:
    ApplyScope();
    ~ApplyScope();
    ApplyScope(const ApplyScope&) = delete;
    ApplyScope& operator=(const ApplyScope&) = delete;
};

bool Applying();

// CLIENT: whether this client's open requests are at their cap, 4 on `atv` or 8 over all ATVs (the host's queue per sender).
// atv_gate's PRE reads it and cancels the verb, so nothing irreversible happens on a copy for an act no request answers. False
// on a host, and while no request is open (no key is built then). Game thread.
bool RequestCapReached(void* atv);

// CLIENT, the open-request gate (per ATV, at most 4 requests open, 8 over all ATVs: the host's queue per sender): register
// a request id for `key`, then send the request (atv_host_judge.h). Returns the id, or 0 when nothing was sent: a 5th
// request on one ATV and a 9th overall are refused here and said once (the cap is the PRE's, above, so this is the backstop
// for a verb nested in another), the adoption's sync (kSync) being exempt from the total, as is a session that is not a connected client's. The gate stays shut while any id is open; an answer
// removes its id and the state it carries is applied, with its sequence recorded, when it closes the last. The other
// exits are the ATV's destroy (NoteAtvGone), the session's end and a world change, each clearing the set and saying an
// unanswered id once. An answer for an id not open is dropped and counted.
uint32_t Ask(const std::wstring& key, uint8_t opcode, float amount, uint8_t module);

// CLIENT: this ATV left the index (its destroy), or entered it (adoption: a state held for it is applied now, and a copy
// that took no state asks the host for it: request op kSync).
void NoteAtvGone(const std::wstring& key);
void NoteAtvIndexed(const std::wstring& key, void* atv);

// [dev] CLIENT, the ATV drill's fixture: the states for `key` (the newest of them) are held, not applied, until this copy
// builds its next condition block (NoteBlockBuilt, from the block's fill), so that block carries the ack of the state
// before it. An empty key disarms and applies a state still held.
void HoldNextStateForDrill(const std::wstring& key);
void NoteBlockBuilt(const std::wstring& key);

// [dev] CLIENT, the ATV drill's fixture on the park's hand-back (atv_spawn_park): whether the host's state for `key`, an ATV
// with no copy here, has been decided on: kept for the copy's adoption, or dropped by the red atv_joiner_rows_off. Game thread.
bool UnadoptedDecidedForDrill(const std::wstring& key);

}  // namespace coop::atv_host_sync

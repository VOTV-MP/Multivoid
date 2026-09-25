// coop/interactables/keypad_sync.h -- the password keypads' lane: the index, the wire and the
// receiver's apply. Talks to the engine only through ue_wrap::passwordlock.
//
// The host's copy of a keypad is the one that decides. What it does reaches every client as a
// KeypadState record: a verb its copy ran (a digit, an open with its verdict, the guesser, the
// set-new-code mode, a false entry), which the client replays on its own copy so the keypad's own
// chain plays its sounds and lands the same state, or the state a chain settled on, which the
// client writes and hands on through the keypad's setActive. The records are authored at the
// keypad's verbs (coop/interactables/keypad_verbs), which also refuse a client's own verbs and send
// its player's entries to the host. Not an interactable_sync toggle channel: a keypad carries a
// typed buffer beside its verdict, and its submit is a verb, not a setter.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::keypad_sync {

// Stores the session and registers the index with the scan hub. Idempotent. Game thread.
void Install(coop::net::Session* session);

// The throttled retry of a state that arrived before its keypad was indexed. Game thread, once
// per pump tick.
void Tick();

// CLIENT: a record from the host, its format already checked by the dispatcher. Game thread.
void OnReliable(const coop::net::KeypadSyncPayload& payload);

// HOST: each indexed keypad's state to a joiner at its world-ready replay. Game thread.
void QueueConnectBroadcastForSlot(int peerSlot);

// The session ended: the pending states and the counters, with one summary line.
void OnDisconnect();

// The lane's name for a keypad, or empty when the lane does not index it. Game thread.
std::wstring KeypadKey(void* lock);

// The keypad the lane indexes under `key`, or null. Game thread.
void* ResolveKeypad(const std::wstring& key);

// The verbs the lane runs on a client's copy. Applying says whether the lane is running `verb` on
// `lock` right now; ApplyingAny whether it is running any verb on it. A verb the keypad's own body
// calls inside one the lane runs (the five-digit submit inside a replayed digit) is not the lane's
// apply of it. Game thread.
enum class Verb : uint8_t { InputNumber, Open, Open2, Reset, FalseEnter, SetActive };
bool Applying(void* lock, Verb verb);
bool ApplyingAny(void* lock);

// HOST: what `lock` is doing, to every client -- a verb it is about to run (from the verb's own
// watch, before its body), or its settled state (after its setActive(false)). Game thread.
void SendEvent(void* lock, coop::net::KeypadEvent event, uint8_t arg);
void SendState(void* lock);

// CLIENT: `lock`'s own chain ended at its setActive(false): a state that arrived while the chain was
// in its wait is written now. Game thread.
void OnClientChainEnd(void* lock);

}  // namespace coop::keypad_sync

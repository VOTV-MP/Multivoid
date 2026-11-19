// coop/props/prop_pry_sync.h -- a crowbar's pry of a prop that carries no wall-attach component (a plank), mirrored across
// peers: one sender, the host.
//
// A prying crowbar ends in the pried object's own crowbarOpen. A wall-attach component's owner frees itself there through
// the component's unstick, which coop/props/prop_stick_sync carries, and a door's pry is the door lane's
// (coop/interactables/door_verb_intent). Every other pryable prop's body changes the prop it runs on, and a client never
// runs it: a script-gate watch on crowbarOpen cancels the body on a client and asks the host with PryAsk, which the
// host's judge (coop/props/prop_pry_host) answers by running the pry on its own copy. Whoever the pry ran on, the host
// records the prop as pried and tells every client with PryState; a client takes the state, never the verb. A second
// crowbarOpen on a pried key is cancelled on either role: a minigame that finishes after the host granted another's pry.
// MTA precedent: a client's request is judged by the server, which then runs it and tells everyone
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3338-3367). docs/props.md carries the lane.

#pragma once

#include <cstdint>
#include <string>

namespace coop::net {
class Session;
struct PryStatePayload;
}  // namespace coop::net

namespace coop::prop_pry_sync {

// Idempotent: caches `session` and registers the script-gate watch on crowbarOpen, once.
void Install(coop::net::Session* session);

// The key a pry names a prop by: the prop's own Key, empty when it has none.
std::wstring KeyOf(void* prop);

// Whether this peer's record says `key` is pried: on the host, a pry it ran or granted; on a client, a PryState it took.
// Game thread.
bool IsPried(const std::wstring& key);

// HOST: `prop`'s pry has run on the host's copy (its own crowbarOpen's end, or a grant). Records the key as pried and
// sends PryState to every client, once: a second mark of a pried key is a no-op. Game thread.
void MarkPried(void* prop);

// CLIENT: the host's state of one pried prop, its format checked by the dispatcher. This peer's copy takes the pry the
// first time the state says pried. A key that resolves to no prop here is counted and dropped. Game thread.
void OnState(const coop::net::PryStatePayload& payload);

// Counters for the session-end line and the drills.
struct Stats {
    uint64_t sent;           // pried states this peer sent (host)
    uint64_t applied;        // pried states this peer applied to its copy (client)
    uint64_t asked;          // asks this peer sent the host (client)
    uint64_t twiceCancelled; // a crowbarOpen of a key already pried, cancelled (either role)
    uint64_t loadNative;     // a client's calls run natively as its own world's load
    uint64_t unresolved;     // a state naming no prop here, dropped
};
Stats GetStats();

// Session end: the counters are said and cleared, and so are the records.
void OnDisconnect();

}  // namespace coop::prop_pry_sync

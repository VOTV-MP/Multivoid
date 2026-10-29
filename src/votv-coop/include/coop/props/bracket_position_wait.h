// coop/props/bracket_position_wait.h -- inside the snapshot bracket a position bind waits for the bracket's end.
//
// A host spawn whose exact key missed and whose same-class query found a neighbour within the fuzzy radius used to
// bind that neighbour on arrival. A neighbour whose own key has not arrived yet reads as unbound, so the order the
// bracket carried its eids in decided whether a key kept its actor. Between the bracket's Begin and its Complete
// such a spawn now waits instead: its payload and sender slot are kept, its eid is marked deferred in bind_wait so
// the messages that need the bind park in the one owner of such messages, and at Complete the waiting spawns are
// matched to the free candidates nearest first and adopted through remote_prop_spawn::AdoptByPosition. A spawn left
// with no free candidate takes the ordinary spawn path, which fresh-spawns.
//
// A keyed prop's release, pose and stick state carry no eid, so bind_wait cannot hold them: the receivers hand the
// newest of each to KeepIfWaiting at their key miss, and the drain replays them after the bind.
//
// Only the wire receive edge waits (the convert and kerfur callers of OnSpawn never do), and only on a client. A spawn
// after Complete never waits. The dev row bracket_position_wait_off turns the wait off whole. Game thread.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::net {
struct PropSpawnPayload;
struct WireKey;
}  // namespace coop::net

namespace coop::props::bracket_position_wait {

// Which kept message: the newest of each kind is kept per waiting key.
enum class Kept : uint8_t { Release, Pose, Stick };

// The bracket opened: any earlier wait is dropped (an aborted bracket re-sends its spawns), and the wait is armed.
void BeginBracket();

// True while a spawn arriving now would wait: the bracket is open and the dev row is off.
bool IsWaiting();

// How many spawns wait now; 0 once the bracket is drained. Read-only, for the dev drill.
size_t WaitingCount();

// Keep the spawn until the bracket's end. A second spawn of the same eid replaces the first. False (said once per
// bracket) when 256 spawns wait already: the caller binds this one on arrival.
bool Wait(const coop::net::PropSpawnPayload& payload, int senderSlot);

// A destroy of a waiting eid, or (eid 0 or invalid) of a waiting key, drops the wait. True when one was dropped.
bool OnDestroy(uint32_t eid, const coop::net::WireKey& key);

// A receiver's key miss: when `key` is a waiting key, keep the newest `bytes` of `what` with its sender slot and
// return true, so the receiver ends its apply. False when nothing waits on the key. The relay to other peers has
// run before the receiver reaches its miss.
bool KeepIfWaiting(const coop::net::WireKey& key, Kept what, const void* bytes, size_t n, int senderSlot);

// The bracket closed: the flag drops first, then a spawn whose exact key resolves now takes the spawn path, every
// other waiting spawn is matched and bound (nearest first), the rest take the spawn path, and the kept releases,
// poses and stick states replay in arrival order. One log line. `localPlayer` is the
// live AmainPlayer_C, possibly null.
void DrainAtComplete(void* localPlayer);

// Drop every wait and give up the bind_wait mark of each: the session ended, the world changed, a bracket began.
void Reset();

}  // namespace coop::props::bracket_position_wait

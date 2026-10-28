// coop/props/container_park.h -- the container READINESS pen: the inbound holding pen for a container slice
// whose container is on this peer but cannot take it yet (its inventory or slice slot does not resolve), and, on
// the host, for a slice whose author has no body to measure yet or whose birth slice is past its arrival budget.
// A local wait on an actor that exists, not a bind: a slice for a container with NO actor here waits in
// bind_wait (coop/element/bind_wait), and no pen entry is a bind wait.
// One entry per eid, latest wins; the lane beside it
// (container_contents_sync) owns the parse and the apply and hands them back through the replay
// callback, so nothing here knows the blob's grammar. Game thread throughout.

#pragma once

#include <cstdint>
#include <vector>

namespace coop::props::container_park {

// How many entries one CLIENT author may hold at once. Reaching it evicts that author's oldest, so
// a newer slice is never refused in favour of a stale one. Host-authored parks are not capped: a client's pen
// holds HOST-authored slices, the world's truth, while a host's pen holds client-authored ones, where a
// peer names WHAT it writes and never what holding it costs.
inline constexpr size_t kMaxParksPerClient = 8;

// How long an entry may wait outside a join bracket before it is dropped as a leak guard. Park
// aging is EVENT-anchored, not wall clock from arrival: a readiness wait during a join is still a
// wait, so while a join snapshot is in flight parks do not age at all; at Complete every park is
// re-stamped and this clock runs from there.
inline constexpr uint64_t kTtlMs = 30'000;

// Hold this blob until its container is ready, on behalf of the peer that authored it. Latest wins per
// eid; the caller's clock, like every other bounded table in this tree.
//
// MTA drops a packet naming an element it does not know, with no else branch
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp, Packet_CustomData). We hold
// instead, because here the container usually becomes ready a moment later: a slice for one that
// has no actor yet waits in bind_wait, this pen is for the one that is here and not ready.
void Admit(uint32_t eid, uint8_t authorSlot, std::vector<uint8_t>&& blob, uint64_t nowMs);

// A slice for the eid applied directly: its older pen entry is erased, so it cannot apply over the newer state
// at the next sweep. Not for the replay callback of Sweep, whose entry the sweep retires itself.
void Erase(uint32_t eid);

// Re-offer each parked blob to its own author's arbitration. The callback returns false only when
// the container is STILL not ready; anything else (applied, refused, malformed) retires the
// entry.
using ReplayFn = bool (*)(const std::vector<uint8_t>& blob, uint8_t authorSlot);

// Replay what can be replayed, and drop what has waited past the TTL outside a join bracket.
void Sweep(ReplayFn replay, uint64_t nowMs);

// The join snapshot's bracket, from the client-side SnapshotBegin / SnapshotComplete dispatch.
void NoteJoinBracket(bool open, uint64_t nowMs);

size_t Size();

void Reset();

// The arithmetic selftest, un-gated at session start: the cap that evicts this author's own oldest
// and nobody else's, the host author that is not capped at all, the re-park that does not grow the
// table, the TTL that drops an entry only outside a bracket, an erase of one eid, and a replay that retires what it
// dealt with.
bool RunSelftest();

}  // namespace coop::props::container_park

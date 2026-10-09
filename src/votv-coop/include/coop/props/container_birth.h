// coop/props/container_birth.h -- a container that changes hands through the world. A throw respawns
// the container and the game's loadData rebinds it to its slot in the thrower's own GObjStack, firing
// neither addObject nor takeObj, and the drop intent carries no contents: the thrower's slice is the
// only copy of them, so it has to travel, and the host must not publish over it meanwhile. This
// module holds that transfer on both sides; coop/props/container_contents_sync ships, judges and
// applies the slices, and docs/devices.md carries the model.
//
// CLIENT: a container this peer threw is held until the host's echo binds it to an element; its
// slice is then owed to the host as a birth (container_slice_wire::kBirthBase).
// HOST: the copy built from a client's intent is a transfer in progress until that author's slice
// lands: not published, not seeded to a joiner, and every slice of that author for it is the newest
// version. It ends when the slice lands, when the author leaves, or at its deadline.
// BOTH: a first publication owed for a container whose slot may not be readable yet is retried until
// its deadline instead of being taken as nothing to send. Game thread; held actors are CachedObjRefs.

#pragma once

#include <cstdint>
#include <set>

namespace coop::props::container_birth {

// How long a transfer waits for its other half: the client for the host's echo, the host for the
// author's slice, either side for a slot to become readable. Long against an echo (a round trip and
// a 250 ms sweep), short against a session.
inline constexpr uint64_t kTtlMs = 30000;
// Transfers in flight per author. Throws are human-rate, so the cap only bounds a peer that floods,
// and that peer's oldest goes, never another author's.
inline constexpr size_t kMaxPerAuthor = 64;

// CLIENT, prop_drop_intent, at the drop intent for a container this peer threw.
void NoteAuthoredBirth(void* actor);
// HOST, prop_drop_intent, at the spawn from a client's intent: the copy is empty and awaited.
void ExpectBirthSlice(void* actor, uint8_t authorSlot);
// HOST, host_spawn_watcher, when its drain takes the host's own birth: a container with contents
// publishes them, since the mirrors are born empty. One awaited from a client says nothing.
void NoteHostBirth(void* actor);
// HOST: the transfers this slot authored end with it.
void OnPeerGone(uint8_t slot);

// HOST: whether `actor` is an awaited transfer, from this author or (authorSlot 0) from any.
bool Awaited(void* actor, uint8_t authorSlot);
// HOST: the author's slice landed; the transfer ends.
void Complete(void* actor);

// Per contents sweep. CLIENT: every thrown container the echo has bound goes into `dirty`, owed as a
// birth. HOST: a transfer past its deadline ends, said; a host birth with contents goes into `dirty`.
void Sweep(uint64_t nowMs, std::set<uint32_t>& dirty);

// CLIENT: true when this eid's next fan-out is its birth slice.
bool IsBirthSlice(uint32_t eid);

// What an unreadable container means for its eid now: nothing owed (nothing to send), still owed
// (retry), or owed past its deadline (given up: the row is gone, and the caller says so).
enum class Owed : uint8_t { None, Pending, GivenUp };
Owed FirstOwed(uint32_t eid, uint64_t nowMs);
// The first publication of `eid` went to every peer.
void FirstSent(uint32_t eid);
// `eid` no longer resolves: whatever was owed for it goes.
void Forget(uint32_t eid);

void Reset();

// The transfer's arithmetic, un-gated at session start before any transfer exists: the per-author cap
// that evicts that author's own oldest and nobody else's, a departing author's transfers, a gone
// container's at the sweep, and a first publication pending, then given up once. No engine object is
// touched. Leaves the tables empty.
bool RunSelftest();

}  // namespace coop::props::container_birth

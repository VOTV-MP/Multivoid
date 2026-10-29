// coop/props/container_contents_internal.h -- what the two halves of the contents lane give each other and container_truth:
// container_contents_sync.cpp (the ingest half: the apply, the pens, the connect seed) and container_contents_publish.cpp
// (the host-to-peers half: the dirty drain, the broadcast, the relay). Also the role, the clock, the world-container resolve
// and the targeted send. Not an API: only those files include it. Game thread.

#pragma once

#include <cstdint>
#include <set>
#include <vector>

namespace coop::net { class Session; }

namespace coop::props::container_contents_sync::internal {

bool IsHostRole();
uint64_t NowMs();

// The inventory of the live world container `eid` names, or null.
void* LiveWorldInventory(uint32_t eid);

// The container's live contents to one slot, forced past the unchanged-hash guard. False when refused.
bool SendContainerTo(uint32_t eid, void* inv, int slot);

// What was last sent or applied for one container: the hash of its records and the flags byte that went with them.
struct Seen { uint64_t records = 0; uint8_t flags = 0; };

// ---- the publish half (container_contents_publish.cpp) ----

// Broadcast one container's slice: to every world-ready peer (toSlot < 0) or to one slot. Unchanged since the last fan-out
// it sends nothing unless `force`. False if the send was refused while a world-ready peer was present, or a first
// publication is not readable yet (the caller arms the retry).
bool BroadcastContainer(coop::net::Session* s, uint32_t eid, void* inv, int toSlot, bool force);

// Host: an accepted client-authored slice on to every other peer, never back to its author.
void RelayToOthers(coop::net::Session* s, uint8_t authorSlot, const std::vector<uint8_t>& blob);

// The sweep's drain of the dirty set (and of the refused sends it retries).
void DrainDirty(coop::net::Session* s);

// The dirty set: marked by a write, drained by the sweep, and swept by the birth lane's pen.
void MarkDirty(uint32_t eid);
std::set<uint32_t>& Dirty();

// What every peer has: set after a fan-out or an accepted birth slice, forgotten when a baseline may not stand.
void NoteSent(uint32_t eid, const Seen& seen);
void ForgetSent(uint32_t eid);

// The session's end: the dirty set, the retries, the dev counters and the sent baselines.
void ResetPublish();

}  // namespace coop::props::container_contents_sync::internal

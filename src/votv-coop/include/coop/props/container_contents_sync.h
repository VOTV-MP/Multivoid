// coop/props/container_contents_sync.h -- a world container's CONTENTS, published by the host. docs/devices.md carries
// the model. A client authors one slice kind, a thrown container's birth (coop/props/container_birth, with its arrival
// bound); the host refuses every other client slice. A slice for a container with no actor waits in bind_wait, one for
// a container not ready in container_park, the bytes are container_slice_wire.
//
// One rule restates HERE, because breaking it destroys player data: only a world container's inventoryComponent is
// written (ue_wrap::container_inventory::IsWorldInventory); a player's own inventoryComponent_player, and any
// component whose layout will not resolve, is SKIPPED, since a write over it replaces that player's inventory.
//
// The edge is coop/props/container_writer_watch (script_gate on the inventory component's addItemAsObject,
// addItemAdvanced, removeInventoryItem, trimInventory and transferFromInvToInf, and the inventory screen's dropItems),
// marking the OWNING actor's eid dirty through MarkHostWrite on the host; a client's mirror is display only, a verb
// on it marks nothing. Apply writes the receiver's own canvasInventory entries (ue_wrap::container_inventory::
// WriteContents), then re-derives the volumes through calculateVolumes; addItemAsObject cannot be the apply verb (it
// takes a live AActor* and serialises it itself).

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <vector>

namespace coop::net { class Session; }

namespace coop::props::container_contents_sync {

void Install(coop::net::Session* session);

// Drives the writer watches' registration (container_writer_watch: the inventory writers),
// the FName resolve and the dirty-set drain. Near-free when the dirty set is empty, the steady state.
void Tick();

// The seam from the edge: this peer wrote the container `eid` (container_writer_watch). Marks it
// dirty for the next sweep and drops the applied-hash no-op guard. Game thread.
void MarkHostWrite(uint32_t eid);

// The next host publication of this container applies even when it is identical to the last, because the client's
// copy may no longer match it. Erases the applied hash and nothing else (no dirty mark, no touch). Game thread.
void ForgetApplied(uint32_t eid);

// ContainerContents chunks: a BlobChunkPayload whose blob is
// `[u8 op=0][u32 eid][u16 n]` then n entries in the coop/items/inventory_item_wire
// grammar. Never refanned -- a client accepts slot 0 and nothing else, and the host accepts only a
// non-zero slot, which is a client-authored slice it takes as a thrown container's birth and refuses otherwise.
void OnContentsChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// Host: ship the joiner one contents blob per live world container. The join save-transfer blob is
// a snapshot taken at request time; this is the anchor that corrects it.
void QueueConnectBroadcastForSlot(int peerSlot);

// The readiness pen's aging is anchored to the client's own join-snapshot bracket: its entries do not age while
// it is open, since a readiness wait during a join is still a wait and a slow link can exceed any fixed TTL with
// no loss at all. At Complete every entry is re-stamped and the TTL runs as a leak guard only. Called from the
// client-side snapshot dispatch.
void NoteJoinSnapshotBracket(bool open);

void OnDisconnect();

// ---- dev-instrument seams (the dev probes) ------------------------
// These exist so the instrument reuses this module's measured world-versus-personal boundary
// instead of reimplementing it. A probe free to get boundary 1 wrong would be testing a different
// rule than the one that ships.

struct WorldContainer {
    uint32_t eid;
    void* actor;
    void* inv;    // the propInventory_C component
};

// Fill `out` with up to `want` live world containers, boundary 1 applied. Host or client.
size_t SnapshotWorldContainers(WorldContainer* out, size_t want);

// The observable digest for one container: how many records its slice holds and the currVol the
// engine reports. Both peers print it; the smoke compares the numbers. False if the eid is not a
// live world container here.
bool ContentsDigest(uint32_t eid, int32_t& outCount, float& outVol);

// The hash of one container's records (coop/props/container_slice_wire::RecordsHash) and its record count, for the parity
// report. False when the eid is not a
// live world container here, its contents do not read, or a transfer into it is in progress. Game thread.
bool ContentHashOf(uint32_t eid, uint64_t& outHash, uint32_t& outRecords);

// Dev (the route census drill): the hash of each record of one container as the lane publishes it
// (RecordsHash over that record alone), in order, and of the whole slice's records. False as ContentHashOf.
// Game thread.
bool DevRecordHashes(uint32_t eid, std::vector<uint64_t>& out, uint64_t& whole);

// Dev (the route census drill): one raw write to a container's contents the way an accepted client slice is applied
// -- the first record appended once more -- and, when `publish`, marked for the sweep like any local write; unpublished,
// the host's contents move ahead of every client's base. False when the contents do not read or are empty. Game thread.
bool DevRawAppend(uint32_t eid, bool publish);

// Dev (the route census drill): as DevRawAppend, with the entry of a live item (its own betterGetData, as addItemAsObject
// files it) appended instead of a copy of the first. False as DevRawAppend, or when the item's entry does not read.
bool DevRawAppendActor(uint32_t eid, void* item, bool publish);

// Host: send this container's live contents to one slot, forced past the unchanged-hash guard -- the truth a refused
// client converges on. False when this is not the host, the container is not live, or the send was refused; true
// means the send was not refused, never a delivery. Unthrottled: for a send owed once (a put commit's, one per hold);
// a refusal the sender can repeat uses container_truth::SendTruthBounded. Game thread.
bool SendTruthToSlot(uint32_t eid, int slot);

// Call fn once per live world container, boundary 1 applied: the set the connect seed walks. Game thread.
void ForEachWorldContainer(void (*fn)(uint32_t eid, void* ctx), void* ctx);

// How many containers this peer holds an applied host slice for (the size of its applied-hash map); the late-join
// drill reads it. Game thread.
size_t AppliedEids();

// Fan-out sends refused with no world-ready peer: owed to the connect replay, not re-armed. Reset at session end.
uint64_t NoReadyRefusals();

// Those same refusals re-armed anyway (container_retry_red on; 0 otherwise). Reset at session end.
uint64_t NoReadyRetries();

// Did this container's fan-out meet a refusal with no world-ready peer? Reset at session end.
bool NoReadyRefusedEid(uint32_t eid);

// Does the host hold a "what every peer has" hash for this container (a fan-out succeeded for it)?
bool SentBaselineKnown(uint32_t eid);

// Dev: how often this lane touched the container: a local write (MarkHostWrite), an applied remote slice, a connect
// seed's send. 0 when it never has. Game thread; reset at session end.
uint32_t TouchEpochOf(uint32_t eid);

}  // namespace coop::props::container_contents_sync

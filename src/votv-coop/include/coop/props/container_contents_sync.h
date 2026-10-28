// coop/props/container_contents_sync.h -- a world container's CONTENTS, authored by the peer whose
// verb fired and arbitrated by the host. docs/devices.md carries the model; the invariants a
// client-authored slice must pass are coop/props/container_write_policy, a slice for a container with no actor
// waits in bind_wait, one for a container not ready in container_park, the bytes are container_slice_wire.
//
// Two rules restate HERE, because breaking either destroys player data: a container whose
// propInventory_C.Player reads true, OR whose offset will not resolve, is SKIPPED (personal
// inventory shares one global GObjStack array, and a write over another peer's slice wipes that
// player's inventory); and a nested container's own index rides as the sentinel -1, on the way out
// AND on the way in, since a real value there names a slot in the SENDER's array.
//
// The edge is coop/props/container_writer_watch (ue_wrap/core/script_gate on addObject and takeObj),
// marking the OWNING actor's eid dirty through MarkHostWrite. Apply raw-writes the receiver's own GObjStack slot, then re-derives the setter-managed
// state through updateVolumesAndMass and recalculateNames; addObject cannot be the apply verb (it
// takes a live AActor* and serialises it itself) and checkObjectsVolume is not called (it ejects).

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <vector>

namespace coop::net { class Session; }

namespace coop::props::container_contents_sync {

void Install(coop::net::Session* session);

// Drives the writer watches' registration (container_writer_watch: the verbs, and the host's drone
// sale), the FName resolve and the dirty-set drain. Near-free when the dirty set is empty, the steady state.
void Tick();

// The seam from the edge: this peer wrote the container `eid` (container_writer_watch). Marks it
// dirty for the next sweep and drops the applied-hash no-op guard. Game thread.
void MarkHostWrite(uint32_t eid);

// ContainerContents chunks: a BlobChunkPayload whose blob is
// `[u8 op=0][u32 eid][u64 baseHash][u16 n]` then n records in the coop/items/save_record_wire
// grammar. Never refanned -- a client accepts slot 0 and nothing else, and the host accepts only a
// non-zero slot, which is a client-authored slice it arbitrates.
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

// ---- dev-instrument seams (coop/dev/container_selftest) ------------------------
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

// The hash the wire uses for one container's contents (coop/props/container_slice_wire::ContentHash) and its record
// count, for the parity report. False when the eid is not a live world container here, its contents do not read, or a
// transfer into it is in progress. Game thread.
bool ContentHashOf(uint32_t eid, uint64_t& outHash, uint32_t& outRecords);

// Dev (the route census drill): the content hash of each record of one container as the lane publishes it
// (ContentHash over that record alone), in order, and of the whole slice. False as ContentHashOf. Game thread.
bool DevRecordHashes(uint32_t eid, std::vector<uint64_t>& out, uint64_t& whole);

// Dev (the route census drill): one raw write to a container's contents the way an accepted client slice is applied
// -- the first record appended once more -- and, when `publish`, marked for the sweep like any local write; unpublished,
// the host's contents move ahead of every client's base. False when the contents do not read or are empty. Game thread.
bool DevRawAppend(uint32_t eid, bool publish);

// Dev (the route census drill): as DevRawAppend, with the record of a live item (its own getData, as addObject files
// it) appended instead of a copy of the first. False as DevRawAppend, or when the item's record does not read.
bool DevRawAppendActor(uint32_t eid, void* item, bool publish);

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

// Dev: what the host's arbitration last decided about a client-authored slice for this container (None: no slice was
// judged since the session began). A slice held for a body not yet posed is not a judgement. Host; reset at session end.
enum class ClientSliceOutcome : uint8_t { None, Refused, Accepted };
ClientSliceOutcome LastClientSliceOutcome(uint32_t eid);

}  // namespace coop::props::container_contents_sync

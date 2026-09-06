// coop/props/container_contents_sync.h -- a world container's CONTENTS, authored by the peer whose
// verb fired and arbitrated by the host. docs/devices.md carries the model. Two rules restate here,
// because breaking either destroys player data: a container whose propInventory_C.Player reads
// true, OR whose offset will not resolve, is SKIPPED (personal inventory shares the same global
// GObjStack array, and a write over another peer's slice wipes that player's inventory); and a
// nested container ships with ints[] CLEARED, since its index names a slot in the SENDER's array.
// The edge is ue_wrap/core/vm_dispatch on addObject and takeObj, marking the component dirty. Apply
// raw-writes the receiver's own GObjStack slot, then re-derives the setter-managed state through
// updateVolumesAndMass and recalculateNames; addObject cannot be the apply verb (it takes a live
// AActor* and serialises it itself) and checkObjectsVolume is not called (an overflow ejector, it
// would destroy contents). Wire: ReliableKind::ContainerContents, a BlobChunkPayload whose blob is
// [u8 op=0][u32 eid][u16 n] then n records in the coop/items/save_record_wire grammar. Never
// refanned; receivers accept senderSlot 0 only. Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::props::container_contents_sync {

void Install(coop::net::Session* session);

// Registers the mutating verbs on first call, then drives the FName resolve and the dirty-set
// drain. Near-free when the dirty set is empty, the steady state.
void Tick();

// Read and clear the takeObj-in-flight latch, armed at the dispatch edge.
// prop_drop_intent::OnClientFinishSpawn consumes it to mark the extracted item's spawn as a
// container extraction. Game thread.
bool TakeObjInFlight();

// ContainerContents chunks.
void OnContentsChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// Host: ship the joiner one contents blob per live world container. The join save-transfer blob is
// a snapshot taken at request time; this is the anchor that corrects it.
void QueueConnectBroadcastForSlot(int peerSlot);

// Park aging is anchored to the client's own join-snapshot bracket: parks do not age while it is
// open, because a contents slice systematically precedes its PropSpawn under backpressure and a
// slow link can exceed any fixed TTL with no loss at all. At Complete every park is re-stamped and
// the TTL runs as a leak guard only. Called from the client-side snapshot dispatch.
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

}  // namespace coop::props::container_contents_sync

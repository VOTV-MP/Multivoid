// coop/props/container_contents_sync.h -- a world container's CONTENTS, authored by the peer whose
// verb fired and arbitrated by the host. docs/devices.md carries the model; the invariants a
// client-authored slice must pass are coop/props/container_write_policy, the pen that holds one it
// cannot judge yet is coop/props/container_park, the bytes are coop/props/container_slice_wire.
//
// Two rules restate HERE, because breaking either destroys player data: a container whose
// propInventory_C.Player reads true, OR whose offset will not resolve, is SKIPPED (personal
// inventory shares one global GObjStack array, and a write over another peer's slice wipes that
// player's inventory); and a nested container's own index rides as the sentinel -1, on the way out
// AND on the way in, since a real value there names a slot in the SENDER's array.
//
// The edge is ue_wrap/core/script_gate on addObject and takeObj, marking the OWNING actor's eid
// dirty. Apply raw-writes the receiver's own GObjStack slot, then re-derives the setter-managed
// state through updateVolumesAndMass and recalculateNames; addObject cannot be the apply verb (it
// takes a live AActor* and serialises it itself) and checkObjectsVolume is not called (it ejects).

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

// ContainerContents chunks: a BlobChunkPayload whose blob is
// `[u8 op=0][u32 eid][u64 baseHash][u16 n]` then n records in the coop/items/save_record_wire
// grammar. Never refanned -- a client accepts slot 0 and nothing else, and the host accepts only a
// non-zero slot, which is a client-authored slice it arbitrates.
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

// Has the addObject/takeObj watch fired on this peer at least once? The instrument's verdict
// needs it: an absent effect means one thing if the edge is live and another if it never was.
bool VerbWatchEntered();

// The observable digest for one container: how many records its slice holds and the currVol the
// engine reports. Both peers print it; the smoke compares the numbers. False if the eid is not a
// live world container here.
bool ContentsDigest(uint32_t eid, int32_t& outCount, float& outVol);

}  // namespace coop::props::container_contents_sync

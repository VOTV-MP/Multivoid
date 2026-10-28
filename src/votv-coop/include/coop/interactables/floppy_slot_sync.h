// coop/interactables/floppy_slot_sync.h -- a disc-holding device's slot as shared state: the server boxes and the
// laptop. An insert moves the disc into its device's fields and destroys the actor, so a slot on no wire loses discs:
// the other peer's device answers an eject with "No floppy disc in the slot", and a rejoin makes it permanent. So the
// slot is state, the host owns it, and a peer reports the outcome of a slot its own game changed. The laptop's digest
// is which disc it holds; its files between two such changes are its quad's (coop/interactables/laptop_buffer_sync),
// which carries this lane's generation. That is MTA's shape for an entity entering a container: the slot states are the
// server's, ship with the entity, and a client's transfer is a request the server answers
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp, VEHICLE_REQUEST_IN). We diverge
// on one point: the claim carries an outcome rather than asking -- insertFloppy and ejectFloppy
// have already run on the peer that reports them -- and the host's canonical is the answer a
// client applies over its own optimistic copy; two peers' changes crossing inside one answer are
// decided by the later claim (docs/base-computer.md, Known limits).

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

// Wire: ReliableKind::FloppySlotState (BlobChunkPayload; head [u8 op][u8 deviceKind], op 0=claim
// carrying one device, 1=canonical carrying a set). On the Default lane with PropDestroy, so a claim cannot
// overtake the destroy of the disc it absorbed. Never blindly relayed: a client's claim reaches
// other peers only as the host's canonical. Game thread throughout.
namespace coop::floppy_slot_sync {

void Install(coop::net::Session* session);

// 1 Hz, game thread: a poll of every device's slot behind floppy_slot::ReadDigest, which reads
// the raw field bytes and mints nothing; an empty slot hashes its type and stops, so the cost is
// set by the boxes that actually hold a disc. HOST: broadcast the canonical for
// a slot that moved, and re-send one whose send was refused. CLIENT: claim a slot that moved, and
// nothing else -- the host's canonical is what the client settles on.
void Tick();

// FloppySlotState chunks. The host takes a claim (bounded by size and by rate per sender),
// applies it to its own device and answers with the canonical, which is also the acknowledgement.
// A client takes canonicals from the host and drops everything else.
void OnChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST, at a joiner's ClientWorldReady edge: every device's slot, unconditionally. The joiner's
// world came from the host's save file, which the session froze before the first insert, so an
// empty slot is as much news as an occupied one. The laptop's file quad follows the laptop's set.
void QueueConnectBroadcastForSlot(int peerSlot);

// The laptop's generation: the host's count of changes of which disc its slot holds, a client's the last canonical's.
// Its file quad (coop/interactables/laptop_buffer_sync) carries it, so an edit meets only the disc it was made on.
uint32_t LaptopGeneration();

// Whether this peer's laptop occupancy is the one both ends agree on: nothing this lane has not taken, and on the host
// nothing unpublished, on a client nothing claimed and unanswered. The quad exchanges edits only while it holds.
bool LaptopOccupancySettled();

// [dev] CLIENT: the laptop's slot swept now rather than at the next 1 Hz sweep, so the laptop drill's claim and an edit
// after it land in one frame, before the host can answer. Game thread.
void DevClaimLaptopNow();

// A slot teardown: the leaver's half assemblies and rate window must not survive into whoever
// recycles that slot.
void OnPeerGone(uint8_t senderSlot);

void OnDisconnect();

}  // namespace coop::floppy_slot_sync

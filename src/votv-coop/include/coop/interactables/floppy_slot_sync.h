// coop/interactables/floppy_slot_sync.h -- a disc-holding device's slot as shared state.
//
// A signal server's slot was on no wire, and that is what loses discs: the insert moves the disc
// into four fields of one AserverBox_C and destroys the actor, the destroy is relayed, and the
// state that replaced the disc exists on one peer. The other peer's box then answers an eject
// with "No floppy disc in the slot", and since a joiner loads the host's save file, which coop
// stops the game refreshing, a rejoin makes the loss permanent.
//
// So the slot is state, the host owns it, and a peer reports the outcome of a slot its own game
// changed. That is MTA's shape for an entity entering a container: the slot states are the
// server's, ship with the entity, and a client's transfer is a request the server answers
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp, VEHICLE_REQUEST_IN). We diverge
// on one point: insertFloppy and ejectFloppy dispatch as EX_LocalVirtualFunction and no seam of
// ours can cancel one, so the claim carries an outcome rather than asking, and the host's
// canonical is the answer a client applies over its own optimistic copy.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

// Wire: ReliableKind::FloppySlotState (BlobChunkPayload; head [u8 op][u8 deviceKind], op 0=claim
// carrying one device, 1=canonical carrying a set). On PropSpawn's lane, so a claim cannot
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
// empty slot is as much news as an occupied one.
void QueueConnectBroadcastForSlot(int peerSlot);

// A slot teardown: the leaver's half assemblies and rate window must not survive into whoever
// recycles that slot.
void OnPeerGone(uint8_t senderSlot);

void OnDisconnect();

}  // namespace coop::floppy_slot_sync

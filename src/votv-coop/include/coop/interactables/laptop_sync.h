// coop/interactables/laptop_sync.h -- the stationary PC (Alaptop_C) power and floppy lane, and the
// LaptopState wire the portable PC's lid (coop/interactables/portable_pc_lid) rides as op 6.
//
// Edges are authored by the presser, there is ONE destroy owner -- the existing K2_DestroyActor
// seam -- the HOST is content authority, and a joiner gets ground-truth rows rather than a replayed
// history. The disc PROP lifecycle is NOT this lane's: an insert destroy already crosses on the
// generic destroy seam and an eject spawn on the birth channels. Game thread throughout, on the
// net-pump tick and the reliable dispatch.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::laptop_sync {

void Install(coop::net::Session* session);

// 4 Hz: resolve, the content streams' TTL sweep and a parked slot edge's lost-content fallback, the
// power target, and the power and slot edge polls. Two axes:
//   POWER   the isOpened edge, polled because every entry verb is EX-invisible. A
//           receiver replays the native actionOptionIndex(b8) under the wire-apply
//           echo guard when local differs from wire; one whose powered or anim gate
//           declines retries until it converges, and power_sync converges the
//           wall-power input.
//   FLOPPY  floppyType change edges, insert and eject, plus the slot scalars and
//           content strings.
// The portable PC (prop_portablePc_C) is a remote terminal to THIS laptop: its lid is
// coop/interactables/portable_pc_lid's, on op 6, and its buffer QUAD laptop_buffer_sync's, whose
// shadow prime piggybacks PrimeBaselines.
void Tick();

// Wire ingest (both roles). HOST: applies + re-fans (except origin). Op 6 goes to the lid lane.
void OnLaptopState(const coop::net::LaptopStatePayload& p, uint8_t senderSlot);

// LaptopBlob content chunks: the host re-fans a client's chunks one for one, then both roles
// assemble and apply. The blob is the laptop SLOT's content alone, pairing with the parked op=1/3
// edge; a disc's content is its save record, which coop/props/prop_save_data carries by Key.
void OnLaptopBlobChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST: ship the joiner the laptop's state (op=3) and, with a disc in its slot, the slot's content
// blob. The lid lane sends its own rows.
void QueueConnectBroadcastForSlot(int peerSlot);

void OnDisconnect();

}  // namespace coop::laptop_sync

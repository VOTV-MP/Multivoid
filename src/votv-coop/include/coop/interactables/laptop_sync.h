// coop/interactables/laptop_sync.h -- the stationary PC (Alaptop_C) power, floppy and lid lane.
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

// 4 Hz: resolve and the instance-generation check, the power and slot edge polls, the client's
// pending-eject-content drain, the deferred disc-content retries and the 1 Hz lid sweep. Three
// axes:
//   POWER   the isOpened edge, polled because every entry verb is EX-invisible. A
//           receiver replays the native actionOptionIndex(b8) under the wire-apply
//           echo guard when local differs from wire; one whose powered or anim gate
//           declines retries until it converges, and power_sync converges the
//           wall-power input.
//   FLOPPY  floppyType change edges, insert and eject, plus the slot scalars and
//           content strings.
//   LID     the portable PC (prop_portablePc_C) is a remote terminal to THIS laptop
//           with its own lid state: a 1 Hz element walk, idempotent any-peer lines,
//           a reflected Open() apply and join rows. Its buffer QUAD lives in
//           laptop_buffer_sync, whose shadow prime piggybacks PrimeBaselines.
void Tick();

// Wire ingest (both roles). HOST: applies + re-fans (except origin).
void OnLaptopState(const coop::net::LaptopStatePayload& p, uint8_t senderSlot);

// LaptopBlob content chunks: the host re-fans a client's chunks one for one, then both roles
// assemble and apply. The head is [kind][eid] -- kind 0 is slot content, pairing with the parked
// op=1/3 edge, kind 1 is disc content by eid. For a disc the HOST loadDatas its authoritative actor
// and re-fans; a receiver writes its mirror when that materializes, retrying until then, and a
// client-ejected disc's content travels client to host after the adoption eid-binding.
void OnLaptopBlobChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST: ship the joiner the full laptop state (op=3 + slot-content blob) +
// one disc-content blob per live content-bearing disc + the lid rows.
void QueueConnectBroadcastForSlot(int peerSlot);

void OnDisconnect();

}  // namespace coop::laptop_sync

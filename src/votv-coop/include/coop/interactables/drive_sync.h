// coop/drive_sync.h -- v119 L5: the drive-chain lanes.
//
//   DriveSlotState (109) -- idempotent per-slot FSM state lines (desk play/
//     comp + eraser slots; slot actors have NO eids -> keyed by role). ANY
//     peer announces its organic slot transitions (receiver-side overlap
//     SELF-SIMULATES inserts; ejects never self-sim); receivers pre-check
//     then apply (reflected putDriveIn / drivePulledOut + the deterministic
//     eject-latch completion); HOST canonical on conflict + connect seed.
//   DrivePayload (110) -- prop_drive.data_0 rows ({u32 eid} + the v65
//     signal_wire codec sans image, BlobChunkPayload chunks). 0x45
//     dirty-marks + a 1 Hz diff-gated baseline poll; birth authors broadcast
//     at adoption.
//   RackState (111) -- prop_driveRack 16-row storage: presser index-ops
//     peer->host HOST-TERMINAL + host-canonical full array + reflected
//     gen() re-apply; deny/refund for races.
//
// Design of record: votv-drive-chain-L5-impl-DESIGN-2026-07-18.md (7-round
// /qf). The slotted-latch of that design is SATISFIED BY the existing
// frozen/static pose gate (remote_prop.cpp "frozen/static non-attachable --
// ignored"): a slotted drive is frozen by putDriveIn on every peer, so
// straggler poses are already dropped -- no second mechanism (RULE 2).
//
// Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::drive_sync {

// Install once per session (latched; safe per net-pump tick).
void Install(coop::net::Session* session);

// Per net-pump tick: verb-name resolution, the dirty-mark barrier drain,
// the 1 Hz sweeps, pending-apply retries, the connect broadcast queue.
void Tick();

// Router entries (event_dispatch_signal.cpp).
void OnDriveSlotState(const coop::net::DriveSlotStatePayload& p, uint8_t senderSlot);
void OnDrivePayloadChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);
void OnRackStateChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST: queue the L5 connect seed (slot lines + drive payloads + rack
// canonicals) for a peer that just reached world-ready.
void QueueConnectBroadcastForSlot(int peerSlot);

// (The v118-style slot-only birth reap was audit-rejected for drives -- one
// shared class, no byte discriminator, false positives on multi-take play.
// The reap is CONTENT-correlated instead: a denied take's ghost identifies
// itself by its adoption payload's row hash; drive_sync reaps it there.)

// CLIENT (prop_drop_intent, the freshBirth drain): note that `actor` is a
// LOCALLY-AUTHORED drive birth -- its payload broadcasts at adoption (first
// eid sight). Without the note a client's first sight stays prime-only: a
// joiner's save-loaded drives materialize AFTER the connect prime and must
// NOT be re-authored (measured in the first v119 smoke: the joiner
// re-broadcast the host's own connect-seed rows + 2 unmatched-eid strays).
void NoteLocalDriveBirth(void* actor);

// Full teardown (the OnDisconnect fanout) -- clears baselines, dirty marks,
// pending applies, deny records; re-run implicitly at next session start.
void OnDisconnect();

}  // namespace coop::drive_sync

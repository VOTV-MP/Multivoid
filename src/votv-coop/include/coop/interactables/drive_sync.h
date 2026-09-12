// coop/interactables/drive_sync.h -- the drive-chain lanes: DriveSlotState, the per-slot FSM state
// lines, and DrivePayload, the prop_drive row contents. RackState lives in drive_rack_sync; this
// module keeps ALL the verb watches and forwards rack marks to it.
//
// The design's slotted latch is SATISFIED BY the existing frozen/static pose gate in
// remote_prop.cpp: a slotted drive is frozen by putDriveIn on every peer, so straggler poses are
// already dropped and a second mechanism would be redundant. Game thread throughout.

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
//
// DriveSlotState carries idempotent per-slot lines for the desk play, comp and eraser slots. Slot
// actors have no eids, so a line is keyed by role. ANY peer announces its organic transitions; a
// receiver-side overlap SELF-SIMULATES inserts and never ejects, then pre-checks and applies --
// reflected putDriveIn or drivePulledOut, plus the deterministic eject-latch completion. The HOST
// is canonical on conflict and on the connect seed.
//
// DrivePayload carries prop_drive.data_0 rows: {u32 eid} plus the signal_wire codec without the
// image, in BlobChunkPayload chunks. Verb dirty-marks and a 1 Hz diff-gated baseline poll drive it,
// and birth authors broadcast at adoption.
void OnDriveSlotState(const coop::net::DriveSlotStatePayload& p, uint8_t senderSlot);
void OnDrivePayloadChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST: queue the connect seed -- slot lines and drive payloads, with the rack canonicals riding
// drive_rack_sync's seed right after -- for a peer that just reached world-ready.
void QueueConnectBroadcastForSlot(int peerSlot);

// A slot-only birth reap does not work for drives: one shared class, no byte discriminator, and
// false positives on multi-take play. The reap is CONTENT-correlated instead -- a denied take's
// ghost identifies itself by its adoption payload's row hash, and drive_sync reaps it there.

// CLIENT (prop_drop_intent, the freshBirth drain): note that `actor` is a LOCALLY-AUTHORED drive
// birth, so its payload broadcasts at adoption, on first eid sight. Without the note a client's
// first sight stays prime-only, and a joiner's save-loaded drives -- which materialize AFTER the
// connect prime -- would be re-authored: the joiner re-broadcasts the host's own connect-seed rows
// plus any unmatched-eid strays.
void NoteLocalDriveBirth(void* actor);

// Full teardown (the OnDisconnect fanout) -- clears slot/payload baselines,
// dirty marks, pending applies, noted births; the deny/taken rings are
// drive_rack_sync's (its own OnDisconnect). Re-run implicitly at next start.
void OnDisconnect();

}  // namespace coop::drive_sync

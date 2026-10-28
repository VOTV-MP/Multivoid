// coop/interactables/drive_sync.h -- the drive chain's slot lane: DriveSlotState, the per-slot FSM state
// lines. RackState lives in drive_rack_sync and a drive's row in drive_payload_sync; this module keeps
// the slot and rack verb watches and forwards rack marks to drive_rack_sync.
//
// The design's slotted latch is SATISFIED BY the existing frozen/static pose gate in
// remote_prop_drive.cpp (ResolveAndStartDrive): a slotted drive is frozen by putDriveIn on every peer, so
// straggler poses are already dropped and a second mechanism would be redundant. Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::drive_sync {

// Install once per session (latched; safe per net-pump tick).
void Install(coop::net::Session* session);

// Per net-pump tick: verb-name resolution, the dirty-mark barrier drain,
// the 1 Hz slot sweep. An insert line for a drive not bound here waits in bind_wait.
void Tick();

// Router entries (event_dispatch_signal.cpp).
//
// DriveSlotState carries idempotent per-slot lines for the desk play, comp and eraser slots. Slot
// actors have no eids, so a line is keyed by role. ANY peer announces its organic transitions -- a
// client only in the world it announced world-ready in, since its load of the host's save is the
// host's state, and never the drive a slot holds taking a new eid; a receiver-side overlap
// SELF-SIMULATES inserts and never ejects, then pre-checks and applies -- reflected putDriveIn or
// drivePulledOut, plus the deterministic eject-latch completion. The HOST is canonical on conflict
// and on the connect seed: a client's line reaches the host alone, which relays a line it accepted
// to every other client and answers a conflict, or an eject naming another drive than its slot
// holds, to its source.
void OnDriveSlotState(const coop::net::DriveSlotStatePayload& p, uint8_t senderSlot);

// The slot lines this peer announced this session: a caller that must follow its own insert onto the wire waits for
// it to rise. Game thread.
uint64_t AnnouncedCount();

// HOST: queue the connect seed -- slot lines, with the drive rows and the rack canonicals riding
// drive_rack_sync's seed right after -- for a peer that just reached world-ready.
void QueueConnectBroadcastForSlot(int peerSlot);

// Full teardown (the OnDisconnect fanout) -- clears the slot baselines,
// dirty marks; the deny/taken rings are drive_rack_sync's
// (its own OnDisconnect). Re-run implicitly at next start.
void OnDisconnect();

}  // namespace coop::drive_sync

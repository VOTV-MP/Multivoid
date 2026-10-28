// coop/interactables/drive_rack_sync.h -- the drive-rack lane, extracted from drive_sync.cpp.
// RackState is prop_driveRack's 16-row storage: index ops go peer to host and are
// host-terminal, the host broadcasts a canonical full array and re-applies gen() by
// reflection, and a raced op is denied and refunded. The take-race axis -- the deny ring, the
// taken ring, the TTL and the consume -- lives WHOLE in this module.
// The rack's rows are this lane's alone: the save-record lane is told the class is owned elsewhere,
// so a rack born with rows reaches the clients at the host's first sight of it, a canonical to every peer.
//
// Owner-API contract, a strictly ONE-WAY dependency (drive_sync and drive_payload_sync include
// this; this includes neither):
//   - drive_sync keeps ALL the verb watches, because putDriveIn is a shared slot-and-rack
//     context; its callback forwards rack marks through MarkDirtyFromVerb().
//   - drive_payload_sync's apply of a client's row asks TryConsumeDenyReap() for the reap
//     VERDICT, while the reap ACTION -- destroy and skip-apply -- stays there.
// Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::drive_rack_sync {

// Install once per session (latched; safe per net-pump tick).
void Install(coop::net::Session* session);

// Per net-pump tick: connect-edge prime, the dirty-mark barrier drain, the
// 1 Hz sweep + assembler sweep, the 60 s stats line. A canonical for a rack not bound here waits in bind_wait.
void Tick();

// Router entry (event_dispatch_signal.cpp).
void OnRackStateChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST: queue the rack canonicals for a peer that just reached world-ready.
// Called right AFTER the drive rows' seed (subsystems source order = the shipped
// slot lines -> drive rows -> racks wire order, all on the Default lane).
void QueueConnectBroadcastForSlot(int peerSlot);

// The verb-watch forward from drive_sync's OnVerbEntry (putDriveIn rack-ctx
// + getDrive). Relaxed atomic store only -- capture-safe mid-verb.
void MarkDirtyFromVerb();

// The take-race reap VERDICT (drive_payload_sync's apply of a client's row, host side): if a
// deny record matches {senderSlot, rowHash} and is within TTL, consume it
// (clear the slot) and return true -- the caller destroys the ghost instead
// of applying. Mirrors the correlation this was extracted from, 1:1.
bool TryConsumeDenyReap(uint8_t senderSlot, uint64_t rowHash);

// Game thread, HOST, read by a drill: whether this lane has taken the rack with this eid into its view, and
// every canonical send it has attempted since the process began, a joiner's seed included (never reset; a drill
// reads a delta).
bool HostSeesRack(uint32_t rackEid);
uint64_t HostCanonicalsSent();

// Full teardown (the OnDisconnect fanout) -- baselines, shadow, dirty mark,
// deny + taken rings, assembler.
void OnDisconnect();

}  // namespace coop::drive_rack_sync

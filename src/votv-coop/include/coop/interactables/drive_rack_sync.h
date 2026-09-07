// coop/drive_rack_sync.h -- v119 L5 lane 3, extracted 2026-07-18 from
// drive_sync.cpp (rack-extraction design, 8-round /qf):
//
//   RackState (111) -- prop_driveRack 16-row storage: presser index-ops
//     peer->host HOST-TERMINAL + host-canonical full array + reflected
//     gen() re-apply; deny/refund for races. The take-race axis (deny ring
//     + taken ring + TTL + consume) lives WHOLE in this module.
//
// Owner-API contract (dependency is strictly ONE-WAY drive_sync -> here;
// this module never includes drive_sync.h):
//   - drive_sync keeps ALL 0x45 verb registration (vm_dispatch is
//     one-callback-per-verb-name and putDriveIn is shared slot/rack ctx);
//     its bracket forwards rack marks via MarkDirtyFromVerb().
//   - drive_sync's payload apply asks TryConsumeDenyReap() for the reap
//     VERDICT; the reap ACTION (destroy + skip-apply) stays payload-side.
//
// Design of record: votv-rack-extraction-DESIGN-2026-07-18.md (on top of
// votv-drive-chain-L5-impl-DESIGN-2026-07-18.md). Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::drive_rack_sync {

// Install once per session (latched; safe per net-pump tick).
void Install(coop::net::Session* session);

// Per net-pump tick: connect-edge prime, the dirty-mark barrier drain, the
// 1 Hz sweep + assembler sweep + pending-apply retries, the 60 s stats line.
void Tick();

// Router entry (event_dispatch_signal.cpp).
void OnRackStateChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST: queue the rack canonicals for a peer that just reached world-ready.
// Called right AFTER drive_sync's seed (subsystems source order = the shipped
// slot-lines -> payloads -> racks byte order on the one pinned lane).
void QueueConnectBroadcastForSlot(int peerSlot);

// The VM-bracket forward from drive_sync's OnVerbEntry (putDriveIn rack-ctx
// + getDrive). Relaxed atomic store only -- capture-safe mid-verb.
void MarkDirtyFromVerb();

// The take-race reap VERDICT (drive_sync ApplyPayloadBlob, host side): if a
// deny record matches {senderSlot, rowHash} and is within TTL, consume it
// (clear the slot) and return true -- the caller destroys the ghost instead
// of applying. Mirrors the correlation this was extracted from, 1:1.
bool TryConsumeDenyReap(uint8_t senderSlot, uint64_t rowHash);

// Full teardown (the OnDisconnect fanout) -- baselines, shadow, dirty mark,
// pending applies, deny + taken rings, assembler.
void OnDisconnect();

}  // namespace coop::drive_rack_sync

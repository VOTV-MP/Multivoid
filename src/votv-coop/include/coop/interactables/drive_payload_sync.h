// coop/interactables/drive_payload_sync.h -- a data drive's recorded row (prop_drive_C.data_0): the host authors it, a
// client's copy holds the host's rows, and every drive a client brings into the world sends its row to the host.
// Its writers are all Blueprint and each is followed by the drive's own upd() on the peer that ran it (a box or rack
// take, a cheat, the thrower, loadData on every route a stored item comes back by, the eraser, the desk). So the host
// sends a drive's row at the POST of prop_drive_C::upd when it changed and at a drive's enrolment when it is not its
// class default's; a client puts back, at the same POST, a drive whose row left the one it holds. A drive a client
// brings into the world (a fresh birth, a drop intent by any route, a held item) is noted by the prop lanes and sends
// its row to the host at its enrolment or its bind. The host takes one row per such drive, from the client that
// brought it and while its own copy is still at the class default, and answers any other client row with its own
// (MTA answers a cancelled element-data change to its source: CGame.cpp:2779-2793). A joiner's world-ready seed
// carries every row that differs from its class default.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::drive_payload_sync {

// Keeps the session; registers the upd watch once a process. Called every pump tick.
void Install(coop::net::Session* session);

// Game thread, every pump tick: settles the watch; sends the enrolments queued since; once a second drops notes
// and rows kept for drives that are gone. A row for a drive not bound here waits in bind_wait.
void Tick();

// Reliable applier (event_dispatch_signal).
void OnDrivePayloadChunk(const coop::net::BlobChunkPayload& p, uint8_t senderSlot);

// HOST: every drive row that differs from its class default's, to the joiner at `peerSlot`, its seed.
void QueueConnectBroadcastForSlot(int peerSlot);

// CLIENT (the prop lanes' expression sites): `actor` came into the world from this player, so its row goes to the host
// at its enrolment or its bind. Game thread.
void NoteOwnDrive(void* actor);

// HOST (the drop intent's spawn): `actor` is the host's copy of a drive the client at `senderSlot` brought into the
// world, so that client's row for it is the one the host takes, once, at its enrolment. Game thread.
void NoteClientBrought(void* actor, uint8_t senderSlot);

// prop_element_tracker::MarkPropElement enrolled `actor` as `eid` (any thread; handled at the next Tick).
void OnEnrolled(void* actor, uint32_t eid);

// remote_prop::RegisterPropMirror bound `actor` to `eid`, the prop of the peer at `senderSlot`. Game thread.
void OnBound(uint32_t eid, void* actor, int senderSlot);

// A peer's slot is free: its half-assembled rows and the drives it brought go with it (bind_wait drops its held rows).
void OnPeerLeft(uint8_t slot);

// The session's end: nothing held, sent or noted.
void OnDisconnect();

// [dev] CLIENT: send `actor`'s row to the host as its own, as a modified client would for a drive it did not bring:
// the drive drill's measure of the host's refusal. False when unsent. Game thread.
bool DevClaimRow(void* actor);

// What the lane did this session, for a drill.
struct Counts {
    uint64_t sent = 0;           // HOST: rows sent to all
    uint64_t applied = 0;        // CLIENT: the host's rows applied
    uint64_t accepted = 0;       // HOST: a client's row taken for a drive it brought
    uint64_t putBack = 0;        // CLIENT: drives put back to the held row
    uint64_t ownSent = 0;        // CLIENT: its own drives' rows sent to the host
    uint64_t refused = 0;        // HOST: client rows refused and answered
};
Counts LaneCounts();

}  // namespace coop::drive_payload_sync

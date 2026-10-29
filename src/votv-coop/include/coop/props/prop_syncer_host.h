// coop/props/prop_syncer_host.h -- HOST-side: a client that syncs a prop its own interaction moves.
//
// A client whose interaction moves a prop asks to be its syncer (PropSyncClaim); the host judges the claim
// and answers it (PropSyncAnswer), a grant opens a driven row that names the client as the row's author, and
// the client's PropDrivePose rows and its PropDriveEnd are admitted against that row before the host's copy
// follows them and every other client receives them as a host-authored row (prop_drive_host). MTA's
// unoccupied-vehicle syncer (reference/mtasa-blue/Server/mods/deathmatch/logic/CUnoccupiedVehicleSync.cpp),
// with the push judged on the server, and the host's own copy kept in step.
//
// A row ends these ways, all here: the client's own rest End (OnClientEnd), a hand taking the prop, a host verb on it,
// the syncer's silence, range or leaving, the prop dying, and for a thrown prop a grant timeout, a flight bound or a run
// of refused rows. The row's readers in prop_drive_host call the first of those back through EndClientRow
// (prop_drive_rows.h).
//
// HOST-ONLY, game thread.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct PropSyncClaimPayload;
struct PropDriveEndPayload;
}  // namespace coop::net

namespace coop::prop_syncer_host {

// A client's PropSyncClaim from peer slot `slot` (1..kMaxPeers-1): judged, then answered. Game thread.
void OnClaim(coop::net::Session& s, const coop::net::PropSyncClaimPayload& p, uint8_t slot);

// A thrown prop: the host elects its thrower (peer slot `slot`) the prop's syncer, MTA's server electing an unoccupied
// vehicle's syncer (CUnoccupiedVehicleSync.cpp OverrideSyncer). A driven row names the slot under a new generation
// (converting the host row an earlier write left, or opening one) and the grant goes to it unasked, echoing `throwSeq`.
// `releaseLoc` is the release's pose and `releaseMs` its arrival on this clock: the first row is measured from them.
// False, counted, when the slot syncs kMaxThrowRowsPerSlot thrown props already, is not world-ready, or the prop cannot
// open a row (the host's copy then keeps its own stream). Game thread.
bool ElectSyncer(void* actor, uint8_t slot, uint16_t throwSeq, const float releaseLoc[3], uint64_t releaseMs);

// The drop intent's election, at the spawn watcher's birth coast, where the spawned prop has its element id: when
// prop_throw holds a mark under `actor`'s key, elects the mark's thrower. True when it did (no birth coast then).
bool ElectFromMark(void* actor);

// A client's PropDriveEnd from peer slot `slot`: the rest End of the drive it syncs. Validated at the router.
// Game thread.
void OnClientEnd(coop::net::Session& s, const coop::net::PropDriveEndPayload& p, uint8_t slot);

// Per gameplay tick, before the rows' publish: drain every origin's PropDrivePose rows and admit them against
// the rows that name their sender; once a second, hand back the rows of a silent or far syncer. Game thread.
void Admit(coop::net::Session& s);

// Peer slot `slot` left: its rows end with no word to it. Game thread.
void OnSlotLeft(uint8_t slot);

// Session end: the buckets, hand-back records, counters and the cached session. Game thread.
void OnDisconnect();

namespace dev {   // read only by src/coop/dev/ (the prop syncer and swinger drills); the admitted counts are kept only while one of the two rows is on
struct Admitted {
    uint8_t  gen = 0;     // the generation of the last row admitted for the eid
    uint32_t rows = 0;    // rows admitted in that generation, kept after the row ends
};
Admitted AdmittedOf(uint32_t eid);
// Rows refused because their sender is no syncer of the prop (no row, or another slot's).
uint32_t NotSyncerCount();
// Rows admitted from a sender that is no syncer of the prop: only the drill's red arm can make this nonzero.
uint32_t NonSyncerAdmitted();
}  // namespace dev

}  // namespace coop::prop_syncer_host

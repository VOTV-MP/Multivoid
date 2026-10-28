// coop/props/prop_drive_stream.h -- the apply of a driven-prop stream: a client's, and the host's of a syncing client's.
//
// The host publishes the pose of every prop under an external drive -- a hook's constraint, a
// body still sliding after the hook let go -- on MsgType::PropDrivePose, and closes each prop's
// stream with a reliable PropDriveEnd (a client that syncs a prop sends the host its End on the same
// kind; the host's router sends that to prop_syncer_host, not here). This module parks the local
// copy on the first pose, its mesh stops simulating so physics leaves it alone, and drives it through
// the same fixed-delay snapshot interpolation the held-prop and trash-carry receivers use
// (coop/props/active_drive.h), keyed by eid. The end edge snaps the final pose, hands the prop its
// physics and velocity back, and closes the generation so a pose still in flight cannot park it
// again. The host's copy of a prop a client syncs is driven by the same code: the pose the host admitted from
// the syncer parks it (FeedAdmitted), and the End closes it (ReleaseHost).
//
// Game thread only. Not folded into remote_prop, the per-slot held-prop receiver: a
// prop here is in nobody's hand, and when a hand takes it that stream wins.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstddef>
#include <cstdint>

namespace coop::net {
class Session;
struct PropDriveEndPayload;
struct PropPoseSnapshot;
}  // namespace coop::net

namespace coop::prop_drive_stream {

// Per game tick, both roles: a client drains what arrived and parks and drives each prop; both roles
// advance every live drive (a gap freezes at the last pose), the host's being the copies FeedAdmitted
// parked. Nothing to drain on the host.
void TickApplyAndDrive(coop::net::Session& s);

// HOST, game thread: a pose of `e.elementId` the host admitted from the prop's syncing client (the key is
// the host's own): parks the host's copy on the first of its generation, a drive target after. Admission
// is prop_syncer_host's; this is the same apply the client runs on the host's batch.
void FeedAdmitted(const coop::net::PropPoseSnapshot& e);

// HOST, game thread: the end of a client-authored drive on the host's copy of `end.eid`: unparks it,
// places it at the End's pose, restores its physics from the End's flags and gives it the velocity.
// A copy a hand holds is yielded to the hand; a prop never parked takes the End as a receiver that never
// saw the generation.
void ReleaseHost(const coop::net::PropDriveEndPayload& end);

// CLIENT, game thread: the host closed a prop's stream. Validated at the router.
void OnEnd(const coop::net::PropDriveEndPayload& p);

// CLIENT, game thread: this peer ended generation `gen` of `eid` itself -- a syncing client's rest End, or the End
// it sends for a grant it no longer holds. The generation is recorded the way a hand's yield records it (closed
// and yielded), minus the hand's physics bookkeeping, so the host's echo of that End and any late pose of that
// generation are dropped here.
void CloseGen(uint32_t eid, uint8_t gen);

// True while this stream has `actor` parked: its pose is the host's stream's, so a converge that
// would move it or re-run its init() leaves it alone, as it leaves a held prop. Game thread.
bool IsParked(void* actor);

// True when a join-window correction read under drive context (gen, open) is older than what this
// copy holds of that prop's drive: a live drive row of a newer generation, or else the END pose
// ApplyEnd wrote onto the eid's CURRENTLY bound actor, of a newer generation, or of the same one
// while the correction was read with it still open. An END pose that died with its actor (a
// load-tail re-create, a purge), never applied (its hold given up) or yielded to a hand judges
// nothing: the correction then stands. Game thread.
// Divergences from MTA's CanUpdateSync (reference/mtasa-blue/Server/mods/deathmatch/logic/
// CElement.cpp:1300, its stamp compared at CUnoccupiedVehicleSync.cpp:285 of the same folder):
// 1. gen 0 is a value ("never driven"), not a wildcard: a correction read before any drive must
//    lose to a later drive END, which a wildcard would accept.
// 2. The compare is ordered (GenAfter), not equality: a NEWER stamp is accepted, since the later
//    drive messages write over it and a drop would lose the only reliable delivery of a prop at
//    rest (MTA's sync is unreliable, so a drop either way costs only a packet).
// 3. The context is two-phase (gen + open), the END edge bumping nothing: it needs no new wire byte.
// MTA's "0 means ignore" is kept for the identity re-arm (hasCtx false), never judged by this rule.
bool IsCorrectionStale(uint32_t eid, uint8_t gen, bool open);

// Session end: every driven prop gets its physics back. Game thread.
void OnDisconnect();

// The stream ends applied here lately, newest first, at most `max` of the last kRecentEnds: each eid,
// the host's final pose and the tick it landed (GetTickCount64). Cleared with the session. For the fall
// drill's verdict, which polls it at 4 Hz: an end pushed out between two polls is lost to it. Game thread.
inline constexpr size_t kRecentEnds = 8;
struct AppliedEnd {
    uint32_t eid = 0;
    ue_wrap::FVector pose{};
    uint64_t atMs = 0;
};
size_t RecentEnds(AppliedEnd* out, size_t max);

namespace dev {   // read only by src/coop/dev/ (the prop syncer drill); written only while the drill row is on
struct DriveView {
    uint8_t          gen = 0;              // the generation of the last row applied (or End applied) for the eid
    uint32_t         rows = 0;             // rows parked on or driven to in that generation, before its End
    bool             endApplied = false;   // that generation's End was applied to the copy here
    ue_wrap::FVector startLoc{};           // the copy's place at the generation's first row, before the park
    bool             startRead = false;
    ue_wrap::FVector lastRowLoc{};         // the place the last row applied in that generation named, before its End
};
// Kept after the End and cleared with the session; a default view when the eid was never driven here.
DriveView ViewOf(uint32_t eid);
// NET THREAD: a row of `eid` reached this peer's receive store, which keeps the newest per eid. Counted for the drill.
void NoteMerged(uint32_t eid);
// Rows of `eid` that reached the receive store this session: against the view's rows it tells coalescing from loss.
uint32_t MergedOf(uint32_t eid);
}  // namespace dev

}  // namespace coop::prop_drive_stream

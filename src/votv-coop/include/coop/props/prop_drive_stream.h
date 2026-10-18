// coop/props/prop_drive_stream.h -- CLIENT-side apply of the host's driven-prop stream.
//
// The host publishes the pose of every prop under an external drive -- a hook's constraint, a
// body still sliding after the hook let go -- on MsgType::PropDrivePose, and closes each prop's
// stream with a reliable PropDriveEnd. This module parks the local copy on the first pose, its mesh
// stops simulating so physics leaves it alone, and drives it through the same fixed-delay snapshot
// interpolation the held-prop and trash-carry receivers use (coop/props/active_drive.h), keyed by
// eid. The end edge snaps the final pose, hands the prop its physics and velocity back, and closes
// the generation so a pose still in flight cannot park it again.
//
// CLIENT-side, game thread only. Not folded into remote_prop, the per-slot held-prop receiver: a
// prop here is in nobody's hand, and when a hand takes it that stream wins.

#pragma once

#include "ue_wrap/core/types.h"

#include <cstddef>
#include <cstdint>

namespace coop::net {
class Session;
struct PropDriveEndPayload;
}  // namespace coop::net

namespace coop::prop_drive_stream {

// CLIENT game tick: drain what arrived, park and drive each prop, advance every live drive (a gap
// freezes at the last pose). No-op on the host and with nothing to drain.
void TickApplyAndDrive(coop::net::Session& s);

// CLIENT, game thread: the host closed a prop's stream. Validated at the router.
void OnEnd(const coop::net::PropDriveEndPayload& p);

// True while this stream has `actor` parked: its pose is the host's stream's, so a converge that
// would move it or re-run its init() leaves it alone, as it leaves a held prop. Game thread.
bool IsParked(void* actor);

// True when a join-window correction read under drive context (gen, open) is older than what this
// copy holds of that prop's drive: a live drive row of a newer generation, or else the END pose
// ApplyEnd wrote onto the eid's CURRENTLY bound actor, of a newer generation, or of the same one
// while the correction was read with it still open. An END pose that died with its actor (a
// load-tail re-create, a purge), never applied (pending expiry) or yielded to a hand judges
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

}  // namespace coop::prop_drive_stream

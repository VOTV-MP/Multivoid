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

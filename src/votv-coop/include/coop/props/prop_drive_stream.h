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

// Session end: every driven prop gets its physics back. Game thread.
void OnDisconnect();

}  // namespace coop::prop_drive_stream

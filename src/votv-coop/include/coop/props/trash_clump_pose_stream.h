// coop/props/trash_clump_pose_stream.h -- CLIENT-side apply of the trash-clump carry pose stream.
//
// A client-grabbed pile's clump is driven by the HOST (on the requester's puppet -- the puppet tick
// is dead), so the host ORIGINATES a per-eid pose batch (MsgType::TrashCarryPose) that EVERY client
// renders: the relay cannot echo a pose back to its origin, so the grabber would never see its own
// carried clump through the per-slot PropPose relay. This module drains that batch each game tick
// and drives the matching trash PROXY through the SAME fixed-delay snapshot interp the per-slot
// held-prop receiver uses (coop/props/active_drive.h), keyed by eid -- so a client-grabbed clump
// carries and throws as smoothly as the host's own, and scales to as many simultaneous client grabs
// as there are peers. The drive ends at the ToPile convert: the carry latch closes host-side, the
// host stops streaming, and ClearDriveForEid snaps the proxy to the pile.
//
// CLIENT-side and game-thread only, and deliberately NOT folded into remote_prop, the per-slot
// receiver: this is the host-authoritative per-eid stream, a distinct channel.

#pragma once

#include "coop/element/element.h"  // ElementId

namespace coop::net { class Session; }

namespace coop::trash_clump_pose_stream {

// CLIENT game tick (from subsystems::TickGameplay, after the WorldActor client tick): drain the latest
// TrashCarryPose batch, ctx-gate each entry, feed it into the per-eid ActiveDrive (BeginLerpToPose), and
// AdvanceLerp every live drive every tick (smooth follow + freeze on a stream gap). No-op on the host /
// when no batch. Game thread.
void TickApplyAndDrive(coop::net::Session& s);

// Drop the per-eid carry drive for `eid` (the carry ended: a ToPile land convert, or the proxy retired).
// Idempotent. Game thread.
void ClearDriveForEid(coop::element::ElementId eid);

// Full reset (net disconnect): clear every per-eid carry drive. Game thread.
void OnDisconnect();

}  // namespace coop::trash_clump_pose_stream

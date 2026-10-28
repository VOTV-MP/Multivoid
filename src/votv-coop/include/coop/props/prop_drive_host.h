// coop/props/prop_drive_host.h -- HOST-side: the props in motion under something other than a
// hand.
//
// The held-prop stream is sourced from the player's grab slot alone, so a prop a hook's constraint
// pulls -- or one still sliding after the hook let go -- had no channel: the host's copy moved and
// every other copy stood still. This is that channel. A verb that puts a host prop under an
// external drive CLAIMS it here (the hook lane is the first); the claim outlives the verb until
// the body rests; while claimed or coasting the prop's pose goes out on MsgType::PropDrivePose
// whenever it moved, and one reliable PropDriveEnd closes the stream with the final pose, the
// host's physics flags and the velocity. MTA's unoccupied-vehicle syncer, with the host as the
// only syncer since props are host-authoritative. A row can also name a client as its author
// (prop_syncer_host grants it); the host then publishes the client's admitted pose as it would its own.
//
// Game thread; the table is the host's, Pump runs on both roles. Verb-fed and never scanned: the set holds only
// what a caller claimed or a client was granted.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::net { class Session; }

namespace coop::prop_drive_host {

// Put `actor`, a keyed Aprop_C descendant this host owns, under the drive. Idempotent while the
// claim holds, so a feeder may call it every pass for every prop it sees tied; a re-claim of a
// coasting prop cancels its end. A prop in a hand -- this player's grab slot, a peer's held-prop
// stream, or the hotbar hand axis -- is refused silently, since the hand lane owns it, and so is a
// class the wire never expresses. `reason` names the verb for the log. Game thread.
void Claim(void* actor, const char* reason);

// How many props coast with no verb's claim. Game thread.
size_t CoastingCount();

// The drive context of `actor` (eid `eid`) for a message that reads its pose now: the claim
// generation last opened for the eid (0 if none this session) and whether that drive is open.
struct DriveContext { uint8_t gen = 0; bool open = false; };
DriveContext ContextOf(void* actor, uint32_t eid);

// A hand holds `actor`: this player's grab slot, a peer's held-prop stream, or the hotbar hand axis.
// Game thread.
bool InAHand(void* actor);

// The verb let go of `actor`: the prop coasts under the stream until it rests, then the end edge
// fires. A no-op for an unclaimed actor. Game thread.
void Release(void* actor);

// An instant push moved `actor`: stream it, coasting, until it rests. A prop a verb has claimed
// stays claimed, and one already coasting starts its rest clock again; the refusals are Claim's.
// `reason` names the push for the log. Game thread.
void Coast(void* actor, const char* reason);

// A host-side write to `actor` (its pose or its physics, from a peer's message) takes the prop from the client that
// syncs it, as a host verb does: the client's row ends, the host's copy coasts from the last admitted pose. A no-op
// for a prop with no client row, so a caller may ask before every such write. Game thread.
void OverrideClientSyncer(void* actor, const char* reason);

// A peer's world just came up: every driven prop's current pose is re-sent on the next tick, so
// the joiner parks the resting ones too, which the delta gate would otherwise never send it.
// Game thread.
void OnPeerWorldReady();

// Per gameplay tick, the driven prop's whole pump, in the one order it needs. BOTH roles first tick
// prop_syncer (a client publishes the pose of each prop it syncs and ends the ones that rested). HOST: admit the rows a
// syncing client sent (prop_syncer_host), then read every driven prop's pose (resting ones at 4 Hz) and
// publish the ones that moved, close the streams of the props that rested or that a hand took, drop the
// dead; a client row publishes the pose just admitted instead of being read. BOTH roles then run the
// apply tick (prop_drive_stream): a client drains the host's batch, the host advances the copies it fed.
// Game thread.
void Pump(coop::net::Session& s, bool isHost);

// Session end. Game thread.
void OnDisconnect();

}  // namespace coop::prop_drive_host

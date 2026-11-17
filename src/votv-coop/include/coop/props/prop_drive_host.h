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

// Whether a drive's author may end it for `actor`'s body: true when the engine reports the body asleep, false when it reads awake
// (it keeps streaming), and true when the sleep cannot be read at all, so such a body rests by the still-time alone and holds
// no row open for ever. The unreadable case is counted and said once per class: `<class> rest by still-time: the body's sleep
// could not be read (#n)`. Game thread.
bool BodyAsleepForEnd(void* actor);

// A hand holds `actor`: this player's grab slot, a peer's held-prop stream, or the hotbar hand axis.
// Game thread.
bool InAHand(void* actor);

// The verb let go of `actor`: the prop coasts under the stream until it rests, then the end edge
// fires. A no-op for an unclaimed actor. Game thread.
void Release(void* actor);

// Who asks for a coast. A Verb is a host verb's own push, throw or release: it takes a prop from the client that syncs
// it (the row becomes the host's). A BodyPush is a body that walked into the prop: it opens a row only where none stands,
// since a row already streaming the prop (a let-go's, a verb's, a client's) is its source and re-times itself by its own
// moves. A LetGo is a hand's plain release (CoastLetGo): it takes a client's row as a Verb does, and is its own cause so a
// drill can tell it from a body's push.
enum class CoastCause : uint8_t { Verb, LetGo, BodyPush };

// An instant push moved `actor`: stream it, coasting, until it rests. A prop a verb has claimed
// stays claimed, and one already coasting starts its rest clock again; the refusals are Claim's.
// `reason` names the push for the log; `cause` says whose push it is. True when this call opened a row or re-timed
// a host row; false for a refusal, for a BodyPush on a prop any row already streams, and for a row a client syncs that a
// BodyPush leaves alone. Game thread.
bool Coast(void* actor, const char* reason, CoastCause cause = CoastCause::Verb);

// A row of any author is open on `actor` right now. Game thread.
bool IsDriven(void* actor);

// The cause of the HOST-authored coast row open on element `eid` right now; false when no such row is open (none, or a
// client's). For a drill that asks who flew a prop. Game thread.
bool OpenCoastCause(uint32_t eid, CoastCause& out);

// True when prop_drive_rules::kMaxCoasting props already coast: the caller opens no further one (a prop that already has a
// row is no further one). A `birth` stops short of it by kCoastPushReserve, so a pile of births cannot take the rows a
// let-go or a body's push needs. The first three refusals and each doubling after print a counter line naming `actor`'s
// key and `reason`, with the running count. Only HOST coasts count: a client's row costs the host no pose read. Game
// thread.
bool CoastCapReached(void* actor, const char* reason, bool birth = false);

// A hand let go of `actor` and its body flies on this host (a client's release applied here, the host's own release):
// Coast, when the prop is simulating and awake, has an element id and the cap is not reached. Past the cap, or for a
// body at rest, a frozen, static or sleeping one, it does nothing and every peer's copy falls on its own. `reason`
// names the release for the log. Game thread.
void CoastLetGo(void* actor, const char* reason);

// A host-side write to `actor` (its pose or its physics, from a peer's message) takes the prop from the client that
// syncs it, as a host verb does: the client's row ends, the host's copy coasts from the last admitted pose. A no-op
// for a prop with no client row, so a caller may ask before every such write. Game thread.
void OverrideClientSyncer(void* actor, const char* reason);

// The client slot whose row stands on the prop with element id `eid`, elected or claimed: the syncer the host admits rows
// from. 0 when the prop has no row or the host authors it. Game thread.
uint8_t ClientSyncerOf(uint32_t eid);

// Is that client row an ELECTED throw (a flight the host step-tests and bounds), not a claimed one (a push or a pickup)?
// False for a prop with no client row. Game thread.
bool ClientRowElected(uint32_t eid);

// A peer's world just came up: every driven prop's current pose is re-sent on the next tick, so
// the joiner parks the resting ones too, which the delta gate would otherwise never send it.
// Game thread.
void OnPeerWorldReady();

// Per gameplay tick, the driven prop's whole pump, in the one order it needs. BOTH roles first tick
// prop_syncer (a client publishes the pose of each prop it syncs and ends the ones that rested). HOST: admit the rows a
// syncing client sent (prop_syncer_host), then read every driven prop's pose (resting ones at 4 Hz) and
// publish the ones that moved, close the streams of the props that rested or that a hand took, drop the
// dead; a client row publishes the pose just admitted instead of being read. BOTH roles then run the
// apply tick (prop_drive_stream): a client drains the host's batch, the host advances the copies it fed. Both
// roles' pump also reads the props a player's body walks into (body_push). Game thread.
void Pump(coop::net::Session& s, bool isHost);

// Session end. Game thread.
void OnDisconnect();

}  // namespace coop::prop_drive_host

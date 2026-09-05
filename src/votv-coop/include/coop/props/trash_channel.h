// coop/props/trash_channel.h -- the host-authoritative trash-entity sync-time context, the
// pile sync's identity and freshness core. A trash entity is a host-minted eid that re-skins
// in place across pile, clump and pile again (the old and new eids are the same); position is
// never identity, so a dense pile cluster can never mis-bind. This module owns the per-eid
// sync-time context: the host bumps it on every transition (grab, throw, land) and stamps it
// on every convert, carry and throw packet, and receivers drop any packet whose context is
// older than the eid's known generation, so a carry or land packet still in flight when the
// entity transitions can never be re-applied to the re-skinned entity. MTA's per-element
// sync-time context is the precedent. The clump-to-pile link is driven from the Func thunk on
// the deferred spawn, which fires for every dispatch route including the ones ProcessEvent
// cannot see: the grab direction records a clump birth certificate the held-object edge
// consumes, and the land direction converts the re-piled clump in place. Game thread only.

#pragma once

#include "coop/element/element.h"  // ElementId

#include <cstdint>

namespace coop::net { class Session; }
namespace ue_wrap { struct FVector; struct FRotator; }

namespace coop::trash_channel {

// Host: a trash entity re-skinned (a grab, pile to clump; or a land, clump to pile). E is the
// host-minted eid of the entity; kind is to-clump or to-pile; newActor is the new rendering,
// already positioned; loc and rot are its transform; chipType is the trash variant, carried
// across both edges. Bumps E's context, re-skins E onto the new actor locally and broadcasts
// the convert to all peers. Host only, driven from the clump adoption (a grab) and the re-pile
// thunk (a land). Game thread.
void OnHostConvert(coop::net::Session& s, coop::element::ElementId E, uint8_t kind, void* newActor,
                   const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot, uint8_t chipType);

// Grab adoption, the deterministic clump birth certificate. Every garbage clump is born from a
// chipPile's deferred spawn, and the source object of that dispatch is the pile, still alive at
// the POST. The Func thunk records the birth here: clump to the source pile's eid and chip
// type. It fires for every grab route, the press and the use-hold (which repeats with no new
// input dispatch), so the held edge consumes the certificate instead of guessing; a heuristic
// that bound a new clump to the open carry mis-bound a foreign clump with two clumps in
// flight, and the eid stuck held forever.

// Host, from the thunk: `clump` was just spawned by the pile owning eid E; the chip type is
// read off the pile. Entries expire after a short TTL (a grab that never reaches the hand:
// hands full, or denied). Game thread.
void NoteClumpBorn(void* clump, coop::element::ElementId E, uint8_t chipType);

// Host, at the held edge: consume the birth certificate for `clump`. True, with E and the chip
// type filled, if the thunk recorded it. Game thread.
bool TakeClumpBorn(void* clump, coop::element::ElementId* outE, uint8_t* outChipType);

// Host: bind `heldClump` onto trash entity E and open the carry through OnHostConvert (the
// context bump, the rebind, the to-clump broadcast). Used by the held edge for a certificate
// clump (a fresh grab) and for a re-grabbed existing tracked clump (a gate-aborted rest clump
// picked back up). Returns E. Game thread.
coop::element::ElementId AdoptBornClump(coop::net::Session& s, coop::element::ElementId E,
                                        void* heldClump, const ue_wrap::FVector& clumpLoc,
                                        const ue_wrap::FRotator& clumpRot, uint8_t chipType);

// The client-grab direction, the host arm of the door-style request. A client sends a
// GrabIntent (the native grab suppressed, then the request). The host validates and executes
// the real grab on the client's puppet (the grab engages on an unpossessed puppet, with no
// controller dependency), broadcasts the authoritative to-clump convert, then registers the
// puppet carry drive, since the puppet's tick does not drive its physics handle and the host
// drives the hold pose.

// Client: send a GrabIntent for the eid to the host, the client-grab request. The pile-grab
// observer's client arm calls this after suppressing the native grab; the synthetic harness
// test calls it to exercise the wire. A no-op unless `s` is a running client. Game thread.
void SendGrabIntent(coop::net::Session& s, uint32_t eid);

// Host: a client at `senderSlot` requested to grab trash entity `eid`. The gates mirror the
// door request's: the eid not already carrying, the sender not already holding any eid. On
// pass: resolve the puppet and the pile actor, run playerGrabbed with the puppet as the
// player, read grabbing_actor synchronously, open the carry and broadcast through
// OnHostConvert, record the holder, and register the per-tick hand drive. A no-op, logged as
// denied, on any gate failure or a dead puppet or pile. Host only. Game thread.
void OnGrabIntent(coop::net::Session& s, uint32_t eid, uint8_t senderSlot);

// Client: send a ThrowIntent to the host. The release mode is the use-press toggle drop, its
// direction ignored; the hard-throw mode is the native throw, with the client's camera-forward
// unit vector at the press. A no-op unless `s` is a running client. Game thread.
void SendThrowIntent(coop::net::Session& s, uint32_t eid, uint8_t mode, const ue_wrap::FVector& dir);

// Host: a client at `senderSlot` requested to throw the puppet-held trash entity `eid`. The
// gate: the sender must currently hold the eid. On pass: release the puppet's grab (clearing
// grabbing_actor and the physics handle, required so the clump's re-pile gate reads not held),
// enable physics and apply the throw velocity (the release mode: the puppet's hand motion,
// capped; the hard throw: the native camera-forward speed over the clump's mass plus the
// puppet's velocity, uncapped), and note the throw, so the host streams the flight rather than
// the hand. A no-op, logged as denied, on a gate failure or a dead puppet or clump. Host only.
// Game thread.
void OnThrowIntent(coop::net::Session& s, uint32_t eid, uint8_t mode,
                   const ue_wrap::FVector& camFwd, uint8_t senderSlot);

// The client carry state, the use-press grab-or-throw toggle. The client tracks the single
// trash eid its own puppet is carrying (a player holds one thing), so a press is a throw when
// carrying and a grab when aimed at a pile proxy. The state is driven by the convert stream the
// client already receives: the request marks a pending grab, the matching inbound to-clump
// confirms the carry, and the matching to-pile (or a throw send) clears it.

// Client: an inbound convert for `eid` was observed. Reconcile the carry state: a to-clump
// matching our pending grab confirms the carry; a to-pile for our carried eid clears it.
// Called from the convert receiver, client only; a no-op on the host. Game thread.
void NoteClientConvertObserved(uint32_t eid, bool toClump);

// Client: the trash eid the local player is carrying through its puppet, or the invalid id.
// The pile-grab observer's toggle reads it: carrying means throw, otherwise grab the aimed
// proxy. Game thread.
coop::element::ElementId ClientCarryEid();

// Host: peer `senderSlot` disconnected. Clear any hold it owns, so the eid becomes
// re-grabbable; the puppet vanishes on disconnect, so the clump is already physics-released,
// and this is the state cleanup plus a forget, so a stranded carry latch cannot keep the
// entity in limbo. Game thread.
void OnGrabHolderLeft(uint8_t senderSlot);

// Host: the puppet-held clump for E was lost without a normal land (the clump died, or the
// puppet went not-live). Clear E's client hold and carry latch, and broadcast a destroy for E,
// so every client retires the frozen proxy and clears its carry toggle; otherwise a client
// that loses a clump mid-carry is stuck in throw mode forever. The trash entity genuinely
// vanished on the host, so a destroy is the honest authoritative edge. Idempotent. Game
// thread.
void ReleaseClientHold(coop::net::Session& s, coop::element::ElementId E);

// Client: clear the local carry toggle. Called from the client destroy path when the carried
// proxy is retired (the host aborted the carry), so the next press grabs instead of throwing a
// dead eid. A no-op if not carrying that eid. Game thread.
void ClearClientCarry(uint32_t eid);

// The carry latch and the land settle, host side. A trash entity is carrying from the real
// grab (a to-clump convert while not carrying) until the real land. During the carry the
// game's stock churn (the held clump re-piles on cluster contact about once a second and the
// game auto-re-grabs it) is suppressed: the re-pile is not broadcast and the context is not
// bumped, so the client renders one clump, pose-streamed, rather than a pile stuck at the
// cluster re-skinned and teleported every cycle. The churn re-grab rebinds E onto the new
// clump, so the carry stream stays alive. The real land is a re-pile not followed by a re-grab
// within the settle window: the settle holds the to-pile broadcast that long, a re-grab
// cancels it (churn), and the timeout commits it (the one land) and closes the latch.
// Graceful either way: too short a window commits a churn re-pile early and the re-grab
// re-opens it, a brief self-correcting flicker; too long lags the land morph a few frames.
// Neither strands E. Per eid.

// Host: a garbage clump re-entered the hand during an active carry of E (the churn re-grab,
// observed at the held-object edge; not a fresh player grab, so no pending grab). Rebind E
// onto the new clump, so the carry pose stream keeps tracking it, and cancel E's pending land
// settle, since a re-grab proves the preceding re-pile was churn. No broadcast, no context
// bump. A no-op if E is not carrying. Game thread.
void OnHostRegrab(coop::element::ElementId E, void* newClump);

// Host: is E mid-carry, the latch open? The local streams gate the held-edge re-grab rebind on
// this. Game thread.
bool IsCarrying(coop::element::ElementId E);

// Host: is a land settle pending for E (a re-pile observed, not yet committed or cancelled)?
// The release edge uses it to tell a churn flicker (the held slot empties right after a
// re-pile, so a settle is pending) from a real drop or throw (an alive clump and no pending
// settle; a re-grab cancels the settle before any throw, so a real release never coincides
// with one). The release edge is suppressed only when carrying and this is true. Game thread.
bool HasPendingSettle(coop::element::ElementId E);

// Host: the single currently carried trash eid, or the invalid id; there is at most one
// carried clump at a time. The drop thunk logs it to cross-check that a real drop or throw is
// the carried clump rather than an equip drop before the flip closes the latch. Game thread.
coop::element::ElementId AnyCarryingEid();

// Host, per gameplay tick: prune expired birth certificates; count down the land settles,
// committing a settled land (the held to-pile broadcast and the latch close) when no re-grab
// arrived within the window; and guarantee carry termination, since every open carry lane
// must eventually close: a lane whose Registry actor died with no re-pile (a consumed clump)
// closes and broadcasts a destroy after a short grace, and a lane whose clump lies at rest
// un-held (the native re-pile gate aborted because the thrower's hand was busy at the land)
// closes silently, the clump staying world-tracked and re-grabbable, the single-player end
// state. `localHeldActor` is the local player's currently held actor, a rest exclusion: a still
// player holding a clump must not read as at rest un-held. Game thread.
void TickCarry(coop::net::Session& s, void* localHeldActor);

// Drop E's carry latch and land settle: E's entity was destroyed or retired, so the latch can
// never close on a land. A safety against a stranded latch; idempotent. Game thread.
void ForgetEid(coop::element::ElementId E);

// The current per-eid sync-time context, for the local streams to stamp on each carry pose. 0
// means untracked or non-trash, no enforcement. Game thread.
uint8_t CtxForEid(coop::element::ElementId E);

// Receiver: a convert for E arrived with sync-time context `ctx`. True if fresh (apply, and
// adopt the host's context as the new known generation); false if stale (an out-of-order or
// duplicate convert, dropped). A context of 0 is legacy or non-trash, always fresh, nothing
// adopted. Game thread.
bool AdoptInboundConvertCtx(coop::element::ElementId E, uint8_t ctx);

// Receiver: a pose or release for E arrived with `ctx`. `requireCurrentGen` picks the gate by
// packet kind: a carry pose passes true (apply only the current generation, holding a pose
// ahead of its convert and dropping a stale one); a release passes false (apply if not stale,
// since a throw legitimately leads the last convert, not being a re-skin). A context of 0, or
// an E with no convert seen yet, means no enforcement. Game thread.
bool IsInboundStreamCtxFresh(coop::element::ElementId E, uint8_t ctx, bool requireCurrentGen);

// Drop all per-eid state at disconnect.
void OnDisconnect();

}  // namespace coop::trash_channel

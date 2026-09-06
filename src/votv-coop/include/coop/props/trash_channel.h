// coop/props/trash_channel.h -- the host-authoritative trash entity: one eid that re-skins in
// place across pile, clump and pile again, and the sync-time context that keeps a packet in
// flight from landing on the re-skinned entity. Position is never identity. Game thread only.
// See docs/piles.md for the model.

#pragma once

#include "coop/element/element.h"  // ElementId

#include <cstdint>

namespace coop::net { class Session; }
namespace ue_wrap { struct FVector; struct FRotator; }

namespace coop::trash_channel {

// Host: E re-skinned. `kind` is to-clump or to-pile, `newActor` the new rendering already
// positioned. Bumps E's context, rebinds E onto the new actor and broadcasts the convert.
void OnHostConvert(coop::net::Session& s, coop::element::ElementId E, uint8_t kind, void* newActor,
                   const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot, uint8_t chipType);

// Grab adoption. Every clump is born from a chipPile's deferred spawn, whose source object is
// the pile; the Func thunk records that link here so the held edge consumes a certificate
// instead of guessing which clump is which.

// Host, from the thunk: `clump` was spawned by the pile owning E. Entries expire on a short
// TTL, for a grab that never reaches the hand.
void NoteClumpBorn(void* clump, coop::element::ElementId E, uint8_t chipType);

// Host, at the held edge: consume `clump`'s birth certificate. True, with E and the chip type
// filled, if the thunk recorded one.
bool TakeClumpBorn(void* clump, coop::element::ElementId* outE, uint8_t* outChipType);

// Host: bind `heldClump` onto E and open the carry. Serves both a fresh certificate clump and a
// re-grabbed tracked one. Returns E.
coop::element::ElementId AdoptBornClump(coop::net::Session& s, coop::element::ElementId E,
                                        void* heldClump, const ue_wrap::FVector& clumpLoc,
                                        const ue_wrap::FRotator& clumpRot, uint8_t chipType);

// The client-grab direction: an intent the host validates and performs on the requester's
// puppet, then drives, since a puppet's own tick does not move its physics handle.

// Client: request the grab of `eid`. Called after the native grab is suppressed. A no-op unless
// `s` is a running client.
void SendGrabIntent(coop::net::Session& s, uint32_t eid);

// Host: `senderSlot` asked to grab `eid`. Gated on the eid not already carrying and the sender
// not already holding one. On pass: grab on the puppet, open the carry, record the holder and
// register the per-tick hand drive. Denied is a logged no-op.
void OnGrabIntent(coop::net::Session& s, uint32_t eid, uint8_t senderSlot);

// Client: request the throw of `eid`. The release mode is the use-press drop and ignores `dir`;
// the hard-throw mode carries the camera-forward unit vector at the press.
void SendThrowIntent(coop::net::Session& s, uint32_t eid, uint8_t mode, const ue_wrap::FVector& dir);

// Host: `senderSlot` asked to throw the `eid` it holds. Releases the puppet's grab (required, or
// the clump's re-pile gate still reads held), enables physics, applies the throw velocity for the
// mode, and notes the throw so the host streams the flight rather than the hand.
void OnThrowIntent(coop::net::Session& s, uint32_t eid, uint8_t mode,
                   const ue_wrap::FVector& camFwd, uint8_t senderSlot);

// The client carry state: the one trash eid this client's puppet holds, so a use press is a
// throw while carrying and a grab otherwise. Driven by the convert stream the client already
// receives.

// Client: reconcile the carry state against an observed convert. A to-clump matching our pending
// grab confirms the carry; a to-pile for the carried eid clears it. A no-op on the host.
void NoteClientConvertObserved(uint32_t eid, bool toClump);

// Client: the trash eid this player carries, or the invalid id. The use-press toggle reads it.
coop::element::ElementId ClientCarryEid();

// Host: `senderSlot` disconnected. Release any hold it owns so the eid is re-grabbable; its
// puppet is gone, so the clump is already physics-released.
void OnGrabHolderLeft(uint8_t senderSlot);

// Host: E's puppet-held clump was lost with no land. Clear the hold and latch and broadcast a
// destroy, so no client is stuck carrying a dead eid. The entity really did vanish here, so a
// destroy is the honest edge. Idempotent.
void ReleaseClientHold(coop::net::Session& s, coop::element::ElementId E);

// Client: clear the carry toggle when the carried proxy is retired, so the next press grabs. A
// no-op if not carrying that eid.
void ClearClientCarry(uint32_t eid);

// The carry latch and the land settle, host side, per eid: the latch spans the real grab to the
// real land, the game's churn re-piles inside it are suppressed, and a re-pile opens a settle
// window that a re-grab cancels and a timeout commits. docs/piles.md carries the model.

// Host: a churn re-grab during E's carry. Rebind E onto the new clump so the pose stream keeps
// tracking it, and cancel E's pending settle, since a re-grab proves the re-pile was churn. No
// broadcast, no context bump. A no-op if E is not carrying.
void OnHostRegrab(coop::element::ElementId E, void* newClump);

// Host: is E mid-carry? The local streams gate the held-edge rebind on it.
bool IsCarrying(coop::element::ElementId E);

// Host: is a settle pending for E -- a re-pile seen, not yet committed or cancelled? The release
// edge uses it to tell a churn flicker from a real drop or throw, and suppresses itself only
// while carrying and this is true.
bool HasPendingSettle(coop::element::ElementId E);

// Host: the one carried trash eid, or the invalid id. The drop thunk logs it to cross-check that
// a real drop is the carried clump and not an equip drop.
coop::element::ElementId AnyCarryingEid();

// Host, per gameplay tick: expire birth certificates, count down the land settles and commit a
// settled land, and terminate any carry the normal path would leave open -- a clump destroyed
// mid-carry closes with a destroy, one left lying un-held closes silently and stays
// world-tracked. `localHeldActor` excludes the local player's own held clump from that rest test.
void TickCarry(coop::net::Session& s, void* localHeldActor);

// Drop E's latch and settle: its entity is gone, so the latch can never close on a land.
// Idempotent.
void ForgetEid(coop::element::ElementId E);

// E's current sync-time context, for the local streams to stamp on each carry pose. 0 means
// untracked or non-trash: no enforcement.
uint8_t CtxForEid(coop::element::ElementId E);

// Receiver: a convert for E arrived with context `ctx`. True if fresh -- apply it and adopt
// `ctx` as the known generation; false if stale, an out-of-order or duplicate convert. A `ctx`
// of 0 is always fresh and adopts nothing.
bool AdoptInboundConvertCtx(coop::element::ElementId E, uint8_t ctx);

// Receiver: a pose or release for E arrived with `ctx`. `requireCurrentGen` picks the gate: a
// carry pose passes true and applies only on the current generation, holding a pose that leads
// its convert; a release passes false and applies unless stale, since a throw legitimately leads
// the last convert rather than re-skinning. A `ctx` of 0, or an E with no convert seen, is
// unenforced.
bool IsInboundStreamCtxFresh(coop::element::ElementId E, uint8_t ctx, bool requireCurrentGen);

// Drop all per-eid state at disconnect.
void OnDisconnect();

}  // namespace coop::trash_channel

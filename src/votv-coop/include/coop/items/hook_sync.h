// coop/items/hook_sync.h -- the grappling hook on the wire. A deployed hook is the world-side
// continuation of the `prop_hook_C` item, so it lives beside the other item lanes.
//
// THE AUTHORITY SPLIT, AND THE GAME DRAWS IT. mainPlayer_C's `activeHook` field names a hook for
// exactly as long as it belongs to the player who fired it; the game clears that field the instant
// the second end anchors, while the actor goes on standing there.
//
//   activeHook names it            -> the FIRING PEER is the syncer. Mirrors elsewhere.
//   activeHook null, actor alive   -> the HOST is the syncer. It is a save actor from then on.
//
// That is `architecture.md`'s third row word for word -- a continuously simulated element the
// interacting peer owns by assignment, whose authority returns to the host on release -- with the
// release edge handed to us by the game instead of inferred.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct HookStatePayload;
struct HookDestroyPayload;
}  // namespace coop::net

namespace coop::hook_sync {

// NEITHER HALF CAN BE THE OTHER. The reel writes through lib_C to the LOCAL player, so a host-run
// hook would reel the HOST rather than the client it belongs to; and an anchored hook must persist
// in the one save in the session, authors motion on host-owned props, and mints an item on release,
// so it cannot stay with a client.
//
// Install caches the session and registers the mirror brain-park: a PRE-interceptor on hook_C's
// ReceiveTick that cancels the Blueprint body for any actor in this lane's mirror table. The park
// has to be a cancel rather than a tick disable -- the class can ever tick and nothing on the
// Blueprint chain serializes whether it STARTS ticking, so the tick function registers inside
// FinishSpawningActor and any disable lands after it, and one frame of that body reels THE VIEWING
// PEER'S OWN PLAYER toward someone else's hook. The gate is per-ACTOR, not per-role: every peer
// holds its own real hooks and other peers' mirrors in the same class at the same time.
//
// Idempotent; retries while the class has not loaded. Net-pump ensure.
void Install(coop::net::Session* session);

// IDENTITY is `(sender slot, owner-local seq)` and it is STABLE ACROSS THE HANDOFF: the anchor does
// not rename the hook, it moves who may write it. It deliberately sits outside the element
// registry, which `coop/creatures/owner_entity_sync.h` already settled for the same reason --
// peer-owned identity is a different axis. The handoff itself is `coop/items/hook_anchor`.
//
// LATE JOIN, per phase. Owner phase: the owner's keepalive re-announce, relayed by the host, which
// is `owner_entity_sync`'s own mechanism and the reason this lane needs no host replay table for
// it. Anchored phase: the host's save, plus a replay for the window a save capture missed.
//
// The driver serves both sides. OWNER: read `activeHook` (one load), track the actor through a
// liveness-checked reference, send HookState on a phase change or a moved head, keepalive, and act
// on the two ways the field goes null -- the actor died, or it anchored. RECEIVER: prune mirrors
// whose actor died under them. Costs one pointer read while the local player has no hook.
void Tick();

// Receivers, game thread, `senderPeerSlot` keys the mirror.
void OnStateMsg(const coop::net::HookStatePayload& p, int senderPeerSlot);
void OnDestroyMsg(const coop::net::HookDestroyPayload& p, int senderPeerSlot);

// A peer left: drop its OWNER-PHASE mirrors. Anchored rows are deliberately kept -- the host owns
// those now, and the peer leaving has nothing to do with them.
void OnPeerLeftSlot(int slot);

// Session end: destroy every mirror. They are actors WE spawned and must not linger into
// single-player.
//
// WHERE THE FORCE IS. A hook's constraint exists on the host and nowhere else
// (coop/items/hook_constraint): a client's hook names what its head bit on its HookState and the
// host ties its mirror of that hook to its own copy of the actor, against the owner's puppet,
// while every constraint a client's game builds -- its own hook's, an anchored mirror's, a
// save-loaded hook's -- is broken at the seam that builds it. Every prop a hook on the host is
// tied to, whichever peer fired the hook, is then streamed by the driven-prop channel
// (hook_prop_claim.cpp). Two peers hooking one prop is two host constraints on one host body.
void OnDisconnect();

}  // namespace coop::hook_sync

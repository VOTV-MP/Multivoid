// coop/props/trash_native_hold.h -- a native trash body the host authors is held still.
//
// A trash body whose pose the host authors (a spawned mirror, a bound save-loaded native) has its tick
// and physics simulation turned off, and its root made Movable so a pose write takes. Hold is that
// recipe, and the one copy of it: the mirror spawn, the bind of a save-loaded native and the join's
// load catch all call it.

#pragma once

namespace coop::net { class Session; }

namespace coop::trash_native_hold {

// The load catch. A joiner's world is the host's save loaded by the game's own spawn loop, and a clump
// saved in flight would run its physics from its first frame and settle off the pose its save holds,
// which is the pose the host's snapshot keys it by. So a client holds the clumps the join's load
// spawns, at their saved pose, until the bind takes them (AdoptOwnNative keeps the hold) or the
// join's divergence sweep retires them.

// What a join matches by pose, and whether it needs the hold:
//   - chip piles (the capture map, the sweep's exact match): Static mobility, never simulate. Exempt.
//   - clumps (the capture map, the sweep's exact match): simulate. HELD.
//   - off-form kerfurs: bound by their portable save key, or by class within 500 cm
//     (kerfur_prop_adoption), so a settle of a few cm after the load cannot lose the match; the bind
//     follows the host's physics flags. Exempt.
//   - any other keyed prop: matched by its key; the 30 cm class-and-radius fuzzy match is a
//     fallback whose bind reconciles the physics flags (prop_wire_parity). Exempt.

// Hold a trash body still: tick off, physics simulation off, root Movable. Idempotent. The caller
// has a live trash actor. Game thread.
void Hold(void* actor);

// The inverse of Hold: tick on, physics simulation on, so the game owns the body again. The one copy of
// the release; the disconnect of a bound clump and of an unbound one both call it. Game thread.
void Release(void* actor);

// Cache the session the catch reads and, until it has taken, patch FinishSpawningActor with the load
// catch. Called every session tick from the first one (the load watchers), since the join's load
// spawns run before the world is up. Game thread.
void Install(coop::net::Session* session);

// Did the load catch hold this body? True while the same object still lives. A hold the host never bound
// is a body the host's world does not have: the join's divergence sweep asks this of an unbound native
// and destroys a held one whatever the completeness floor's class count says. Game thread.
bool IsHeld(void* actor);

// The sweep destroyed a held body: count it and say the first and every 100th. Game thread.
void NoteRetired(void* actor);

// Drop the cached session. Game thread.
void OnDisconnect();

// The session ended: every held clump still live that the bind never took gets its tick and physics back,
// and the held set clears. Called with pile_spawn_bind::OnDisconnect, before the bound ones' teardown.
// Game thread.
void ReleaseUnbound();

}  // namespace coop::trash_native_hold

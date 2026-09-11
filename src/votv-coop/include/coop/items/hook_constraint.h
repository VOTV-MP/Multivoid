// coop/items/hook_constraint.h -- where a hook's physics constraint lives: on the host, and
// nowhere else.
//
// A hook's tie is a PhysX constraint the game builds on whichever machine runs attach_a or
// makeAttachments: the thrower's for a hook in hand, every peer's for an anchored hook it loads or
// adopts. A prop is host-authoritative, so a constraint pulling a prop on a client is that client
// writing shared-world motion the host never sees, and the copies diverge. Two peers hooking one
// prop is the same defect twice over. One authority per force, the doctrine's rule, applied to
// the tie itself: a CLIENT breaks every constraint a hook builds on it, at the native seam that
// builds it, and the HOST builds a client's hook its real constraint on the mirror it holds of
// that hook. Game thread throughout, except the seam callback, which only posts.

#pragma once

#include <cstdint>

namespace coop::net {
class Session;
struct HookStatePayload;
}  // namespace coop::net

// The two halves, one per role.
//
//   CLIENT  every constraint a hook builds here is broken as it is built. No client copy of a prop
//           is ever pulled. The client's own hook keeps its pull on the client's own player, which
//           is a velocity write in the tick and not the constraint, and its head still rides what
//           it bit, because the attach is a parenting the break leaves alone.
//   HOST    a client's hook is given a real constraint. The client's HookState names what its
//           head bit, and the host runs the game's own attach_a on its mirror of that hook: head
//           on the host's copy of the prop, tail on that client's puppet. The prop is then one
//           host body under one or more host constraints -- the host's own hook, an anchored one,
//           another client's -- and the driven-prop channel streams it to everyone
//           (coop/items/hook_prop_claim reads the mirrors). The reel crosses as the cable length
//           the mirror already receives.
namespace coop::hook_constraint {

// MTA collapses sync ownership onto one player when two entities are linked
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CVehicle.cpp, GetController). Ours differs
// on purpose: two hooks on one prop are two forces, and only a machine holding both constraints
// can simulate them, so the prop's syncer stays the host and the LINKS move to it instead.
//
// Cache the session and patch the seam, once per process: a native Func patch has no unpatch, so
// the callback reads the session's role per call and is inert in single player. Idempotent;
// retried from Tick until the engine class resolves. Game thread.
void Install(coop::net::Session* session);

// Drain the breaks the seam queued: classify each (a hook of this lane's classes, its own tie and
// not the flight tether), break it, and retry the ones the class table could not judge yet for a
// bounded window. A no-op on the host, whose seam queues nothing. Game thread, from the lane's
// tick.
void Tick();

// What became of a bite the host tried to reproduce.
enum class BiteResult {
    Tied,     // the mirror now has the game's constraint against what the owner's hook bit
    Retry,    // the actor or the owner's puppet is not here yet; ask again on the next state
    Refused,  // malformed, unresolvable by name, or attach_a rejected it: never ask again
};

// HOST: give `mirror`, the host's mirror of slot `ownerSlot`'s hook, the tie its owner's hook has
// -- the game's own attach_a, head on the host's copy of the bitten actor at the head position the
// owner's hook stores, tail on the owner's puppet. The bitten actor is resolved by element id,
// then by save key through the tracked-prop index, then through the game's own key map, so a
// keyed thing that is not a prop -- the ATV -- ties too. An out-of-reach bite is logged and
// applied: bounds are client-scoped and cost trust, never display. attach_a's own reject set
// (a character, a hook, a child actor, an actor that ignores hooks) destroys the mirror as it
// would the real hook; the caller reads the mirror's liveness after a Refused. Game thread.
BiteResult BiteMirror(void* mirror, uint8_t ownerSlot, const coop::net::HookStatePayload& p);

// Session end: drop the queue and the session pointer; the seam stays patched and inert.
void OnDisconnect();

}  // namespace coop::hook_constraint

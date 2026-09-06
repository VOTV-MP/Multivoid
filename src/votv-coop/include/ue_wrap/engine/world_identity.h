// ue_wrap/engine/world_identity.h -- which UWorld is current, and which UWorld a given
// UObject belongs to. After a world transition the dying world's actors are not kill-flagged
// until the eventual GC purge, so every liveness-based predicate keeps answering alive for
// them: a solo quit-to-menu left the dead pawn resolvable for the better part of a minute, and
// a widget scan fed that dead world's controller into every live widget, thousands of
// absorbed access violations per second. World gates must key on world identity or
// travel-start signals, never on per-object liveness; this module is that key. Everything
// here is a comparison token: the returned pointers are identities, not objects, and are
// never dereferenced, since a dead world's address is exactly the value this module exists
// to recognise. The engine wrapper's world context cannot serve: it resolves to the immortal
// GameInstance by design and is constant across every travel. A departed world can also stay
// in the object array indefinitely: after a coop session one was left kill-flagged with a
// null level and never purged, since our own rooted proxy mirrors anchored it through their
// Outer chain (their un-root ran only for a live actor, and at a world teardown every mirror
// reports not alive); a pin is now owned by a GcPin and released from its destructor.

#pragma once

#include <cstdint>

namespace ue_wrap::world_identity {

// The UWorld the game is running right now, as an opaque identity, or null if it cannot be
// determined (boot, mid-travel, or an unresolved reflection lookup after a recook; see
// Degraded). Resolved from the immortal chain GameInstance, first local player, player
// controller, outer chain, the level's owning world. Memoised on the game thread at a fixed
// cadence and published to an atomic, so calls from any other thread are a single relaxed
// load and never touch engine memory.
void* CurrentWorld();

// The UWorld `obj` belongs to, or null when `obj` is not world-scoped (a class, a UFunction, a
// class-default object, a cooked asset, the GameInstance); those legitimately outlive worlds,
// and a null answer means no world term, not a failed lookup. Walks the Outer chain (bounded)
// for a level and reads its owning world; a world met directly in the chain answers for
// itself. `obj` must be slot-validated by the caller in the same game-thread task, since this
// dereferences it.
void* WorldOf(void* obj);

// Which of the game's worlds is current, the question every world gate actually asks,
// answered by the one reader a dying world cannot hold alive. Computed inside the refresh, at
// the instant the pointer comes out of the engine's own field, because the identity above
// must never be dereferenced by a consumer. Unknown is a third value on purpose and never
// means left: CurrentWorld is legitimately null across a travel (about a second for the boot
// travel and the quit-to-menu), and a gate that read null as no-longer-in-gameplay would act
// on a peer in the middle of a level load. Every consumer decides what Unknown means for it:
// a gate that starts something wants a positive Gameplay, a gate that ends something wants a
// positive Other, and neither may fire on Unknown.
enum class WorldKind : uint8_t {
    Unknown = 0,  // not determinable this instant: boot, mid-travel, or Degraded()
    Gameplay,     // the untitled_1 map -- every mode; the SAVE selects story vs sandbox
    Other,        // a world that is not the gameplay map: menu, preLoad, a tutorial map
};
WorldKind CurrentWorldKind();

// Bumped every time CurrentWorld is observed to change: a cheap staleness token for callers
// that would rather compare one integer than two pointers. Starts at 1, so a zero-initialised
// stamp is always stale.
uint32_t Generation();

// True when the reflection lookups this module needs could not be resolved (a recook renamed
// the owning-world, player-controller or local-players property, or either of the two classes
// the chain needs). In that state CurrentWorld returns null, and every world term that guards
// a cached object read must fail open: degrading to the liveness-only behaviour is a
// performance defect, while failing closed would make every cached actor read as dead and
// take the whole mod down. The exception: a gate that decides whether to act on the world,
// rather than whether a pointer is still valid, fails closed here on purpose, because Unknown
// is not a licence to act; the registry reaper neither reaps nor flees on Unknown, accepts
// that a genuine recook stops its safety net, and says so in a one-shot alarm keyed on a
// sustained Unknown rather than on this flag.
bool Degraded();

// The one-shot error log and the degraded latch, called internally; exposed so the boot health
// check can report the same fact in the same place every other version-surface break is
// reported.
void LogResolutionStateOnce();

// [dev] VOTVCOOP_WORLD_ID_PROBE=1: a 1 Hz comparison dump of the candidate current-world
// readers (this module's chain, the lookup by class, the live-world census) alongside the
// world of whatever the caller believes is the local pawn; the instrument that measures
// whether the chain actually moves at a solo quit-to-menu. The pawn is passed in rather than
// resolved here, since this module is engine substrate and must not reach up into the player
// registry. Pass null with no candidate. Game thread only; a no-op when unset.
void TickProbe(void* localPawnForCompare);

}  // namespace ue_wrap::world_identity

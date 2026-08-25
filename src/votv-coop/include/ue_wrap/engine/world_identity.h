// ue_wrap/engine/world_identity.h -- WHICH UWorld is current, and which UWorld
// does a given UObject belong to.
//
// WHY THIS EXISTS (measured 2026-08-23, the Linux 9-fps triage):
// `[[lesson-dying-world-actors-not-killflagged-at-menu]]` -- after a world
// transition the DYING world's actors are not kill-flagged until the eventual GC
// purge, so every liveness-based predicate in the tree keeps answering "alive" for
// them. Measured window: a SOLO quit-to-menu left the dead pawn slot-live and
// resolvable for 44+ seconds, and `players::Registry::Local()` served it the whole
// time. `input_owner`'s 1 Hz scan then fed that dead world's PlayerController into
// `HasUserFocusedDescendants` on every live UserWidget -- ~2,508 absorbed access
// violations per second, 78% of a 12.35 MB log, the game thread consumed through
// the entire join flow.
//
// The lesson's own conclusion, verbatim: "World gates must key on WORLD IDENTITY /
// travel-start signals, never per-object liveness." This module is that key.
//
// EVERYTHING HERE IS A COMPARISON TOKEN. The returned pointers are identities, not
// objects: NEVER dereference one. A dead world's address is exactly the value this
// module exists to recognise, and dereferencing it is the fault we are preventing.
//
// Do NOT reuse `engine.cpp`'s `g_worldContext` for this -- it resolves to the
// immortal GameInstance by design (the 2026-05-30 bug2 fix) and is therefore
// constant across every travel, i.e. structurally incapable of answering this
// question. That mistake is recorded in the lesson above.

#pragma once

#include <cstdint>

namespace ue_wrap::world_identity {

// The UWorld the game is running RIGHT NOW, as an opaque identity, or nullptr if it
// cannot be determined (boot, mid-travel, or an unresolved reflection lookup after a
// game recook -- see `Degraded()`).
//
// Resolved from the immortal chain GameInstance -> LocalPlayers[0] ->
// PlayerController -> (outer chain) -> ULevel::OwningWorld. Memoised on the game
// thread at a fixed cadence and published to an atomic, so calls from any other
// thread are a single relaxed load and never touch engine memory.
void* CurrentWorld();

// The UWorld `obj` belongs to, or nullptr when `obj` is NOT world-scoped -- a
// UClass, a UFunction, a class-default object, a cooked asset, the GameInstance.
// Those legitimately outlive worlds, and a null answer means "this object has no
// world term", not "lookup failed".
//
// Walks `obj`'s Outer chain (bounded) for a ULevel and reads its OwningWorld; a
// UWorld encountered directly in the chain answers for itself. `obj` must be
// slot-validated by the caller in the SAME game-thread task (this dereferences it).
void* WorldOf(void* obj);

// WHICH of VOTV's worlds is current -- the question every world GATE in the tree
// actually asks, answered by the one reader that a dying world cannot hold alive.
//
// It is computed INSIDE the refresh, at the instant the pointer comes out of the
// engine's own field, because the identity above must never be dereferenced by a
// consumer. A caller that names the world itself is reading a pointer it was told
// not to read.
//
// `Unknown` is a THIRD value on purpose and never means "left": CurrentWorld() is
// legitimately null across a travel (measured 2026-08-25: ~1 s for both the boot
// travel and the quit-to-menu), and a gate that read null as "no longer in the
// gameplay world" would act on a peer in the middle of a level load -- principle 8.
// Every consumer must decide explicitly what Unknown means FOR IT: a gate that
// STARTS something wants positive `Gameplay`, a gate that ENDS something wants
// positive `Other`, and neither may fire on Unknown.
enum class WorldKind : uint8_t {
    Unknown = 0,  // not determinable this instant: boot, mid-travel, or Degraded()
    Gameplay,     // the untitled_1 map -- every mode; the SAVE selects story vs sandbox
    Other,        // a world that is not the gameplay map: menu, preLoad, a tutorial map
};
WorldKind CurrentWorldKind();

// Bumped every time CurrentWorld() is observed to CHANGE. A cheap staleness token
// for callers that would rather compare one integer than two pointers. Starts at 1
// so a zero-initialised stamp is always stale.
uint32_t Generation();

// True when the reflection lookups this module needs could not be resolved (a game
// recook renamed ULevel::OwningWorld / UPlayer::PlayerController /
// UGameInstance::LocalPlayers). In that state CurrentWorld() returns nullptr and
// every world term in the tree MUST fail OPEN -- degrading to the pre-2026-08-23
// liveness-only behaviour is a performance defect; failing closed would make every
// cached actor read as dead and take the whole mod down.
bool Degraded();

// One-shot ERROR log + the degraded latch, called internally. Exposed so the boot
// HealthCheck can report the same fact in the same place every other version-surface
// break is reported.
void LogResolutionStateOnce();

// [dev] VOTVCOOP_WORLD_ID_PROBE=1 -- 1 Hz comparison dump of the candidate current-
// world readers (this module's chain vs FindObjectByClass(World) vs the live-World
// census) alongside the world of whatever the caller believes is the local pawn.
// The instrument that measures whether the chain above actually MOVES at a solo
// quit-to-menu -- the one premise the consuming design rests on.
//
// `localPawnForCompare` is passed IN rather than resolved here: this module is
// engine substrate and must not reach up into `coop::players` (principle 7). Pass
// nullptr if the caller has no candidate. Game thread only; a no-op when unset.
void TickProbe(void* localPawnForCompare);

}  // namespace ue_wrap::world_identity

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
//
// A DEFECT LIVED IN THIS TERRITORY AND IS NOW FIXED (2026-09-01) -- the shape is worth
// knowing, because the NEXT one will look the same. After a COOP session the departed
// gameplay world was left kill-flagged with its `PersistentLevel` already NULL and NEVER
// purged -- 75 s of dwell and a forced `CollectGarbage` both left it -- so the next
// `open untitled_1` in the same process adopted that husk and
// `UGameInstance::CreateGameModeForURL` dereferenced a null `AWorldSettings` at `+0x268`.
//
// THE HYPOTHESIS THIS HEADER USED TO CARRY WAS WRONG, and that is the part to remember: it
// said the husk was stuck in a deferred `FinishDestroy` gated on the render `FScene`. `[V]`
// the world's flags were `PendingKill` and NOTHING else -- no `Unreachable`, no
// `RF_BeginDestroyed` -- so `BeginDestroy` had never run and GC was finding it REACHABLE
// every pass. 871 of our own trash-pile proxy mirrors were still GC-rooted and anchored it
// through their Outer chain, because their un-root was written inside `if (liveActor)` and
// at a world teardown every mirror reports not-alive. Note what that means for THIS module:
// `Alive()` returning false is exactly when a cleanup is most needed and least likely to
// run. A pin is now owned by an `ue_wrap::GcPin` and released from its destructor.
// Full RE (section 9):
// `research/findings/join-identity/votv-rejoin-loadmap-null-worldsettings-RE-2026-08-31.md`.
// Repro, still green as a regression gate: `python tools/mp.py reloadchurn --rejoin`.
//
// The practical consequence for THIS module's callers: a world this module has moved off
// may still be in the object array indefinitely, so `FindObjectByClass(WorldClass)` can
// keep returning it for as long as the process lives -- which is the header's existing
// rule, now with a measured worst case rather than "44 seconds".

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

// ---- THE UNSCOPED-RESOLVE CENSUS (2026-09-01) -----------------------------------------
//
// `reflection::FindObjectByClass` skips ONLY nulls and the CDO -- no liveness test, no world
// filter -- and returns the FIRST match in GUObjectArray index order. Combined with the fact
// this header exists for (a dying world's actors are not kill-flagged until the GC purge,
// measured 44+ s), that means after a menu -> game cycle the OLD instance sits at a LOWER
// index and WINS. Every caller resolving a WORLD-SCOPED actor this way can silently address
// the world it just left.
//
// `[V]` This is not theoretical: on 2026-09-01 it produced a 1284-byte "host world". A joiner
// arriving the instant a re-hosted lobby appeared was handed a save serialized from a gamemode
// whose world no longer existed -- a structurally COMPLETE GVAS file describing nothing -- and
// kept its own world instead (two ATVs, every door disagreeing). Fixed at that one site
// (`ue_wrap/engine/save_capture.cpp`) by comparing `WorldOf(candidate)` against
// `CurrentWorld()`; a null stamp is NOT a rejection (see the rule above).
//
// `[V]` CENSUS, same day, 136 `FindObjectByClass` call sites outside reflection.cpp, by the
// class they resolve. WORLD-SCOPED and therefore exposed (~52):
//     30  mainGamemode  (21 `GamemodeClass` + 8 `mainGamemode_C` + 1 profile alias)
//      8  MainPlayerClass
//      6  DaynightCycleClass   <- the weather lane; see the fog-in-the-menu bug, still OPEN
//      3  SuperFogClass · 3 UiMenuClass · 2 kBlackScreenClass
// NOT exposed: `GameInstanceClass` (13 -- immortal by design, outlives every world),
// `WorldClass` (14 -- the world itself; use `CurrentWorld()`, which this header already says
// is the reader to prefer), and asset/CDO lookups such as `saveSlot_C`.
//
// ONLY ONE of those ~52 is scoped today. The rest are UNAUDITED -- this census is a triage
// list, not a claim that they are broken: a site reached only while a single world is live is
// fine. The discriminating question per site is "can this run across a world change?", and the
// cheapest answer for anything that can is the same two-line comparison save_capture now uses.

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
// UGameInstance::LocalPlayers, or either of the two CLASSES the chain needs). In that
// state CurrentWorld() returns nullptr and every world term that guards a CACHED
// OBJECT READ must fail OPEN -- degrading to the pre-2026-08-23 liveness-only
// behaviour is a performance defect; failing closed would make every cached actor
// read as dead and take the whole mod down.
//
// THE EXCEPTION, written down because the next consumer will read the rule above and
// follow it: a gate that decides WHETHER TO ACT ON THE WORLD -- rather than whether a
// pointer is still valid -- fails CLOSED here on purpose, because Unknown is not a
// licence to act. `registry_reaper` is the shipped instance: it neither reaps nor flees
// on Unknown, accepts that a genuine recook therefore stops its safety net, and says so
// in a one-shot alarm keyed on a sustained Unknown rather than on this flag.
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

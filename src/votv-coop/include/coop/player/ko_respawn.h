// coop/player/ko_respawn.h -- KO RESPAWN: a lethal hit knocks the local player
// out instead of running VOTV's stock permadeath (death -> 10 s -> the main
// menu, with no way back into the session).
//
// [death] section config: ko_respawn (on/off), ko_ragdoll_seconds,
// ko_invulnerable_seconds, ko_spawn_at_start.
//
// ---------------------------------------------------------------------------
// THE MECHANISM, AND WHY THE FIRST ONE WAS WRONG (measured 2026-08-31 from
// mainPlayer.uasset bytecode -- do not re-derive this, and do not "improve" it
// back into a conversion).
//
// VOTV's death is a one-way LATENT chain, not a flag:
//
//   Add Player Damage --health<=0--> kill() --> ragdollMode(true,false,true)
//        --> fallen(true) --> uber @37412:  dead := true
//                                           RetriggerableDelay(5 s) --> @4353
//            @4353: blackScreen widget      RetriggerableDelay(5 s) --> @4277
//            @4277: lib_C::loadLevel('menu')
//
// Four facts follow, and together they say the death cannot be UNDONE:
//   1. `ragdollMode` NEVER writes `dead`. The only write in the whole class is
//      uber @37412, and `dead := false` EXISTS NOWHERE -- the game has no
//      revive. The v1 header claimed "the same function that set dead clears
//      it"; that was never true.
//   2. `ragdollMode(true, passOut, false)` on an already-ragdolled player (which
//      the death has just made it) is a NO-OP: `NotEqual(ragdoll, isRagdoll)`
//      is false, so it jumps to @196, reads `IFNOT(death)`, and returns.
//   3. If it were NOT a no-op it would be WORSE: `fallen(false)` reaches uber
//      @39685, which re-reads `dead` and, finding it set, jumps back into the
//      death chain at @37478 -- the "conversion" re-arms the very thing it is
//      trying to cancel.
//   4. Neither latent delay re-reads `dead`, so even a perfect flag-clear still
//      lands in the main menu 10 s later.
//
// And the v1 PRIMARY layer could not fire either: all seven `Add Player Damage`
// call sites in the ubergraph -- and `kill()`, and `ragdollMode()` -- are
// `EX_LocalVirtualFunction`, i.e. INVISIBLE to a ProcessEvent detour per
// `docs/COOP_DISPATCH_VISIBILITY.md`. An interceptor there sees only calls WE
// dispatch, never the game's own damage.
//
// So the only correct architecture is PREVENTION: the death must never be
// authored. Its single choke point is `ragdollMode`, whose FIRST instruction is
// `IFNOT(canRagdoll) POP` -- the game's own unconditional gate, and one this
// class never writes (zero write sites in its bytecode; it is a defaults-driven
// flag). Holding it shut makes every death path in the game, seen or unseen,
// terminate before it can set `dead`.
//
// THE LANE, then, is:
//   * hold `coop::ragdoll_gate` shut for the whole session (Holder::KoRespawn);
//   * poll `saveSlot.health` -- with the gate shut, reaching 0 is HARMLESS, so
//     a poll is sufficient and no interception is needed;
//   * author the KO ourselves: borrow the gate for one
//     `ragdollMode(true, passOut=true, death=false)` on a player whose `dead` is
//     provably false, so uber @39685 takes the LIVING branch;
//   * after ko_ragdoll_seconds, `forceWakeup()`, restore vitals, teleport to the
//     КПП start point (ko_spawn_at_start), and arm a damage-immune window.
//
// DECLARED BEHAVIOUR COST (RULE 1: stated, not hidden). While the feature is on,
// the local player cannot ragdoll AT ALL from the game's own causes -- the
// manual ragdoll key, a fall knockdown, an exhaustion faint. `canRagdoll` is a
// single boolean with no "only for death" bit, and the calls that would pass
// `death=true` are invisible to us, so the coarse gate is the only enforcement
// reachable without the `EX_Local*` interception substrate
// (`docs/COOP_VM_DISPATCH_PLAN.md`). With that substrate we would instead
// rewrite `death=true -> false` inside `ragdollMode` and keep every native
// ragdoll; that is the fidelity upgrade, and it is filed, not built.
//
// FAIL-SAFE, NOT FAIL-QUIET: if `dead` ever becomes true anyway, the gate did
// not hold and this lane has no way back. `HandleLocalDeath` then returns false
// so the legacy permadeath flee runs -- being kicked to the menu is bad, but
// pretending to have saved the player while the game travels underneath us is
// worse.

#pragma once

namespace coop::net { class Session; }

namespace coop::ko_respawn {

// True when the [death].ko_respawn flag is on. Game thread.
bool Enabled();

// True while a KO is in progress (the player is down, respawn pending).
// Game thread. Used by net_pump to skip the permadeath flee on re-entry.
bool Active();

// The net_pump death-policy FAIL-SAFE: `mainPlayer` was detected with dead=true.
// Under this design that is unreachable, so it means the ragdoll gate never took
// (offsets unresolved, a pawn we never saw). Logs loudly and returns FALSE so
// the caller runs the legacy permadeath flee -- there is nothing to convert, and
// claiming otherwise would strand the player in a world the game is tearing
// down. Returns false when the feature is off, too. Game thread.
bool HandleLocalDeath(coop::net::Session& session, void* mainPlayer);

// Cache the session + read the config (idempotent; called every pump tick).
void Install(coop::net::Session* session);

// Game-thread per-tick drive: hold the gate, poll for the KO trigger, run the
// respawn timer. Called from subsystems::TickGameplay.
void Tick();

// Session-lifecycle resets (re-arm the timers, drop the gate + cached pointer).
void OnSessionStart();
void OnDisconnect();

}  // namespace coop::ko_respawn

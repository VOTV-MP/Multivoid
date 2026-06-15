// coop/wisp_attack_sync.h -- Killer Wisp coop, HOST side (Inc2, parts A + E).
//
// See research/findings/votv-killerwisp-coop-design-2026-06-13.md. The host's killerwisp_C
// always physically grabs + kills the LOCAL host (its BP verbs hit getMainPlayer(), never
// its acquired `Target`). This module makes the wisp lethal to the player it ACTUALLY
// targeted (incl. a client) and mirrors the tear:
//
//   * Per net-pump tick (HOST only): walk the host's tracked Npc Elements, find killerwisps,
//     read their attack FSM (ue_wrap::wisp::ReadState). Classify `Target` via the players
//     Registry (IsPuppet == a client puppet -> route; IsLocal == the host's own player ->
//     do nothing, the host dies for real).
//   * For a client victim, NEUTRALIZE the host's false-grab on the GRAB rising edge:
//       (1) an always-on AddPlayerDamage PRE-cancel zeroes the wisp's per-limb damage to the
//           HOST while ANY wisp grabs/tryGrabs a client (beats the grab->d1 race), and
//       (2) wisp::releasePlayer() Montage_Stops the fatality chain so the DROP-notify
//           getMainPlayer().ragdollMode(...,death=true) -- the actual host kill -- never
//           fires (a WIDE window: DROP is the last of grab->d1..d4->drop).
//   * RELAY (once per grab): WispGrab -> the victim slot (it ragdoll-dies for real after a
//     fixed delay) + WispTear -> all (the tear mirror), and run the tear locally on the
//     host's own wisp. Then DESTROY the host's wisp after the tear delay to break the
//     re-grab loop (the spawner makes more later).
//
// DEFERRED follow-on (no stubs): C4 host-health snapshot/restore is implemented here as
// belt-and-suspenders, but the gibs/blood + the victim socket-attach hold live in
// wisp_tear_mirror's follow-on. Inc3 (event-driven killerWisps diff + spawner suppression)
// is separate.

#pragma once

namespace coop::net { class Session; }

namespace coop::wisp_attack_sync {

// Cache the session + install the AddPlayerDamage PRE-cancel interceptor (idempotent; the
// interceptor resolves once mainPlayer_C is loaded). Call every net-pump tick like the
// other Install()s.
void Install(coop::net::Session* session);

// HOST per-tick detect + neutralize + relay (see header). Cheap no-op off the host / when
// no wisp is grabbing a client. Walks the tracked Npc set (small), NOT GUObjectArray. Game
// thread.
void Tick();

// Clear per-session state (handled-wisp edges, pending destroys, the damage-cancel latch).
void OnDisconnect();

}  // namespace coop::wisp_attack_sync

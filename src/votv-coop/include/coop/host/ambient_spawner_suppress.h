#pragma once

// coop/ambient_spawner_suppress.h -- client-side ambient flora/forage
// RNG-spawner suppression (Fork C, 2026-06-10).
//
// Result-level RNG mirroring doctrine: peers cannot share an RNG stream
// (global engine rand, divergent call counts, no asset edits), so the
// HOST's spawn RESULTS stream over the prop pipeline (Init POST broadcast /
// connect snapshot) and a CONNECTED CLIENT must not mint its own divergent
// ambient props between adoption brackets. PRE-cancel interceptors on:
//   mushroomMaster_C::spawn        (looping timer -> spawns spawner children)
//   mushroomSpawner_C::spawn       (looping timer -> spawns prop_food_C caps)
//   pineconeSpawner_C::ReceiveTick (random-interval tick -> sticks/pinecones/crystals)
// Deliberately NOT suppressed: garbagePileSpawner_C (one-shot at level load,
// seeded, fires pre-connect -- the connect sweep's domain) and
// undergroundGarbageSpawner_C (its dirthole_item_C output is outside the
// snapshot universe; suppressing it deletes the client's per-peer loot
// mounds with no host replacement -- dirtholes are per-peer LOCAL by
// doctrine).
//
// MTA precedent: CMultiplayerSA.cpp:828 DISABLE CPopulation__AddToPopulation
// (+ trains/planes/car generators :1086-:1101). MTA disables unconditionally
// (its binary never runs SP); we runtime-gate on running()+role==Client
// because this DLL also serves solo play (principle 6).
//
// Per-tick ensure, idempotent; registration is process-lifetime, the gate
// lives in the callback. No OnDisconnect hook (the live gate needs no edge).

namespace coop::net {
class Session;
}  // namespace coop::net

namespace coop::ambient_spawner_suppress {

// Install the 3 PRE-cancel interceptors (lazy class resolve, ~1 Hz retry
// until all 3 register, then latched). Stores `session` for the per-dispatch
// gate. Game thread only (called from net_pump's InstallObservers ensure).
void Install(coop::net::Session* session);

}  // namespace coop::ambient_spawner_suppress

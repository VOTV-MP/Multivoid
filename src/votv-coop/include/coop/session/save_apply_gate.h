// coop/session/save_apply_gate.h -- "has the game applied the save to MY world?" latch.
//
// ONE concept: the local pawn's post-load PLACEMENT edge. A gameplay world reports "up"
// (possessed local pawn resolves) SECONDS before the game's own load chain finishes
// applying the registered save: gm mainGamemode_C::loadObjects ("Loading main level
// data") runs at the load tail, and ONLY there the player is teleported to the save's
// playerTransform (bytecode: loadObjects -> transformToPlayer(saveSlot.playerTransform),
// skipped for an exactly-zero transform = a fresh world keeps its map spawn -- which is
// also a final placement). Anything that PUBLISHES the local pawn transform before that
// edge publishes the map's parked pre-placement position.
//
// Born 2026-07-02 (join-jump take-5 verdict): take-4 gated the pose stream on the
// ClientWorldReady predicate (world up + prop-registry coherence) -- and the registry is
// coherent off the level-DEFAULT props well before loadObjects runs, so a joining client
// still streamed the parked spot (-37695,69978) for ~4 s (host log 19:10:55-58: the
// puppet spawned, snapped to the parked spot, froze, then snapped to the real spawn).
// World coherence is NOT pawn coherence; this latch is the missing pawn half.
//
// Mechanism: a Func-table POST patch on mainGamemode_C::loadObjects (a BP-internal EX_
// call -- INVISIBLE to ProcessEvent, docs/COOP_DISPATCH_VISIBILITY.md; the Func patch
// fires on every dispatch path) stamps {gm instance, local pawn}. The stamp is valid
// while the gm instance is LIVE (IsLiveByIndex -- GC-robust) AND the current local pawn
// IS the stamped pawn: a world change spawns a NEW pawn + a NEW gm, so the gate closes
// itself until the new world's loadObjects fires. No explicit reset edges to miss.
//
// Pre-session worlds: when the hook first installs and a local pawn is ALREADY live
// (browser-flow host / in-world joiner), that world's loadObjects pre-dates our hook by
// definition -- stamp immediately (the pawn doubles as the liveness anchor). A joiner
// that then save-transfers into a fresh world gets a new pawn -> the stale stamp
// mismatches -> gated until the new world's loadObjects, exactly as intended.
//
// Game-thread only (the pump tick installs/reads; the Func patch fires in BP dispatch).

#pragma once

namespace coop::save_apply_gate {

// Resolve mainGamemode_C::loadObjects and install the POST latch. Idempotent + cheap
// once latched (one bool); before the gameplay GM class loads it retries next call.
// Call once per pump tick from the UNGATED section (must be armed before the world's
// loadObjects fires -- the class loads with the map, ticks run at 60 Hz through the
// load, so the hook lands frames before BeginPlay reaches the load chain).
void EnsureInstalled();

// True iff the game finished applying the save to the world `localPawn` lives in
// (loadObjects POST seen for this pawn, or the world pre-dated the hook). False for
// null. The pose stream must not publish `localPawn`'s transform while this is false.
bool IsSaveAppliedFor(void* localPawn);

}  // namespace coop::save_apply_gate

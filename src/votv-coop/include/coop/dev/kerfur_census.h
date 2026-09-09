#pragma once
// kerfur_census -- a DIAGNOSTIC census of every live kerfur form (NPC active + prop off)
// on THIS peer, to pin the forward off->active dup root. It runs on the HOST too: a
// client-only census with a hardcoded "Host=6" literal made the 5-vs-6 unmeasurable,
// because the host's real count was never read.
//
// Two modes:
//  - DEFAULT (flag off): CLIENT-only, one-shot at load-tail quiescence (the original
//    forward-dup diagnostic -- captures the final reconciled client state once).
//  - [dev] kerfur_census=1: PERIODIC on BOTH host and client (~every 10s), so a hands-on
//    can read the LATEST census on each peer and diff the real counts/forms/eids after the
//    world settles (the 5-vs-6 measurement). RULE-2-exempts-probes: gated, read-only.
//
// DIAGNOSTIC ONLY -- it reads + logs, it never mutates state or destroys an actor. Game thread.
namespace coop::kerfur_census {

// Call every tick from BOTH the client npc-mirror tick AND the host pose-stream tick. Internally
// role-gated: default = client one-shot at quiescence; kerfur_census=1 = periodic on both peers.
// No-op before quiescence (default mode) / between periodic emits. Idempotent.
void Tick();

// Re-arm the one-shot for a fresh connect / world swap (client world-ready + session-end resets).
void Reset();

}  // namespace coop::kerfur_census

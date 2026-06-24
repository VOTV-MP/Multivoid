#pragma once
// kerfur_census -- a CLIENT-side, one-shot DIAGNOSTIC census of every live kerfur form
// at load-tail quiescence. Created 2026-06-24 to pin the forward off->active dup root
// (doc kerfur/07): the dup's stale half is a "silent" un-reconciled local actor -- a
// save-loaded kerfur form that was never bound (no host mirror) NOR swept, so it never
// appears in any bind/spawn/sweep log line. The only way to see it is to enumerate the
// GUObjectArray directly and print every kerfur (NPC + prop) with its bound status.
//
// DIAGNOSTIC ONLY -- it reads + logs, it never mutates state or destroys an actor.
// Remove once the forward-dup root is settled (mirror-identity vs retire-authority).
namespace coop::kerfur_census {

// Call every client tick (after the divergence + ghost sweeps). Fires the census ONCE,
// the first tick after HasLoadTailQuiesced() -- so it captures the FINAL reconciled
// state. No-op on the host and before quiescence. Idempotent after it fires.
void TickOnceAtQuiescence();

// Re-arm for a fresh connect / world swap (call from the client world-ready + session-end
// resets, alongside the sibling ghost-sweep latch) so a reconnect re-censuses.
void Reset();

}  // namespace coop::kerfur_census

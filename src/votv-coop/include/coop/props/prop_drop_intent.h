// coop/props/prop_drop_intent.h -- the CLIENT-place -> HOST-authoritative keyed-prop DROP INTENT lane.
//
// ONE concept: a CLIENT placing a keyed world prop it had PICKED UP (hold-R place). Prop sync is
// host-authoritative: a client's own fresh Aprop_C spawn is skipped at prop_lifecycle:210 (client
// keyed spawns are host-auth), so a client-placed rock is INVISIBLE to the host (F2). The proper fix
// (NOT a seam-catch crutch -- see the /qf-15 convergence in
// research/findings/votv-keyed-prop-grabdrop-intent-lane-DESIGN-2026-07-09.md) is the pattern
// chipPiles already use: the CLIENT sends an INTENT, the HOST is the sole authority.
//
// THE FLOW (Increment 1 = the DROP half; the grab half already works -- the client's hold-R pickup
// DESTROY crosses the bidirectional destroy-seam and the host destroys its copy):
//   1. PICKUP: the client's hold-R pickup destroys the world rock -> DestroySeamBody broadcasts
//      DESTROY(key) (host destroys ITS copy) and calls NoteClientKeyedDestroy(key) -> the key is
//      PARKED. The park is the SAFETY INVARIANT: it means "the host has no copy of this key now",
//      so a later re-spawn of it authors exactly ONE host prop -> no host dup.
//   2. PLACE: the client's hold-R place (simulateDrop) spawns a FRESH Aprop_C; our FinishSpawn
//      post-hook enqueues it; one tick later (Key restored by loadData) Tick() reads the Key and, iff
//      it is in the park set, sends PropDropIntent{className,key,propName,transform,scale,physFlags}
//      to the host and unparks it.
//   3. HOST: OnPropDropIntent spawns the authoritative Aprop by Key at the transform (NOT
//      echo-suppressed) -> the host's own FinishSpawn watcher (host_spawn_watcher) expresses it and
//      broadcasts PropSpawn to ALL peers. The placing client adopts its own untracked local rock by
//      Key (ResolveLiveActorByKey scan-fallback) -> no dup.
//
// WHY the +1-tick send is load-bearing: the place ALSO destroys the in-hand display husk. If that
// husk-destroy crosses as DESTROY(key), the reliable channel delivers it BEFORE the +1-tick
// PropDropIntent -> the host processes it against a rock it no longer has (no-op) THEN spawns from
// the intent -> the spawn survives. (v2 authored the spawn same-tick and the husk-destroy killed it.)
//
// Client-side state (park set + pending) is game-thread-only. The host handler is game-thread-only.
// [[feedback-folder-per-domain-concept-rule]] [[lesson-client-keyed-prop-move-two-wire-halves]]
// [[lesson-reuse-proven-author-not-raw-reimpl]] [[feedback-map-all-wire-events-before-fixing-missing-sync]]

#pragma once

#include <string>

#include "coop/net/protocol.h"

namespace coop::net { class Session; }

namespace coop::prop_drop_intent {

// Install the CLIENT FinishSpawn post-hook (chains after host_spawn_watcher's on the same
// FinishSpawningActor UFunction). Idempotent + retry-throttled while the UFunction is unresolved.
// Game thread. Safe to call every subsystem-install tick.
void Install(coop::net::Session* session);

// Per-net-pump-tick drain (CLIENT): for each pending place spawn whose Key is now restored AND parked,
// author a PropDropIntent to the host and unpark. Game thread. No-op on the host / empty pending.
void Tick(coop::net::Session* session);

// Called from prop_lifecycle::DestroySeamBody right AFTER a CLIENT broadcasts a keyed-prop DESTROY:
// park the key so a later same-key place authors a host-authoritative drop intent (and only then --
// the host has already destroyed its copy => no dup). Game thread. Bounded FIFO set.
void NoteClientKeyedDestroy(const std::wstring& key);

// HOST handler for a received PropDropIntent: spawn the authoritative Aprop_C by Key at the transform
// (the host's FinishSpawn watcher broadcasts it). Dup-guarded (skips if the host already has the Key
// live). Game thread.
void OnPropDropIntent(coop::net::Session& session, const coop::net::PropDropIntentPayload& p,
                      uint8_t senderSlot);

// Session teardown -- clear the park set + pending. Game thread.
void Reset();

}  // namespace coop::prop_drop_intent

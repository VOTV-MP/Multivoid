// coop/host_spawn_watcher.h -- M2: host-authoritative mirroring of ambient
// spawner outputs (the pinecone scare + sibling forage), 2026-06-11.
// Roadmap: research/findings/votv-events-triggers-catalog-2026-06-11.md (M2).
//
// THE SEAM. VOTV's ambient spawners (pineconeSpawner_C etc.) materialize their
// props via UGameplayStatics::BeginDeferredActorSpawnFromClass. The spawned
// actor's own Init() is dispatched BP-internally (EX_LocalVirtualFunction ->
// ProcessInternal), so it BYPASSES our ProcessEvent detour -- empirically
// confirmed (the 2026-06-11 pinecone_probe force-spawn: zero Init-POST fired,
// which REFUTED the earlier "already syncs via Init-POST" RE). But the
// BeginDeferred CALL is a cross-object GameplayStatics UFunction dispatched
// THROUGH ProcessEvent -- OBSERVABLE. So we register our OWN POST observer on
// it (the weather_lightning / npc_sync multi-observer precedent: each
// (UFunction, cb) pair gets its own observer-table slot, all fire) and mirror
// the spawn host->client.
//
// WHY A SEPARATE MODULE (not folded into npc_sync): npc_sync owns the SAME
// BeginDeferred seam for the NPC route (allowlist -> EntitySpawn). This module
// is the PROP route (ambient transient props -> PropSpawn-by-eid, the
// trash-clump precedent). They COEXIST as two independent POST observers on the
// one shared UFunction -- zero coupling, zero edit to the shipped NPC path. (The
// events-catalog's eventual "unified HostSpawnWatcher" would merge the routing;
// the two-observer split is the lowest-risk delivery now.)
//
// HOST-ONLY broadcaster. The client never runs these spawners (its copies are
// cancelled by coop/ambient_spawner_suppress), so it never locally spawns a
// divergent pinecone; it receives the host's via PropSpawn ->
// remote_prop_spawn::OnSpawn (UNCHANGED). The mirror spawns SIMULATING and
// drops under its OWN physics: local fall physics is intentionally NOT synced
// (user 2026-06-11: "local physics of falling objects doesn't need syncing").
//
// LIFECYCLE: PropSpawn on detection; a per-tick death-watch (IsLiveByIndex --
// the trash-clump precedent) broadcasts PropDestroy when the prop's
// SetLifeSpan(600) expires or it is consumed. Principle 7: coop/ network layer;
// all engine access via ue_wrap.

#pragma once

namespace coop::net { class Session; }

namespace coop::host_spawn_watcher {

// Cache the session pointer (the boot-lifetime Session -- session_holder pattern).
void SetSession(coop::net::Session* session);

// Idempotent: resolve the GameplayStatics spawn UFunctions + offsets + the
// ambient-prop class set, then arm TWO seams:
//   - BeginDeferredActorSpawnFromClass POST observer -> the KEYLESS ambient set
//     (pinecone/stick/crystal): broadcast keyless PropSpawn + death-watch
//     (spawner dispatch is PE-visible -- proven; the transform must be read off
//     the PARAMS, which only the PE observer provides).
//   - FinishSpawningActor UFunction::Func patch (v106, 2026-07-07) -> the KEYED
//     spawn seam for EVERY dispatch route, incl. the EX_CallMath spawns a PE
//     observer can never see (propInventory::takeObj's R-drop/quick-slot place --
//     log-proven 2026-07-07: those waited the 20s safety census). The callback
//     only ENQUEUES; DrainPendingSpawns() adopts ~1 tick later, once the whole
//     BP call (loadData key restore) has completed. Replaces the old PE
//     FinishSpawningActor observer (RULE 2: the Func route is a strict superset).
// Non-fatal per seam. Called every net-pump tick until resolved. Caches `session`.
void Install(coop::net::Session* session);

// Drain the FinishSpawningActor pending queue: express (prop_lifecycle::
// ExpressSpawnedProp) every finished keyed Aprop_C spawn that is live, still
// untracked, and NOT the local hotbar hand actor (the hand-edge in
// coop/player/hand_item owns that actor's eventual world release). Entries
// retry a bounded number of ticks (key may mint late), then drop to the
// periodic safety census. Host-side express; game thread (net-pump tick).
void DrainPendingSpawns(coop::net::Session* session);

// Per-tick death-watch: any mirrored ambient prop whose actor the engine has
// destroyed (SetLifeSpan expiry / consumption -- the spawner-spawned actor has
// no observable K2_DestroyActor) broadcasts PropDestroy(eid) so the client
// drops its mirror. Host-only; cheap (IsLiveByIndex over a small bounded set).
// Game thread (net-pump tick).
void TickWatchedProps(coop::net::Session* session);

// Clear per-session state (the death-watch list + session pointer). The POST
// observer stays registered (it self-gates on connected() + role==Host).
// Net disconnect.
void OnDisconnect();

}  // namespace coop::host_spawn_watcher

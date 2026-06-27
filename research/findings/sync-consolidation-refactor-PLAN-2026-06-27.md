# Sync-consolidation refactor — PLAN (on review, 2026-06-27)

STATUS: **DESIGN / ON REVIEW — do NOT start rewriting until the user reviews this plan.**
Author: Claude. Supersedes nothing yet; this is the structure design the user asked for
([[project-sync-module-refactor-2026-06-27]]). Grounded in a 4-agent structural map of the current
sync layer + the MTA:SA client architecture (`reference/mtasa-blue/`, per RULE 2026-05-28).

---

## 0. Why now, why structural (the pivot, user-decided 2026-06-27)

D1 (steady-state grab of a save-bound pile leaves a ghost dup) and D2 (client kerfur toggle flashes
in-air then disappears) are **not two bugs** — they are **one mirror-identity-race class** surfacing in
two places **because the sync logic is physically smeared with no single owner of identity and no single
reconcile point**. Proof from the maps:

- **Identity (eid<->actor) is bound in FIVE files**: `remote_prop.cpp:744` (`RegisterPropMirror`, the
  nominal "single" entry), `trash_channel.cpp:53` (`RebindE` wrapper + `g_heldBy`), `trash_collect_sync.cpp:472`
  (self-seed `MarkPropElement`), `save_identity_bind.cpp:130` (bound-native bind), `prop_element_tracker.cpp:450`
  (`RebindLocalElementActor` + the `g_actorToPropElementId` forward map + the `g_boundMirrorNatives` mark).
  There is no one owner; "is this eid bound to a live save-native" is answered by a SET
  (`g_boundMirrorNatives`) that one code path sets and another path's timing can leave stale.
- **Every identity-mutating reconcile converges on ONE join-window one-shot** — `RunDivergenceSweep_`
  (`remote_prop_spawn.cpp:1304-1311`): twin-retire (`SweepReconcileSaveTimeTwins`), variant-1
  (`BindUnboundReCreatesByPosition`), b3 (`ApplyPendingPosCorrections`), census. It fires **once per join**
  at quiescence (`TickClientReconcile`, `:1473`) and never again. The only identity reconcile with a real
  post-quiescence path is b3 (`event_dispatch_entity.cpp:409`, gated on `HasLoadTailQuiesced`). So a save
  native that the engine churns — or a save pile a peer grabs — **after** the window has **no handler**.
  That is D1, verbatim: at 15:01:49 the grab-convert hit `morphBoundNative=false` (the bound-mirror mark
  the steady re-seed cycle had let go stale) → fresh proxy + orphaned native, and the `ArmPendingSaveTimeTwin`
  it armed (`remote_prop.cpp:997`) had **no consumer** because the one-shot sweep already fired at 14:59:24.
- **D2 is the predict-vs-relay split**: the trash path RELAYS (client sends `GrabIntent`, host authors —
  `trash_channel.cpp:310`), but `kerfur_convert.cpp` PREDICTS (the client converts locally, parks a ghost
  `:516`, and destroys it after a 4 s timeout `:545` when the host's authoritative version arrives under a
  different eid). Two authority models for two entity kinds in the same module = the flash.

**A structural cause needs a structural fix.** Patching D1 and D2 in place leaves the next instance (D3)
to surface in a third place for the same reason. Consolidate first; then each class is fixed in ONE place.

---

## 1. The target shape (from MTA, the 15-year-proven structure)

MTA's client sync is: **one identity owner** + **typed managers that are views, not owners** + **one
packet dispatch** + **one generic entity-add with collision-reconcile baked in** + **one deferred-destroy
funnel** + **a staleness-gated apply** under **host-granted relay authority** (never optimistic predict).
We adopt that shape, skipping the parts RULE 3/1 exclude (Lua, anti-cheat, asset/ASE/CEGUI, Dimensions).

New subtree: **`src/votv-coop/{src,include}/coop/sync/`** (principle-7 gameplay/network side; engine
access stays behind `ue_wrap/`). One conceptual sync engine, physically one module, behind a clean API.

| MTA piece (evidence) | Our consolidated piece | Subsumes today |
|---|---|---|
| `CElementIDs` sole ID owner (`CElementArray.cpp:23-46`); bind in ctor/dtor/`SetID` only (`CClientEntity.cpp:40,74,259`) | **`SyncRegistry`** — the SOLE owner of eid<->actor (+ reverse + per-entry flags: kind, is-save-native, owner-slot, ctx, local-vs-host). One `Bind`, one `Unbind`, one `Resolve`. | `remote_prop` RegisterPropMirror/ResolveLiveActorByEid/ResolveMirrorEidByActor + `MirrorManager<Prop>`; `prop_element_tracker` g_actorToPropElementId / RebindLocalElementActor / g_boundMirrorNatives / key index; `save_identity_bind` bind kernel |
| Typed managers delegate `Get` to the registry (`CClientObjectManager.cpp:80`) | **per-kind appliers** (`PileApplier`, `KerfurApplier`, `PropApplier`) — iterate their kind, build/skin/retire the engine actor, defer identity to `SyncRegistry` | the kind-specific bodies scattered in remote_prop_spawn OnSpawn, trash_proxy, kerfur_* |
| `CPacketHandler::ProcessPacket` one switch (`:35`) | **`SyncRouter::OnReliable(kind, reader)`** — one dispatch; fixes the "ReliableKind wires in 3 places" hazard ([[feedback-reliablekind-router-checklist]]) | event_dispatch_entity/state fan-out + the master-router list |
| `Packet_EntityAdd` resolves ID → if occupied, retire-stale → construct-with-ID (`:2957-3022`) | **`SyncReconcile::CreateOrAdopt(eid, kind, xform, …)`** — ONE create path with collision-reconcile baked in (CLAIM/ADOPT: resolve eid → if occupied by a divergent/stale actor, retire-then-rebind), used at join AND steady | OnSpawn dedup ladder + OnConvert morph hand-off + the join-window twin/variant-1/b3 reconciles |
| `CObjectSync` ctx-gated apply (`CanUpdateSync`, `:181`) | **`SyncReconcile::ApplyUpdate(eid, xform, ctx)`** — one staleness-gated apply on every kind | trash_channel ctx gate (already exists) generalized to all kinds |
| `CElementDeleter` one deferred destroy funnel (`:22-48`) | **`SyncDestroyQueue`** — one game-thread-safe deferred retire (no destroy mid-ProcessEvent) | scattered DestroyActor / RetireProxy / UnmarkAndDestroy / DestroyLocalProp |
| START/STOP_SYNC host-granted authority (`CObjectSync.cpp:85,119`) | **`SyncAuthority`** — relay + host-granted transient syncer; client sends intent, host authors, optionally grants carry-syncer rights, revokes on release | trash grab/throw (already relay) + kerfur (to be converted from predict) |

The **reconcile engine is no longer a one-shot**. `SyncReconcile` exposes ONE reconcile entry driven by
TWO triggers: the join-window quiescence pass (as today) AND a steady-state pass (a throttled tick +
event-driven on create/convert). Twin-retire, unbound-rebind, pos-correction, kerfur-retire all live on
it and run in both regimes. **This is the single structural change that closes the D1 class.**

---

## 2. The exposed API (the clean boundary — outward simple, inward hidden)

Callers (event dispatch, net pump, the engine hooks) talk to the module through this surface ONLY; the
eid-bind / ordinal-map / divergence-valve / purge-escalation / mirror-teardown complexity is private.

```
namespace coop::sync {

// ---- lifecycle (the session/world clock) ----
void OnSessionStart(Session&);
void OnJoinerConnect(int peerSlot);          // host: begin a connect snapshot for a joiner
void OnLocalWorldReady();                     // client: world loaded, begin reconcile bracket
void OnDisconnect(int peerSlot);              // per-slot teardown
void Tick();                                  // the single steady pump (reaper, steady reconcile, drives)

// ---- inbound (one router; the ONLY packet entry) ----
void OnReliable(ReliableKind kind, Reader&, int senderSlot);   // create/convert/destroy/snappos/intent...
void OnPoseStream(const PoseSnapshot&, int senderSlot);        // unreliable pose apply (ctx-gated)

// ---- identity (the ONLY eid<->actor authority) ----
Actor*   SyncRegistry::Resolve(Eid);          // null if unbound/not-live
Eid      SyncRegistry::EidOf(Actor*);
bool     SyncRegistry::IsSaveNative(Eid);      // the D1-critical question, answered by ONE owner
// Bind/Unbind are PRIVATE to the module (only CreateOrAdopt / DestroyQueue call them)

// ---- local action seams (engine hooks call in; relay, not predict) ----
void OnLocalGrabIntent(Eid);                  // client: relay to host (no local predict-spawn)
void OnLocalThrowIntent(Eid, ThrowParams);
void OnLocalSpawnObserved(Actor*, …);         // host: a local spawn to mirror out
}
```

Names/exact shape are provisional (refined as the move lands); the POINT is the surface: **one router,
one identity owner, one create-or-adopt, one destroy queue, one steady tick** — sync is *called through*
this, not by poking 12 files' internals.

---

## 3. Where the mirror-identity-race class CONVERGES (so it is fixed once)

- **D1 (steady-state grab orphans the native)** converges at **`SyncReconcile::CreateOrAdopt`** +
  `SyncRegistry::IsSaveNative`. When the grab-convert arrives, `CreateOrAdopt(eid)` resolves the registry,
  finds the bound save-native (a registry-entry FLAG, not a stale external set, so it is never lost to a
  re-seed cycle), and retires-then-rebinds in place — the morph hand-off, now UNCONDITIONAL on timing
  because the reconcile is not a join-window one-shot. The `ArmPendingSaveTimeTwin` crutch disappears:
  there is nothing to "arm for the sweep later" because create reconciles immediately, join or steady.
- **D2 (kerfur predict flash)** converges at **`SyncAuthority`**. Kerfur toggle becomes a relay intent
  (`OnLocalGrabIntent`-shaped) → host authors the convert → broadcasts → client applies the authoritative
  one. No local prediction = no parked ghost = no 4 s rollback flash. Identical authority path as
  grab/throw — one model for all kinds (MTA's relay).
- Both reduce to the same three structural invariants the module enforces: **(i) one identity owner**,
  **(ii) reconcile runs in steady state, not only join-window**, **(iii) actions relay, not predict.**

---

## 4. Rewrite order — leaf-first, behavior preserved, MOVE-commits and FIX-commits SEPARATE

Discipline (user-set 2026-06-27): a structural **move** and a behavior **fix** are NEVER in the same
commit, so a breakage bisects to one or the other. Free to fix what I see — but as a labeled fix-commit
AFTER the relevant piece has moved. Commit prefixes: `[sync-refactor move]` vs `[sync-refactor fix]`.

**The behavior-preservation etalon (we have NO green baseline — this is how we stay safe):** the etalon
is *current behavior, D1/D2 included*. After each MOVE-commit, the SAME check must reproduce the SAME
observable (the same log-marker sequence; D1/D2 still present) = the move changed nothing. The checks:
(a) the autonomous pile harness ([[reference-pile-test-harness]]: `tools/pile-test-assert.ps1` +
`autotest_chippile.cpp` + `python tools/mp.py smoke`) — log-truth invariants PASS identically;
(b) a named-window LAN smoke + log-diff ([[feedback-autonomous-lan-named-windows]]) — the bind/convert/
sweep marker stream is byte-identical modulo addresses/timestamps. A FIX-commit then *intentionally*
flips a marker (D1 dup gone) and is verified on its own.

| # | Step (commit kind) | What | Behavior check |
|---|---|---|---|
| 0 | move | Create `coop/sync/` skeleton + facade header; no code moved yet, just the namespace + a forwarding shim so callers compile. | builds; smoke identical |
| 1 | **move** | **Extract `SyncRegistry`** — pull the eid<->actor binding (the 5 binders + MirrorManager + g_actorToPropElementId + g_boundMirrorNatives + key index) behind one owner. The 5 call-sites now call `SyncRegistry::{Bind,Unbind,Resolve}` with identical semantics. Biggest/riskiest — do it FIRST (everything rests on it). | smoke identical; same bind log sequence |
| 1b | **fix** | **D1 enabler**: make `IsSaveNative` a registry-entry flag PRESERVED across the steady re-seed cycle (the mark that went stale). Separate commit; this alone may already cut D1's orphan even before the reconcile unify. | D1 grab: native NOT orphaned (marker flips) |
| 2 | move | **Consolidate the convert pipeline** — OnConvert decision tree + OnHostConvert authoring + the re-pile thunk trigger → one `convert` applier path calling `CreateOrAdopt`/`ApplyUpdate`. Carry/hold state (today in 4 places) collapses into the registry entry + authority. | smoke identical; same convert markers |
| 3 | move | **Unify the router** — one `SyncRouter::OnReliable` switch; delete the 3-place ReliableKind wiring. | smoke identical |
| 4 | move | **Move the reconcile mechanisms onto `SyncReconcile`** — twin-retire, variant-1, b3, kerfur-retire, ordinal-bind — still gated to current (join-window) timing. Pure relocation. | smoke identical; sweep markers identical |
| 4b | **fix** | **D1 close**: add the STEADY-STATE reconcile pass (the unified entry runs at quiescence AND on a throttled steady tick + on create). Twin-retire / unbound-rebind now have a steady consumer. Delete `ArmPendingSaveTimeTwin` (subsumed by create-time reconcile). | D1 grab steady-state: no dup; regression on join-window dup intact |
| 5 | move | **Unify the destroy funnel** — one `SyncDestroyQueue` (deferred, GT-safe); route every DestroyActor/RetireProxy/UnmarkAndDestroy through it. | smoke identical |
| 6 | move | **`SyncAuthority`** scaffold — formalize the relay+granted-syncer model around the existing trash grab/throw (already relay), no behavior change. | smoke identical |
| 6b | **fix** | **D2 relay**: convert kerfur toggle from predict-then-park-ghost to relay-intent (host authors, client applies authoritative). Delete the parked-ghost reaper + the 4 s timeout. | D2: kerfur toggle, no in-air flash; single activate |
| 7 | **fix/verify** | **Variant-1 force-churn verify** — now that reconcile has a steady path, variant-1's role is well-defined; run the force-churn runbook (find the GC re-instance trigger) to verify N>0 position-bind. | variant-1 binds churned natives by save-pos |
| 8 | move | **Dedupe the residue** the maps flagged: the two world caches (`g_seedWorld` vs `g_reapWorld`), the purge-episode flag 3-way coupling, the re-seed broadcast asymmetry (Express vs retrigger) — fold into the module's lifecycle. | smoke identical |

`(a)` reaper escalation + all VERIFIED work (#1/#2/b2/b3) **migrate as-is** inside the moves (they are
correct; the refactor relocates their structure without touching behavior — they stay verified).

---

## 5. Bugs-along-the-way ledger (fix AFTER the matching move, separate commits)

| Bug / smell (evidence) | Fix locus (after which move) | Commit |
|---|---|---|
| **D1** morphBoundNative stale (`remote_prop.cpp:1061`; bound-mark lost across re-seed) | step 1b (registry flag) + step 4b (steady reconcile) | fix |
| **D2** kerfur predict-rollback flash (`kerfur_convert.cpp:516,545`) | step 6b (relay) | fix |
| ReliableKind wired in 3 places ([[feedback-reliablekind-router-checklist]]) | step 3 (one router) | move resolves it |
| Carry/hold state in 4 places (host g_carry/g_settle/g_heldBy, client toggle, g_drives, g_proxies.isClump) | step 2 (registry entry + authority) | move resolves it |
| Purge-episode flag owned across 2 files, 3-way coupling (`g_inPurgeEpisode` written only by net_pump, read by tracker+spawn) | step 8 | fix |
| Two world caches resolve UWorld separately (`g_seedWorld` vs `g_reapWorld`) | step 8 | fix |
| Re-seed broadcast asymmetry (Express=no re-bracket vs retrigger=re-bracket, selected by net_pump branch, re-seed unaware) | step 8 | fix |
| `remote_prop_spawn.cpp` 1545 LOC OVER the 1500 hard cap | dissolved by steps 1-4 (OnSpawn ladder + sweep machinery split out) | move resolves it |
| `HasLoadTailQuiesced`/`g_sweepFired` is the de-facto join clock for 8 external sites but lives private in remote_prop_spawn | step 0/4 (becomes a module lifecycle signal) | move resolves it |

---

## 6. Risks + guards

- **The registry extraction (step 1) is the highest-risk move** — it touches every binder. Mitigation:
  do it as the FIRST move with the facade forwarding shim, so semantics are provably identical (each old
  call forwards 1:1); only step 1b changes a flag's lifetime, separately.
- **No green etalon** — guarded by the move-vs-fix discipline + the per-step behavior check (§4): a move
  that changes a marker is a bug in the move, caught immediately; a fix is verified alone.
- **Game-thread safety** — UE UFunctions are GT-only; the registry's net-thread reach (the worker-thread
  Init/Destroy observers, save_identity_bind's net-thread `SetReceivedMap`) keeps the existing mutex
  discipline; `SyncDestroyQueue` enforces deferred GT destroy.
- **Scope is days, not hours** — explicitly accepted by the user (structural fix pays off vs patching the
  smeared class forever). Each step is independently committed + checked, so it can pause/resume safely.
- **Don't carry known-broken as-is if a fix is in view** — allowed (user): move minimally-working, then
  fix in the next commit. Never bundle.

---

## 7. NEXT
USER reviews this plan. On approval → execute step 0 onward, MOVE-commits and FIX-commits separate,
behavior-checked each step, push the assembled module + the D1/D2/variant-1 fixes as the verified sync
ETALON. Until approval: no rewriting. push held (HEAD `6732920f`).

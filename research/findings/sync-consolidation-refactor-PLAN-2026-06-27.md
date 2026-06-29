# Sync-consolidation refactor — PLAN + AS-BUILT LEDGER (2026-06-27)

STATUS: **SHIPPED + PUSHED to origin/main `63aa4c01` (2026-06-28).** The whole arc is on origin: foundation +
D1/D2 reconcile (VERIFIED 16:06) + the CreateOrAdopt keystone (`ecdc527c`/`6aeaf55c`, g_actorToPropElementId
retired) + convert/destroy found ALREADY-funneled (`ee19ec8c`) + SyncRouter bool-chain (`fcf5b1b1`) + residue-
fold REJECTED on inspection (perf/RULE-1) + authority model NAMED (coop/sync/sync.h) + the partial sync-tree
REORG (coop/{world,social,host,devices}) + the **09:54 re-seed-orphan ghost FIX (`ddaec1fa`), PROVEN by an
in-process self-test (`c6192d06`, VERDICT=PASS both peers)**. Autonomously verified: kerfurtoggle + joinchurn PASS.
HANDS-ON 10:15 (post-push): #1 kerfur twitch = CLEARED (gone); #2 kerfur HANG-IN-AIR on turn-off = CONFIRMED REAL,
**fix built `150da133`** (re-enable physics on adopt-by-eid -- AS-BUILT, not yet hands-on-RE-verified); moved-in-
window piles MISPLACED on client = CONFIRMED REAL, **probe added (no blind fix)** -> next hands-on pins the root.
HEAD `7e89b1d1` (2 ahead: the #2 fix + probe + runbook, NOT pushed). Deployed `E3E6BEAB` (hash-verified host+client).
Author: Claude. Grounded in a 4-agent structural map + MTA:SA (`reference/mtasa-blue/`, RULE 2026-05-28).
Topic: [[project-sync-module-refactor-2026-06-27]].

### SHIPPED THIS ARC (2026-06-27, 11 commits) -- the AS-BUILT ledger
| Plan step | Commit | State | Evidence |
|---|---|---|---|
| 1 [move] unified actor->eid reverse absorbed into element::Registry (`EidForActor`/`NoteActorRebind`) + `Element::m_saveNative` | `ce132e0d` | **AS-BUILT** (builds; 15:44 binds used it) | registry.cpp/element.cpp/element.h |
| 1b [fix] D1 enabler: `g_boundMirrorNatives` SET DELETED; `IsBoundMirrorNative` via `Element::IsSaveNative`+liveness | `066d0a49` | **AS-BUILT** (not hands-on-verified for the grab scenario) | prop_element_tracker.cpp:326, save_identity_bind.cpp |
| 4b [fix] D1 close: reconcile NON-one-shot (`coop/sync/sync_reconcile`, join + steady triggers) | `47384057` | **AS-BUILT** | sync_reconcile.{h,cpp}; remote_prop_spawn.cpp:1304,1477 |
| 4b+ [fix] reconcile runs on the valve-ABORT path; post-purge steady trigger | `432183ce`,`8b85cb2e` | **BOTH VERIFIED (16:06 log).** fix #1: valve-abort reconcile re-bound **242** GC-churned natives by save-pos (`RE-BIND by position` x242 on the abort path). fix #2: `post-purge window` steady reconcile **fired 13x** (16:06:52->56) -- the `8b85cb2e` gate-order fix opened the window. World ended CLEAN (KERFUR 6/6, 0 UNCLAIMED, 0 orphans). NOTE: fix #2's 13 fires were no-ops (fix #1's abort path had already re-bound all 242 first) -- proven-to-FIRE, its catch-the-trickle value not stressed this run. | remote_prop_spawn.cpp:1226, sync_reconcile.cpp |
| 6b [fix] D2: kerfur ghost adopted by STABLE EID not fuzzy position | `df589591` | **AS-BUILT** (not hands-on-verified; the toggle scenario untested) | kerfur_convert.cpp `TakeParkedGhostByEid` |
| 7 [probe] variant-1 force-churn dev probe + runbook + GATE diag | `425633d7`,`4d6f2afb`,`2033cdb6` | **PARTIAL** -- probe moot (real purge pre-empts the synthetic churn); 15:44 world ended CLEAN (variant-1 N=0=no-churn-needed); fix #1 confirmed | save_identity_bind.cpp `ForceSaveChurnForTest` |

### ASSEMBLY PROGRESS (2026-06-28, STAGE 3)
- **KEYSTONE COMPLETE (build GREEN):** `ecdc527c` `[move]` `coop/sync/sync.h` facade + `CreateOrAdoptPropMirror`
  (RegisterPropMirror body verbatim; forwards) -> `6aeaf55c` `[fix]` `g_actorToPropElementId` RETIRED onto the
  unified `Registry::EidForActor` (every reader; LOCALS-ONLY filter kept; idempotency safer -- blocks
  minting-over-mirror; TOCTOU double-check `EidForActor!=ourEid`; UnmarkKnownKeyedProp IsBeingDeleted guard; Reap
  gate `EidForActor==pr.id`; self-test adjusted). Prop identity = ONE owner end-to-end. Both g_propElementsById +
  g_actorToPropElementId + g_boundMirrorNatives satellites now gone.
- **OnSpawn fold = ACHIEVED at the identity layer (no code change needed):** OnSpawn (`remote_prop_spawn.cpp`) is
  the PropApplier (find-or-spawn the engine actor: key/eid dedup, fuzzy fallback, claim, grab/drive skips,
  epsilon-converge); its identity binds ALL funnel through `RegisterPropMirror` -> `CreateOrAdoptPropMirror`. The
  applier logic correctly STAYS (pulling engine-actor-spawn into the identity primitive would be wrong, RULE 7).
- **CONVERT pipeline = ALREADY CONSOLIDATED at the identity layer (verified 2026-06-28, no move needed).** Both
  morph call sites -- `remote_prop.cpp:1010` (trash convert-beat-spawn hand-off, `rebindInPlace=morphBoundNative`)
  and `remote_prop.cpp:1097` (aprop/kerfur re-skin, `rebindInPlace=true`) -- route through
  `RegisterPropMirror` -> `CreateOrAdoptPropMirror`, whose `morph` path IS the single rebind decision (SetActor for
  a MIRROR / RebindLocalElementActor for a LOCAL, on the Element's authoritative IsMirror flag). The OLD-actor
  RETIRE correctly STAYS at the call site: it carries convert-DOMAIN pre-steps (`MarkIncomingDestroy` echo-suppress,
  `ClearAnyDriveFor`, `UnmarkKnownKeyedProp`) that must NOT be smeared into the identity keystone (RULE 1 / RULE 7).
  The retire ORDER (rebind-then-destroy, so the eid never resolves to a dead actor) is already the invariant. The
  earlier plan note ("centralize the retire INTO CreateOrAdopt") was REJECTED on inspection -- it would split
  domain concerns. Convert is done.
- **SyncDestroyQueue = ALREADY THE FUNNEL (verified 2026-06-28, no move needed).** `element::ElementDeleter`
  (Enqueue any-thread -> Flush game-thread at `net_pump.cpp:346`, top of Tick) is the single deferred Element-retire
  funnel. 13 producers route through `ElementDeleter::Get().Enqueue`: prop_element_tracker (4: reap/unmark/double-
  check), npc_sync (5), npc_world_enum (1), trash_proxy (1), world_actor_sync (2), kerfur_reconcile (1). The
  remaining DIRECT actor destroys (trash_proxy un-root `:223`, OnConvert echo-suppressed `:1104`, the LOCAL native
  pile `:1013`) are bare-actor retires with site-specific pre-steps -- only their ELEMENT bookkeeping funnels here,
  which is correct. The stale "no producer routes destruction here yet" comment at net_pump was corrected this run.

- **SyncRouter = DONE (`fcf5b1b1` `[move]`, build GREEN).** HandleEntity/State/WorldEvent now return bool (true iff
  the kind is in that family); event_feed::Update drops the three parallel family case-label groups and chains them in
  its default (break on first owner, else the existing unknown-kind log). Minimal transform -- every internal break
  untouched; each switch gained `default: return false` + a trailing `return true`. The family switch is now the SINGLE
  membership declaration: a new ReliableKind wires in 2 places (enum + family switch), not 3. Net -53 LOC. The inline
  special cases (Join/Balance/Teleport/Wisp/Snapshot/handshake) are disjoint from every family and keep priority.
- **Step-8 residue = folding REJECTED on inspection (2026-06-28).** The two world-ptr caches (g_seedWorld in
  prop_element_tracker vs g_reapWorld in net_pump) are CONSCIOUSLY separate: the seeder's piggybacks the seed walk's
  existing GUObjectArray iteration (perf audit W-2 forbids a per-walk FindObjectsByClass -- the 120->60fps lesson); the
  reaper's is a throttled FindObjectByClass for its gameplay-vs-menu name check (documented net_pump.cpp:425 +
  engine.cpp:60). The purge-episode flag is already a clean single-writer pub/sub (net_pump detects -> tracker owns the
  atomic -> gate/sweep/reconcile read). Folding either into a coop::sync::Tick would regress perf or couple subsystems
  for zero functional gain = a crutch (RULE 1) + churn (RULE 2). Left as-is; the finding is recorded in coop/sync/sync.h.
- **SyncAuthority = the model is now NAMED (coop/sync/sync.h AUTHORITY CONTRACT), behavior unchanged.** There is no
  scattered authority code to gather -- host-auth convert (kerfur_convert) + client-relay grab/throw already exist
  cleanly. The ONE open behavior change is the D2 host->client corrective-pose for an adopted kerfur off-prop, which is
  the DEFERRED symptom-fix (built WITH its hands-on, not scaffolded speculatively -- RULE 2). See the 3 OPEN SYMPTOMS.

THE WHOLE REFACTOR IS BUILT to its RULE-1-correct end: the identity module (one eid<->actor owner across create / adopt
/ morph / destroy) is ASSEMBLED, the router is consolidated, and the residue/authority items were assessed -- folding
rejected as a perf-regression/churn, the one real open item (D2 corrective-pose) is the deferred symptom.

### AUTONOMOUS VERIFICATION 2026-06-28 (~20:50, user off-PC + green-lit; binary hash-verified `0E04B197` both peers)
Two real LAN scenarios PASS against the assembled module -- the two MOST-refactored paths are non-regressed:
- **`mp.py kerfurtoggle`** (convert/identity path): client toggled a kerfur off->on; BOTH adopts went BY EID
  (`adopted parked turn-off/-on ghost ... by eid -- deterministic, no fuzzy miss`), `orphan_destroy=0`, `cascade=0`,
  `out_of_range=0`, host `ElementDeleter: flushed 1 deferred element`. Keystone bound the kerfur prop AND chipPiles
  via `CreateOrAdoptPropMirror`; router dispatched both families w/ ZERO `unknown ReliableKind`; clean teardown
  (`UnregisterPropMirror eid drained`). => keystone + SyncRouter + convert + destroy funnel CLEAN on a real toggle.
  (The scenario's own verdict FALSE-FAILed on the RETIRED `Gap-I-1 FUZZY MATCH` marker -- the D2 fix adopts by-eid;
  harness assertion fixed `62780b92`.) The 970 `BeginDeferred null` = fresh-boot world-load-window spawn-nulls
  (orthogonal to the refactor; the spawn factory is upstream of the bind; world recovered + piles bound).
- **`mp.py joinchurn` PASS** (join/reconcile path, real s_1234 save-world join): `divergence-sweep=1` (fired once,
  no churn re-arm), `incremental-PropSpawn=3`, `unclaimed-destroyed=0` (NO world-wipe -- the >50% valve held),
  `no-local-match-flood=0`, `kerfur materialized=1` with `0 spurious converts`, `malformed_drops=0`, RSS stable
  ~3150MB. => join census + reconcile + pile-presence + kerfur-mirror + liveness all clean.

NOT autonomously closable (VISUAL / needs the user's eyes): #1 kerfur twitch + #2 hang-in-air-on-off -- downstream of
a clean adopt, not reproduced (0 spurious converts) but neither scenario sustained a converted kerfur long enough to
expose them. #3 pile-host-only looks CLEARED (joinchurn unclaimed-destroyed=0 = client piles not dropped) -> likely the
transitional noise predicted.

### 09:54 RE-SEED-ORPHAN GHOST -- FIXED + PROVEN AUTONOMOUSLY (2026-06-28 ~22:07)
ROOT (2-agent trace): the reaper Take()s a purged bound save-native's Prop Element but DEFERS ~Element to
ElementDeleter::Flush (worker-thread safety); between Take and Flush the registry actor->eid reverse is STALE, so a
post-purge re-seed running in that window orphans a GC-churned save-native (cursor consumed -> overflow-drop) into a
tracked-but-unbound ghost -- and variant-1's position re-bind only fired ~30s later at the late quiescence sweep.
FIX (`ddaec1fa`, net_pump purge-drain re-seed edge): (a) ElementDeleter::Flush() BEFORE ReSeedKnownKeyedProps (force
the precondition) + (b) BindUnboundReCreatesByPosition() right AFTER (re-bind churned natives by POSITION immediately
-- host-shipped save-time positions, 1cm; moved-in-window handled by b3/proxy-wins). W-2-gated to the rare post-purge
re-seed only.
PROOF (`c6192d06` reseed_orphan_selftest, deployed 4BC7B452): a host-side IN-PROCESS self-test reproduces the exact
Take->re-seed race on a real chipPile native (no save-transfer-join, no rendering -> cannot false-green like a clean
smoke). Result on BOTH peers: VERDICT=PASS -- bound=1 took=1 eidBeforeFlush=32000(stale, the race) eidAfterFlush=
kInvalid((a) Flush settles it) rebound=1 finalEid=32000((b) re-bound by position, NO orphan). The whole mechanism is
proven link-by-link, deterministically.

PUSH: the user pre-authorized "test proves the fix -> sync verified autonomously -> push the rubicon". The test proved
it -> PUSHED 2026-06-28. The two VISUAL symptoms (#1/#2 kerfur twitch/hang) remain a SEPARATE deferred SyncAuthority
track needing a human pass; they do not block this push.

NEXT: user eyeballs #1/#2 (+ a client steady-grab for D1) via `research/handson_runbook_2026-06-28_whole_module.md`,
OR green-lights the push of the autonomously-verified assembled module as the sync ETALON.

### OPEN SYMPTOMS seen 2026-06-28 (DO NOT FIX NOW -- verify-after-assembly)
Decided 2026-06-28: the hands-on D1/D2 check was PREMATURE -- the module is half-moved (reconcile/bind
partly in `coop/sync/`, authority/spawn paths still scattered), so testing the transitional state catches
under-assembled noise, not real bugs. These three symptoms are RECORDED, not fixed; re-check them AFTER
the module is fully assembled (gone = transitional noise; remain = real bugs fixed on the whole path):
1. **Kerfurs TWITCH** -- "want to face both host and client at once" = two authority sources tugging the
   actor (host pose vs a still-live local predict). The 16:24 client log shows adopt-by-eid WORKED
   (every toggle `adopted parked ... ghost as PROP/NPC mirror eid=N (by eid)`, zero fuzzy-miss, zero
   timeout-destroy) -- so this is DOWNSTREAM of adoption, not an adopt failure.
2. **All kerfurs HANG IN AIR on turn-off** -- the off-form (`prop_kerfurOmega_C`) stays at the NPC death
   pos (z~6207, standing height) instead of falling/resting. Suspicion: the adopted off-prop is the
   CLIENT's local ghost (adopted in place at the local predicted pos) bound onto the "held-pose stream"
   (`kerfur_entity[client]: ... held-pose stream can now carry it`) though nothing holds it -> no pose
   correction -> frozen where the client spawned it. Worse than the OLD flash (old = timeout-destroy =
   vanished; new = adopted-but-mispositioned = hangs). Likely the adopt left local physics/authority
   half-set -- exactly the kind of half-path the assembly (CreateOrAdopt + SyncAuthority, one owner)
   resolves.
3. **Piles present on HOST, ABSENT on CLIENT** (disappear). Not yet greps-classified (cancelled mid-grep
   when the assembly decision was made). Re-grep after assembly.
HONEST: at JOIN the 16:24 census was CLEAN (6/6 kerfurs, 0 orphans, 242 RE-BIND by position on the abort
path = fix #1 still good); the symptoms are STEADY-STATE interaction artifacts in the half-assembled state.

The plan/design below is RETAINED as the roadmap for the remaining steps; the step table in §4 is the
source of truth for what shipped.

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

### FOUNDATION DISCOVERY (2026-06-27, on reading the actual code) — the registry already exists
`SyncRegistry`'s role is **already played** by `element::Registry` (the MTA `CElementIDs` analog: the sole
eid->Element* array, host/peer range partition, `RegisterMirror`/`Get`/`FreeId`, "intentionally NO bulk
Reset" — `registry.h`) + `MirrorManager<Prop>` (the MTA per-type `CClient*Manager` analog: Install /
AllocAndInstall / Take / Snapshot / Drain — `mirror_manager.h`) + `Element` (the `CClientEntity` analog:
id/type/name/typeName/actor/internalIdx/beingDeleted/mirror/ownerSlot — `element.h`). These are good and
MTA-shaped. **So step 1 is NOT "build a new class" — it is "absorb the 3 satellite indices that leaked
OUT of the registry into `prop_element_tracker` back into the Element/Registry layer, with one bind
owner."** The 3 satellites (`prop_element_tracker.cpp`):
1. `g_actorToPropElementId` (`:119`) — actor->eid reverse, **LOCALS ONLY** (a mirror is deliberately absent).
2. `g_boundMirrorNatives` (`:129`) — the is-save-native SET (actor->internalIdx). **A separate set keyed on
   actor, set by save_identity_bind, that can read stale relative to the binding = D1's exact root.** It
   exists ONLY because `MarkPropElement`/`OnConvert` must answer "is this actor a bound save-native" holding
   only the actor, and mirrors are not in satellite #1.
3. `g_keyToActor`/`g_actorToKey` (`:176-177`) — the key->actor index (the FindByKeyString balloon fix).

**The consolidation:** maintain ONE actor->eid reverse index INSIDE the registry/manager, covering BOTH
locals AND mirrors, written atomically at every Install/AllocAndInstall/RegisterMirror and cleared at every
Take/Unregister/Free. Then make **is-save-native an `Element` field** (`m_saveNative`, set by
save_identity_bind at bind). "Is actor A a bound save-native" becomes `Resolve(EidOf(A))->IsSaveNative()` —
one path through the registry, atomic with the binding, **structurally unable to desync** (satellite #2
disappears). Satellite #3 (key index) moves onto the manager as a secondary index maintained by the same
bind owner. This is smaller, lower-risk, and more faithful to the existing MTA-shaped core than a new class.

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
| 1 | **move** | **Absorb the 3 satellites into the existing `element::Registry`/`MirrorManager`/`Element`** (NOT a new class — see Foundation Discovery). 1a: add ONE unified actor->eid reverse index on `MirrorManager<Prop>` (or Registry), written at every Install/AllocAndInstall/RegisterMirror, cleared at Take/Unregister/Free — covering BOTH locals and mirrors; repoint `g_actorToPropElementId` readers + `ResolveMirrorEidByActor` at it. 1a': move the key->actor index onto the same owner. Semantics identical (same set/clear points). Biggest/riskiest — FIRST (all rests on it). | smoke identical; SAME bind + grab/throw/#1/#2/b2/b3 marker stream |
| 1b | **fix** | **D1 enabler**: add `Element::m_saveNative` (set by `save_identity_bind` at bind); answer "is actor a bound save-native" via `Resolve(EidOf(actor))->IsSaveNative()` instead of the `g_boundMirrorNatives` set; DELETE the satellite set. The flag is now atomic with the binding -> cannot read stale relative to it (the 15:01:49 `morphBoundNative=false` cause). Separate commit; may already cut D1's orphan before the reconcile unify. | D1 grab: morphBoundNative TRUE, native NOT orphaned (marker flips) |
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

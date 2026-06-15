I have everything verified. Now I'll synthesize the architect brief.

---

# Save-Transfer Kerfur Ghost-Prop Dupe — Root-Cause Brief & Fix Plan

## 1. ROOT CAUSE

The user's bug is NOT a class exemption, NOT a key/claim mismatch, NOT a cross-pipeline collision, and NOT a host-snapshot defect. All of those were chased and **ruled out** with code+RE proof (the kerfur prop is a plain keyed `Aprop_C` in the sweep universe; the host correctly omits it; props and NPCs ride disjoint channels). The ghost survives a **client-side reconciliation TIMING gap in the prop divergence sweep**.

Ranked by certainty:

### RC-A (PRIMARY, high confidence) — the prop sweep fires ONCE inline at `SnapshotComplete` with NO load-quiescence gate; the keyless-skip then permanently exempts any prop whose `Aprop_Key` is still `None` at that instant.

- The sole client mechanism that can remove a save-loaded-but-host-converted-away prop is `coop::remote_prop_spawn::DestroyUnclaimedDivergentProps(localPlayer)` — `remote_prop_spawn.cpp:905`, invoked **inline, exactly once per bracket** from `event_feed.cpp:380` in the `SnapshotComplete` case.
- Phase-2 of that sweep (`remote_prop_spawn.cpp:986-990`) reads the prop's key and, **if `key.empty() || key == L"None"`, COUNTS it into `keylessSkippedByClass` and `continue`s** — `// keyless non-pile: NOT expressible -> NEVER swept`.
- The code self-documents this as the **"ADOPTION HOLE"** (`remote_prop_spawn.cpp:963-970`): *"'keyless' is a TRANSIENT pre-Init state, and the world load's late tail (+872 keyed actors appeared AFTER the sweep in the hands-on run) turns skipped actors into keyed, interactive, never-adjudicated ghosts ... Until the structural fix (deferred adjudication or a load-quiescence gate) lands."* The WARN at `:1010-1013` names the exposure (forensic: "162 keyless skipped, top 109 prop_C").
- VOTV's save-load (`mainGamemode::loadObjects -> loadData`) restores the prop's `Aprop_Key` (deterministic, base `Aprop_C::loadData`, `this.key=data.key` — `bp_reflection/prop.json` Exports[115]) on a **late Init pass that is not quiesced before `ClientWorldReady`/`SnapshotComplete` fire**. `LoadStorySave` returns at mainPlayer spawn (`engine.cpp:257`) while `loadObjects` materialization continues across later frames. The `ClientWorldReady` coherence gate (`net_pump.cpp:525-543`) only checks `IsRegistrySeededForCurrentWorld` = "the stamped UWorld is still live" (`prop_element_tracker.cpp:634-642`) — it does **NOT** prove `loadData` finished keying every prop.
- Net: when the one-shot sweep walks, the kerfur prop reads `Key=None`, hits the keyless-skip, survives, then its key mints moments later -> a fully-keyed, interactive ghost the one-shot sweep already drained past. Host has only the NPC; client has ghost prop + NPC.

### RC-B (PRIMARY-adjacent, medium confidence) — the sweep can run against a different world generation than the one `loadObjects` finally placed the prop in (the two-level-load fragility).

- A save-transfer join performs **TWO level loads** (menu -> story world swap; `net_pump.cpp` comment + `npc_adoption.cpp:217-225`). The bracket re-arms per `ClientWorldReady` (`subsystems.cpp:148-151`), and `IsRegistrySeededForCurrentWorld` going stale-then-live does not re-trigger a fresh bracket for a *late prop materializing in an already-coherent final world*. So a prop that lands after the final bracket's sweep is never re-adjudicated. Same class of bug as RC-A; same fix closes it.

RC-A and RC-B are two faces of one structural defect: **the prop sweep has no equivalent of the NPC sweep's adoption-convergence gate.** Compare `npc_adoption::Tick` (`npc_adoption.cpp:203-208`): the NPC ghost sweep fires only when `g_snapshotDelivered && g_pending.empty() && !g_ghostSwept` — i.e. **after every pending adoption has converged** (= the world has quiesced for NPCs). The prop sweep has nothing equivalent — it sweeps inline the instant `SnapshotComplete` arrives. This asymmetry is the bug.

### Definitively excluded (do NOT "fix" these — they are correct)
- Class exemption — FALSE. `prop_kerfurOmega_C : Aprop_C` passes `IsClassKeyedInteractable` (`prop.cpp:139-141`), is NOT `IsPerPlayerPropClass` (only inventory container, `prop_lifecycle.cpp:674`) nor `IsWireSuppressedPropClass` (only mushroom, `:657`).
- Host expressing/claiming it — FALSE. Host drained the Element on convert (`kerfur_convert.cpp:209` -> `SyncDestroyedTrackedProp` -> `PropMirrors().Take(eid)`); `IsLiveByIndex` filter (`prop_snapshot.cpp:140`) rejects the dead actor anyway. No PropSpawn -> no `RecordClaim` -> genuinely unclaimed.
- Cross-pipeline (NPC `EntitySpawn` claiming the prop) — FALSE. `RecordClaim` exists only on prop paths; `npc_adoption`/`npc_mirror` never call it.
- NPC ghost sweep covering it — FALSE by construction. `DestroyUntrackedClientNpcs` gates on `IsAllowlistedClass` (`npc_mirror.cpp:515`); `Aprop_C` is disjoint from the `ACharacter` NPC allowlist.

---

## 2. THE FIX (RULE 1, host-authoritative, MTA element-reconcile shape)

**Reject the crutches first:**
- ❌ Host broadcasting an explicit "converted-away key destroy" at the connect edge — this is a per-event replay journal (RULE 1 crutch: it patches the symptom for kerfur only, leaves every other late-tail prop ghost — the documented `prop_C` wall-panel dupe — alive, and duplicates the absence-sweep machinery the architecture deliberately chose). The findings confirm the host's silence is **correct-by-design**; the snapshot is purely additive/presence-based and that is the right shape.
- ❌ A kerfur-prop special case / cross-form (prop<->NPC) reconcile keyed on identity — narrow, doesn't generalize, and the prop is identity-clean so there is nothing to special-case.
- ❌ Just adding `prop_kerfurOmega_C` to some allowlist — it's already in-universe; the class gate already dooms it.

**The deepest non-crutch fix = close the documented hole the sweep itself names: give the prop divergence sweep a load-quiescence gate + deferred re-adjudication, exactly mirroring the NPC ghost sweep's convergence gate.** This fixes the kerfur ghost AND every late-tail `prop_C` straggler ghost in one structural change.

### Mechanism — "deferred adjudication" (the option the in-code comment at `remote_prop_spawn.cpp:968` explicitly calls for)

Instead of one inline sweep at `SnapshotComplete`, the sweep becomes a **deferred, re-armable, quiescence-gated pass driven from a tick** — the same shape `npc_adoption::Tick` already uses:

1. At `SnapshotComplete`, **arm** the sweep (set a `g_sweepArmed` latch + record the bracket's claim set as frozen) instead of running it inline. Keep `g_claimedActors` alive (do NOT clear it inline).
2. A **per-tick driver** (`remote_prop_spawn::TickClientReconcile`, called from the existing net-pump client tick alongside `npc_adoption::Tick`) runs the sweep only once the world has **quiesced**: a short stability window where no *new keyed in-universe props have appeared since the last scan* (the late-tail has drained). This is the prop analogue of `g_pending.empty()`. Concretely: each tick, count live in-universe keyed actors; when the count is **stable across N consecutive scans** (e.g. ~3 scans / ~600ms) the load tail has settled -> run the one real sweep, then disarm.
3. The sweep body is unchanged EXCEPT the keyless-skip is no longer load-bearing: by quiescence time every save prop has its `Aprop_Key` restored, so the kerfur ghost reads its deterministic key, lands in `doomed`, and is destroyed. (Keep the keyless-skip as a safety net for genuinely-keyless transients, but it is now only hit by true never-keyed actors, not late-tail stragglers.)
4. Re-arm on each `ClientWorldReady`/`SnapshotBegin` (the two-level-load case, RC-B): a fresh bracket resets the latch so the FINAL world gets a quiesced sweep. This is the `OnClientWorldReady` reset that `npc_adoption.cpp:217-228` already does for NPCs.

**This is the precedent already shipped in this very codebase** (the v75 NPC fix) — applying its proven shape to the prop channel. It is RULE-1 root-cause (closes the named hole at its source), host-authoritative (client reconciles its set to the host's expressed set, additively), and generalizes to all props.

### MTA precedent (RULE 2026-05-28)
- The client-reconciles-its-entity-set-to-the-server's-on-join shape is MTA's join sync: `reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CPlayerJoinCompletePacket.cpp` (server tells the joiner the authoritative set) + `CClientGame::DoPulses` (`CClientGame.cpp:1126`) drives reconciliation **per-pulse**, not in one inline burst at the join packet — i.e. MTA already adjudicates entity divergence on a *tick loop after the world is ready*, never inline at the join message. Our inline `SnapshotComplete` sweep diverges from that; moving to a tick-driven, quiescence-gated sweep restores the MTA shape. `CClientEntityRefManager` (`reference/mtasa-blue/Client/.../CClientEntityRefManager.cpp`) is the element-lifetime reconcile precedent for "destroy locals the server set no longer contains." Cite both in the design comment.

### Exact functions to change
- `remote_prop_spawn.cpp` — `DestroyUnclaimedDivergentProps` split into `ArmDivergenceSweep()` (latch + freeze claim set, called at SnapshotComplete) and `TickClientReconcile()` (quiescence-gated run). Keep the existing walk body.
- `event_feed.cpp:380` — call `ArmDivergenceSweep()` instead of running the sweep inline.
- `event_feed.cpp:357` (`SnapshotBegin`) and the client world-ready reset — re-arm/reset the latch (mirror `npc_adoption::OnClientWorldReady`).
- The net-pump client tick — add `remote_prop_spawn::TickClientReconcile()` next to `npc_adoption::Tick()`.

---

## 3. FILE-BY-FILE PLAN

**Protocol: NO wire change.** Current head is mid-bump to **v77 (ATVs)** — the fix is client-internal reconciliation logic only; it adds no payload, no ReliableKind, no field. **Do NOT bump the version** for this; ride on v77. (This is a strong signal the fix is correct-shaped: a host-authoritative absence-reconcile needs no new wire message — the host's silence is already the signal.)

| File | Function / signature | Change |
|---|---|---|
| `include/coop/remote_prop_spawn.h` | `void DestroyUnclaimedDivergentProps(void*)` decl | Replace with `void ArmDivergenceSweep();` + `void TickClientReconcile();` + `void OnClientWorldReadyResetSweep();`. Keep `BeginClaimTracking/RecordClaim/ResetClaimTracking`. |
| `src/coop/remote_prop_spawn.cpp` (1068 LOC — **over 800 soft cap; FLAG**) | `DestroyUnclaimedDivergentProps` (905-1056) | Rename body to `static void RunDivergenceSweep_(void* localPlayer)` (the existing Phase 1/2/3 walk, unchanged). Add `ArmDivergenceSweep()` (latch `g_sweepArmed=true`, stash `localPlayer`, snapshot a stability baseline). Add `TickClientReconcile()`: while armed, count live in-universe keyed actors each tick; when stable across `kQuiesceScans` consecutive scans (≈3, ~200ms apart, reuse a `std::chrono` throttle like `npc_adoption`'s `kPollIntervalMs`) OR a hard `kSweepDeadlineMs` (~8000ms, parity with `npc_adoption::kAdoptTimeoutMs`) elapses -> call `RunDivergenceSweep_` once, disarm. Move the inline `ForceGarbageCollection()` (1053) into the deferred run so it fires once after the real sweep. |
| `src/coop/event_feed.cpp` (466 LOC, ok) | `SnapshotComplete` case (360-386) | Line 380: `DestroyUnclaimedDivergentProps(localPlayer)` -> `remote_prop_spawn::ArmDivergenceSweep(localPlayer)`. (Now symmetric with the NPC side at 385 which also just arms.) |
| `src/coop/event_feed.cpp` | `SnapshotBegin` (348-358) + client world-ready path | Ensure `BeginClaimTracking()` (357) also resets the sweep latch via `OnClientWorldReadyResetSweep()` so a re-bracket re-arms (RC-B two-load case). |
| net-pump client tick (the function that calls `npc_adoption::Tick()` / `npc_mirror::TickClientNpcs` — `net_pump.cpp` / `subsystems.cpp`) | tick driver | Add `coop::remote_prop_spawn::TickClientReconcile();` adjacent to `npc_adoption::Tick()`. Zero steady-state cost: single bool check when disarmed (mirror npc_adoption's "NO-OP once converged"). |

**Keyless-skip:** keep the `key.empty()||key==L"None"` branch (`986-990`) as a true-transient guard, but update the comment — it is no longer the adoption hole once quiescence-gated; it only catches genuinely never-keyed actors. The forensic WARN at 1010 stays (now it should print 0 for late-tail props on a healthy join — a regression tripwire).

**Modular cap (RULE 2026-05-25):** `remote_prop_spawn.cpp` is **1068 LOC (over the 800 soft cap, under the 1500 hard cap)**. This change is net-small (split one function, add a tick + latch ≈ +60 LOC -> ~1128). **FLAG with an extraction proposal:** extract the claim-tracking + divergence-reconcile subsystem (`BeginClaimTracking`/`RecordClaim`/`ArmDivergenceSweep`/`TickClientReconcile`/`RunDivergenceSweep_` ≈ ~330 LOC) into a new `coop/prop_divergence_sweep.{h,cpp}` as a **separate prior commit** (RULE: extract first, then add). This is the natural one-feature-per-file split; `remote_prop_spawn.cpp` keeps the OnSpawn/bind machinery. Do the extraction commit, then the deferred-sweep commit on top.

**Coordinate with v77 (ATVs):** ATVs are full shared physics entities purchased at runtime, not save-loaded props — they ride their own `atv_sync` channel (`subsystems.cpp:159`) and do not enter the prop divergence sweep universe, so there is **no interaction**. Just rebase the sweep refactor onto the v77 head; no protocol coordination needed.

---

## 4. RISKS / TEST

### Exact repro (the user's case)
1. Host: New Game (fresh save, current version). Place/have multiple turned-OFF kerfurs (prop_kerfurOmega_C lying on ground). **Save.**
2. Host: load that save, turn **ONE** kerfur ON (radial -> turn on -> NPC spawns, prop K2_DestroyActor'd). **Do NOT re-save** (the bug precondition — confirmed `kerfur_convert` writes no save).
3. Client: join via save-transfer.
4. **PASS:** client sees exactly ONE active kerfur NPC (adopted/fresh-spawned via npc_adoption) and **ZERO** ghost props for that kerfur. The other turned-OFF kerfurs remain as props on BOTH peers.
5. **FAIL (current):** client shows the ghost turned-OFF prop next to the active NPC.

### Instrumentation to confirm RC before/after
- Before the fix, the existing WARN (`remote_prop_spawn.cpp:1010`) should name `prop_kerfurOmega_C` (or `prop_C` lineage) in `keylessSkippedByClass` on the failing join — that is the smoking gun proving RC-A. After the fix, that count should be 0 and the doomed histogram (`:1035-1041`) should show the kerfur prop destroyed.

### Edge cases
- **Multiple turned-off kerfurs:** only the converted one is absent from the host snapshot; the rest are expressed -> claimed -> kept. The quiesced sweep destroys only the unclaimed converted one. ✓ (No risk of nuking the still-off props — they're claimed.)
- **Host re-saves mid-session after turn-on:** then the on-disk save has the NPC, no prop -> client never materializes the ghost prop. Fix is a no-op (nothing unclaimed to sweep). ✓
- **2nd joiner:** each join arms its own bracket + quiesced sweep independently; the deferred latch is per-client-tick (single client process). ✓
- **Quiescence false-positive (sweep runs too early):** mitigated by N-consecutive-stable-scans + the existing `RecordClaim` claim set — a still-loading legit prop that the host DID express is claimed, so even an early sweep keeps it. The only actor at risk is an unclaimed one, which is precisely a divergent ghost. The hard deadline (`kSweepDeadlineMs`) guarantees the sweep eventually runs even if the world never fully quiesces.
- **Quiescence never reached / client stuck loading:** hard deadline fires the sweep anyway (parity with npc_adoption's 8s timeout) — never strands a ghost forever.
- **Performance:** the tick driver is a single bool check when disarmed; the stability scan is the same single GUObjectArray walk the sweep already does, now run a few times during the ~600ms quiesce window on the cold join path only. No per-frame steady-state cost. (Audit per the perf template before handoff.)

### Pre-handoff
Run the full pre-deploy checklist: hot-path re-entry audit on `TickClientReconcile` (HOT/WARM/COLD table), modular-cap evidence (`wc -l` showing the extraction landed remote_prop_spawn under control and the new file under 800), deploy x4, ≥30s LAN smoke with the exact repro above, host+client log diff (confirm the keyless WARN went to 0 and the doomed histogram named the kerfur prop). Do NOT mark done on a clean build alone.

---

**Key files (absolute):**
- `d:\Projects\Programming\VOTV_MP\src\votv-coop\src\coop\remote_prop_spawn.cpp` (sweep — the fix site; 1068 LOC, flag for extraction)
- `d:\Projects\Programming\VOTV_MP\src\votv-coop\src\coop\event_feed.cpp` (`:380` inline call -> arm; `:357` re-arm)
- `d:\Projects\Programming\VOTV_MP\src\votv-coop\src\coop\npc_adoption.cpp` (`:183-228` — the proven precedent pattern to mirror)
- `d:\Projects\Programming\VOTV_MP\src\votv-coop\src\coop\prop_snapshot.cpp` (`:310-316`, `:375-383` — bracket defer/abort, RC-B context)
- `d:\Projects\Programming\VOTV_MP\src\votv-coop\src\coop\subsystems.cpp` (`:148-194` ConnectReplayForSlot — additive replay, no change)
- `d:\Projects\Programming\VOTV_MP\src\votv-coop\include\coop\net\protocol.h` (`:697` v77 head — no bump needed)
- New: `d:\Projects\Programming\VOTV_MP\src\votv-coop\src\coop\prop_divergence_sweep.{h,cpp}` (extraction target)
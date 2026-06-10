# Client world-divergence physics fix — final design (2026-06-09)

Status: **CORRECTED + RE-SETTLED 2026-06-09 (session 5, post-compact). The prior
"IMPLEMENTATION-READY Candidate B" below was FALSIFIED by verification and is SUPERSEDED
by the ★ CORRECTED PLAN ★ section immediately below.** Current deployed build = `D7A1D3DAE83D`.

---

## ★ CORRECTED CONVERGED PLAN (session 5) — BUILD THIS, supersedes everything below ★

### What verification falsified (RULE-1 "verify don't guess" paid off)
- **The 3 classes are NOT spawn-only.** bp_reflect parsed the cooked persistent level
  (untitled_1.umap, 50,951 exports): `actorChipPile_C` = **34 PLACED** (+RNG), `trashBitsPile_C`
  = **392 PLACED with baked DETERMINISTIC base64-GUID keys identical on both peers** (+RNG),
  `prop_garbageClump_C` = **1 PLACED** (+RNG). => the falsified Candidate B (blanket class-sweep
  at SnapshotBegin) would DESTROY 427 deterministic shared-scenery instances. DEAD approach.
- **The "767 diverged" is NOT the crash and is a misleading smoke metric.** Real client log:
  the diverged exact-key reconciles are deterministic-keyed actors whose host-save position
  differs from client-fresh; 25 at d=0cm (rotation-only), 233 at d>100cm. BUT the dominant
  simulating class among them is **320 `prop_C` (Aprop_C, physFlags=0x05 SIMULATING)** + 81
  trashBitsPile — so the exact-key TELEPORT (remote_prop_spawn.cpp:248-255) WAKES 320 settled
  prop_C bodies into the client's divergent layout = the permanent ~45ms/frame physics cost.
- **Boot-time spawner suppression STRUCTURALLY CANNOT win the race** (re-confirmed): the log
  shows spawner interceptors install at CONNECT (after world-gen); the spawner BP class isn't
  in GUObjectArray at DLL-attach (prior boot-arm caught 11/767). Do NOT pursue boot suppression.
- **KILLER DIAGNOSTIC**: the HOST runs the identical DLL at 110-120 FPS while the client sits at
  44-50. Our DLL subsystems = ~3ms/frame (~1ms without perf_probe selftime). The other ~19ms is
  ENGINE PhysX from woken bodies. => it is NOT a hot-path bug in our code; it is the physics wake.
  (Verified by 3 agents: 2 architects + 1 diagnosis, converged.)

### ROOT CAUSE (one mechanism, two stamp/reconcile sites)
Settled host props (sleep=true => SP `init()` leaves them NON-simulating) are mirrored on the
client as SIMULATING bodies, which penetrate the client's RNG-divergent layout => PhysX ejects
("flying objects") + permanent solver cost + intermittent fatal crash. Two sites put a simulating
body into divergence: (1) Path C fresh-spawn stamps kSimulatePhysics unconditionally; (2) the
exact-key/fuzzy reconcile TELEPORTS the client's existing simulating body to the host position.

### ★ P1 — THE FPS + CRASH FIX (high-value, SP-faithful, build first) ★
Make the client mirror of a SETTLED host prop NON-simulating (kinematic), matching SP exactly
(`Aprop_C::init() = SetSimulatePhysics(NOT(static||frozen||sleep))`). VERIFIED: `Aprop_C.sleep
@ 0x02DD` (SDK dump prop.hpp line 19 `bool sleep; // 0x02DD`). `DriveTogglePhysics(actor,mesh,sim)`
(remote_prop.cpp:308-311) already covers Aprop_C(mesh)+non-Aprop_C(root). Path C ALREADY honors
physFlags via DriveSimulate (remote_prop_spawn.cpp:523-524) => the :281 stamp fix auto-covers it.
1. `ue_wrap::prop::IsSleeping(prop)` reads `Aprop_sleep@0x02DD` (NEW; sdk_profile.h + prop.h/cpp).
2. `prop_snapshot.cpp:281`: SP-parity stamp — `kSimulatePhysics` ONLY when
   `IsDescendantOfProp(obj) && !(IsStatic||IsFrozen||IsSleeping)`; else physFlags=0 (non-Aprop_C
   kinematic too). Only :281 changes — prop_lifecycle.cpp:271/:433 (Init/takeObj POST) fire on
   FRESH gameplay spawns which ARE actively simulating => leave them (architect B, confirmed).
3. `remote_prop_spawn.cpp` NEW static `ReconcileToHostPhysics(actor, payload.physFlags)`: when
   host NOT simulating (`!(physFlags&kSimulatePhysics)`), `DriveTogglePhysics(actor,mesh,false)`
   BEFORE the teleport (off->place; the user's insight). Called in the exact-key branch (after
   the IsActorUnderAnyDrive drive-skip return, ~:205) AND the fuzzy branch (after its drive-skip,
   ~:306). SAFE where kAtRest's PutRigidBodyToSleep crashed: SetSimulatePhysics(false) removes the
   body from the solver island (no de-penetration), unlike poking a body still in-solver. The
   epsilon-gate STAYS (micro-opt: skip the redundant SetActorLocation when aligned).

### ★ P2 — THE DOUBLING FIX (after P1 smoke-green; cosmetic+memory, not crash) ★
Claim-tracking + destroy-unclaimed (both architects converged). During the drain, mark every
client actor matched by exact-key/fuzzy as CLAIMED (a per-connect `unordered_set<void*>` in
remote_prop_spawn.cpp, cleared at SnapshotBegin). At SnapshotComplete, walk the client's keyed
RNG-spawnable-class props (NEW `IsRngDivergentClass`: chipPile/trashBitsPile/garbageClump lineage
+ mature mushroom) and `DestroyLocalProp(unclaimed, deferred=false)` — these are the client's
RNG-extra copies the host doesn't have. SOUND because the 392 deterministic trashBitsPiles +34
chipPiles get exact-key/fuzzy CLAIMED => protected; only divergent RNG extras are unclaimed.
Hooks: BeginClaimTracking at event_feed.cpp SnapshotBegin (~639), DestroyUnclaimed at
SnapshotComplete handler. Do NOT add garbageClump to IsWireSuppressedPropClass (breaks client
grab morphs — architect B). After P2 green: RULE-2 delete the fuzzy branch (:287-368) +
RestoreCollisionIfNeeded if confirmed dead.

### Smoke metric correction
The `diverged` count is NOT the failure signal (it counts harmless-after-P1 reconciles). The real
signals: (a) NO PhysX crash / posted-task-FAULT / game Fatal, (b) sustained client FPS within
~80% of HOST FPS after the connect-drain settles (host-vs-client comparison = the killer metric),
(c) RSS stable. Fix smoke_phystele to compare host vs client steady FPS, not count reconciles.

### Build order: P1 (IsSleeping -> :281 stamp -> ReconcileToHostPhysics) -> build/deploy/smoke
(FPS parity, no crash) -> P2 (claim-tracking) -> smoke (no doubles) -> RULE-2 fuzzy delete ->
hands-on -> perf_probe selftime OFF. Agents: wf architects a5b31b15/a910ea3d + diagnosis ad7f1e7b.

### ★ P1 IMPLEMENTATION STATE (session 5, 2026-06-10) -- code-complete, build-clean x2 ★
DONE + refined:
- `Aprop_sleep=0x02DD` (sdk_profile.h) + `ue_wrap::prop::IsSleeping()` (prop.h/cpp). Offset CONFIRMED from SDK dump prop.hpp:19.
- `prop_snapshot.cpp:281`: SP-parity stamp -- kSimulatePhysics ONLY when `IsDescendantOfProp && !(IsStatic||IsFrozen||IsSleeping)`; non-Aprop_C -> physFlags=0. kIsHeavy/kFrozen now only set inside the Aprop_C gate.
- `remote_prop_spawn.cpp` `ReconcileToHostPhysics(actor, physFlags)` -- **Aprop_C-ONLY** (GetStaticMesh!=null -> DriveSimulate(mesh,false)); skips non-Aprop_C (kinematic BY DESIGN via the GetStaticMesh-null 2a-UAF gate; diagnosis: their teleports are physics-harmless -> NO touch -> no UAF). Called in exact-key branch (after the drive-skip return) + fuzzy branch (before SetActorLocation), BEFORE the teleport (off->place).
- Path C (fresh spawn) ALREADY honors physFlags via DriveSimulate(mesh, physFlags&kSimulatePhysics) -> the :281 fix auto-covers Aprop_C fresh-spawns; non-Aprop_C need nothing (audit Issue-1 dissolved by Aprop_C-only scope).
- `DriveTogglePhysics` kept FILE-LOCAL (the public-move was reverted -- Aprop_C-only ReconcileToHostPhysics doesn't need it; RULE 2 don't expose unused).
AUDIT (feature-dev:code-reviewer acf8221e): 0 CRITICAL. Issue-1 resolved by the Aprop_C-only refinement. NOTE: remote_prop.cpp = 896 LOC (>800 soft cap) -- extraction proposal: move RegisterPropMirror/mirror-manager internals to coop/prop_mirror_manager.cpp (DEFERRED, not this change).
SMOKE: metric fixed (host-vs-client FPS parity ratio>=0.6, NOT diverged-count). First smoke run was DEGENERATE (host mid-level-re-open -> 88 live/1383 dying props streamed; client player died, UI looped, 9GB balloon) -> added `--host-settle 30` (wait for host PLAY READY + drain dying elements before client connect). RE-RUN in progress.
NOTE on the 88/1383: the host's story-load re-issues `open untitled_1`, tearing down the boot-world's prop elements (linger "dying"); connecting mid-transition = degenerate. PRE-EXISTING, not P1. The dying-drain rate is the settle target.

### ★ P2 IMPLEMENTATION STATE (session 5 cont., 2026-06-10) -- code-complete, audited, build-clean ★
DONE (all UNCOMMITTED):
- **3 claim sites in `remote_prop_spawn.cpp` OnSpawn** (not 4): `RecordClaim(existing)` right after `if (existing) {` (covers exact-key main + the drive-skip early return), `RecordClaim(fuzzy)` right after `if (fuzzy) {` (same coverage), `RecordClaim(spawned)` right after the BeginDeferred null-check — **BEFORE FinishSpawningActor** (audit CRITICAL: a Finish-failure must leak one claimed half-spawned actor — the exact pre-P2 behavior — not leave an unclaimed half-CONSTRUCTED actor for the sweep to K2_DestroyActor mid-construction: no BeginPlay, unregistered PhysX body -> the kAtRest crash class). OnConvert routes through OnSpawn (remote_prop.cpp:787) -> convert-born piles claimed automatically.
- **State**: anon-namespace `unordered_set<void*> g_claimedActors` + `bool g_claimTrackingActive` + `RecordClaim()` (1-bool-read no-op outside a join). Game-thread only (event_feed drain + net_pump disconnect edge) — no mutex. Pointers only ever compared, never dereferenced.
- **Public API** (remote_prop_spawn.h): `BeginClaimTracking()` / `DestroyUnclaimedDivergentProps()` / `ResetClaimTracking()`. Sweep = two-phase (collect during ONE GUObjectArray walk — class gate FIRST, then Default__ CDO skip, then IsLive; destroy AFTER the walk so K2_DestroyActor's GUObjectArray mutations never invalidate iteration), gated on g_claimTrackingActive (a Complete without Begin must not sweep an empty claim set — WARN+skip), echo-suppressed destroys via the promoted `coop::prop_lifecycle::DestroyLocalProp(actor, deferred=false)` (public API now; MarkIncomingDestroy before K2_DestroyActor).
- **Wire-up** (event_feed.cpp): `BeginClaimTracking()` after join_progress::BeginSnapshot in the SnapshotBegin case; `DestroyUnclaimedDivergentProps()` after join_progress::Complete() in SnapshotComplete. Ordering safe: Begin/PropSpawn*/Complete all Lane::Bulk, dispatched INLINE on the game thread by the drain (OnSpawn inline at event_feed.cpp:314) -> all claims strictly precede the sweep. Unicast-safe: prop_snapshot sends both brackets SendReliableToSlot(joiner) + handlers validate senderPeerSlot!=0 / Role::Host -> a second connected client never arms/sweeps.
- **Disconnect reset** (net_pump.cpp): `ResetClaimTracking()` at BOTH teardown sites (aggregate-disconnect + local-player-death) — a mid-snapshot drop must not leave dangling actor pointers armed across sessions.
- **`ue_wrap::prop::IsRngDivergentClass(cls)`** = pure cached-pointer compares (chipPile/trashBitsPile/garbageClump via the existing extra-bases trio + NEW file-scope `g_propFoodMushroomCls` atomic). **NEW `ResolveRngDivergentBases()`** called ONCE at sweep start (ResolveExtraBases + one-shot mushroom FindClass; returns unresolved-lineage count -> WARN "instances skipped this join"). Kills the audit-Finding-4 silent-miss AND the O(N_slots x N_objects) hazard (a FindClass retry per sweep slot would re-walk GUObjectArray 250k times).
- **KEY DESIGN CORRECTION (deviation from the pre-compaction plan, documented in IsWireSuppressedPropClass body)**: extending IsWireSuppressedPropClass to chipPile/trashBitsPile is provably WRONG — the predicate is SYMMETRIC across 3 call sites incl. the snapshot enumerate-skip (prop_snapshot.cpp:256): adding trashBitsPile would drop the 392 level-PLACED deterministic-key piles from the snapshot -> unclaimed -> MY OWN sweep destroys them; adding chipPile would destroy the client's v52 convert-born piles at Init. DROPPED; claim-sweep alone closes connect-time doubling.
- **Audit** (feature-dev:code-reviewer a6c568a79cdf00acd): 1 CRITICAL (claim-before-Finish, FIXED + dup claim removed per RULE 2), Finding 3 (swept watched piles -> spurious PropDestroy) verified structurally UNREACHABLE (trash_collect_sync::OnDisconnect clears g_watchedPiles at both teardown sites; in-join WatchPile always follows a claim) — documented in the sweep, no defensive API (RULE 1). Finding 4 fixed via ResolveRngDivergentBases.
- File sizes: prop.cpp 613 / prop_lifecycle.cpp 704 / remote_prop_spawn.cpp 712 / headers small — all under the 800 soft cap. event_feed.cpp 1274 / net_pump.cpp 1092 pre-existing-deferred (this diff: ~15/2 lines).
- **P2 SMOKE RESULT (smoke_phystele 2026-06-10, exit PASS)**: client steady FPS 88 vs host 115 = **ratio 0.77** (>=0.60 GREEN); **RSS ratio 1.09x** (client 4443MB / host 4067MB, <=1.5 GREEN); 2306 host snapshot candidates; 1399 harmless kinematic reconciles; 0 posted-task FAULTs; no Fatal/AV either peer. Client log: `claim tracking ARMED` at SnapshotBegin (11:53:44) -> `claim sweep -- 465 RNG-divergent-class actors live, 349 claimed by the host snapshot, 116 unclaimed divergent locals destroyed` (11:53:46). **All 116 destroys logged `was wire-received destroy -- skip rebroadcast`** = echo suppression PROVEN; host log has **0 PropDestroy received + 0 claim lines** (never armed -> unicast safety holds); no "not armed" WARN; no unresolved-base WARN; zero ERROR/FAULT/SEH both logs (case-SENSITIVE scan -- note `DefaultFOV` false-matches FAULT case-insensitively). The 116 (vs the "hundreds" estimate) is expected: the smoke client connects ~40s after boot, before chipPiles/mushrooms accumulate; a longer-lived client world sweeps more.
- **P2 touched files (ALL UNCOMMITTED, on top of the P1 diff)**: `include/ue_wrap/prop.h` (+IsRngDivergentClass +ResolveRngDivergentBases docs), `src/ue_wrap/prop.cpp` (+g_propFoodMushroomCls file-scope atomic, +ResolveRngDivergentBases, IsRngDivergentClass=pure compares), `include/coop/prop_lifecycle.h` + `src/coop/prop_lifecycle.cpp` (DestroyLocalProp promoted to public API + the P2 design note inside IsWireSuppressedPropClass), `include/coop/remote_prop_spawn.h` + `src/coop/remote_prop_spawn.cpp` (claim state + 3 claim sites + Begin/Destroy/Reset + sweep), `src/coop/event_feed.cpp` (2 hook lines at SnapshotBegin/Complete), `src/coop/net_pump.cpp` (ResetClaimTracking at both teardown sites + include).
- **NEXT (in order)**: (1) HANDS-ON user test P1+P2 together (watch: client FPS ~= host after connect, no flying props, no doubled piles/mushrooms near base, pile grab/convert still works both directions). (2) ~~After green: RULE-2 delete the fuzzy branch~~ **FALSIFIED 2026-06-10 hands-on: the fuzzy branch is LOAD-BEARING** -- 117 fuzzy binds in the real run (53 prop_C, 19 mushroom, 17 prop_swinger, 15 asologoPiece, 8 torch...): runtime-keyed (crypto-random per-machine `generateRandomKey`) but UNMOVED props bind ONLY via fuzzy. v54 hardened it with the propName identity gate instead. KEEP. (3) perf_probe=0 + perf_probe_selftime=0 in the game inis before ship (still ON). (4) Deferred: remote_prop.cpp 896>800 extraction (prop_mirror_manager), event_feed drain budget-cap, ~~post-connect RUNTIME divergent-spawn verification (unverified problem)~~ **VERIFIED REAL 2026-06-10** (host piles born post-connect never mirror; see votv-snapshot-adoption-root-causes-2026-06-10.md), SEPARATE pre-existing host crash: 2113 IsHeavy use-after-free faults (votv-coop.dll+0x135A08). **2026-06-10 hands-on came back RED -- the P1+P2 root-cause session is `votv-snapshot-adoption-root-causes-2026-06-10.md` (empty-bracket mass destroy + white-cube identity + keyless-pile snapshot hole); this section's smoke-PASS state was real but the smoke missed all three (host-settle hid the bracket race; autonomous smoke can't see visuals).**

---

## ★★ IMPLEMENTATION-READY PLAN (3 code-verified workflows converged) — BUILD THIS ★★

### Progress this session (all UNCOMMITTED)
- VERIFIED: `Aprop_C.sleep @ 0x02DD` (UE4SS ObjectDump `prop_C:sleep [o:2DD]`); the test
  client uses the **play path** (`harness.cpp:643`, env net role), NOT the menu path :448.
- BUILT + VALIDATED the autonomous test `tools/mp.py smoke_phystele` (+ registered in argparse).
  It FAILs RED on the current build: **767 diverged reconciles, client FPS min 3, RSS +2.7 GB,
  client process CRASHED**. Greps `diverged (d=`, `posted task FAULT`, window-title "crash",
  game-log `Fatal error`/AV, FPS<30, RSS>500MB. Core deterministic signal = the `diverged`
  count (current build hundreds -> after fix ~0). Must go GREEN before "done".

### ROOT-CAUSE 1 — suppress the client's divergent RNG props (workflow `wf_5360da78-7c4`, 28/27/24)
**Removal IS necessary (not optional):** if the client KEEPS its divergent copies, the host
snapshot's de-dupe is KEY-based (`ResolveLiveActorByKey` remote_prop_spawn.cpp:176) + 30cm
fuzzy (`FindNearbySameClass` :287). RNG props minted their OWN NewGuid key per peer -> exact
miss; >30cm divergent -> fuzzy miss -> Path C spawns the host's copy IN ADDITION -> **DOUBLED
props**. physFlags fixes the CRASH; removal fixes the DUPLICATION. Both required, independent.

**Chosen mechanism = Candidate B (connect-time enumerate+destroy) + A-complement.** (A-at-boot
REJECTED: Aprop_C/Aactor_save_C not in GUObjectArray at DLL-attach; prop_lifecycle::Install
gates on FindClass resolving = only during/after world-gen. C force-load REJECTED: no
force-load primitive exists; FindClass/FindObject are lookups-of-resident only.)

**B hook points (all file:line-verified):**
1. **NEW `ue_wrap::prop::IsRngDivergentClass(void* cls)`** (add to prop.cpp ~96-157 + prop.h):
   true for `IsChipPile` (actorChipPile lineage), `WalksToBase(cls, TrashBitsPileCls())` (the
   392 `trashBitsPile_C` = **Aactor_save_C lineage, NOT Aprop_C**), `WalksToBase(cls,
   GarbageClumpCls())`, + mushroom classes (`prop_food_mushroom_C` + `PropMushroomGrowingClass`).
   Lineage-based -> covers BOTH Aprop_C AND Aactor_save_C. Bases already resolved by
   `ResolveExtraBases()` (prop.cpp:96-121).
2. **NEW `prop_lifecycle::DestroyDivergentRngProps()`**: a `SeedWalk_`-shaped GUObjectArray walk
   (template = prop_element_tracker.cpp:488-501) filtered by `IsKeyedInteractable && IsRngDivergentClass`,
   calling `DestroyLocalProp(obj, /*deferred=*/false)` each. Gate `role()==Client`. Does its OWN
   fresh walk (not the stale g_knownKeyedProps set).
3. **Trigger at the client `SnapshotBegin` handler `event_feed.cpp:622-639`**, right after
   `join_progress::BeginSnapshot(p.propTotal)` (:639), BEFORE any PropSpawn drains. RACE-FREE: host
   sends SnapshotBegin on `Lane::Bulk` strictly ahead of the first PropSpawn (prop_snapshot.cpp:122-130;
   session_lanes.h:55 both on Bulk -> GNS in-lane order). (The net_pump.cpp:596 connect-edge block is
   the WRONG hook -- it only announces flashlight to host, receives no PropSpawn; TriggerForSlot is
   host-only.)
4. **Destroy primitive = `DestroyLocalProp(actor, deferred=false)`** (prop_lifecycle.cpp:102-132):
   the PROVEN mushroom-7 path -- `MarkIncomingDestroy` (:124) BEFORE `K2_DestroyActor` (:125) -> the
   PRE observer ConsumeIncomingDestroy's it -> host's canonical copy never destroyed; IsLive-guarded.
   Direct loop, ZERO new RegisterInterceptor (interceptor table 15/16 untouched).
5. **arirTrasher caveat (the one soft spot):** `arirTrasher_C` emits GENERIC Aprop_C garbage
   (NewGuid) but hand-placed cooked Aprop_C FURNITURE is also Aprop_C -> a blanket Aprop_C destroy
   would nuke furniture. RESOLUTION: scope the CONNECT sweep to the **non-Aprop_C** divergent classes
   only (chipPile/trashBitsPile/garbageClump + mushroom) -- these are SPAWN-ONLY in VOTV (no cooked
   placed instances). arirTrasher's Aprop_C output is caught by the A-complement instead. Do NOT use
   a {GUID}-key-format heuristic on Aprop_C.
6. **A-complement (post-connect dynamic re-spawns):** extend `IsWireSuppressedPropClass`
   (prop_lifecycle.cpp:566-568, currently `== PropMushroomGrowingClass`) to the divergent set incl.
   arirTrasher/prop_food_mushroom_C. The existing client Init-POST gate (prop_lifecycle.cpp:207-213)
   then `DestroyLocalProp(self, deferred=true)` for any spawning AFTER the observer installs (the gap
   before garbage_sync interceptors latch). Sits BEFORE the :221 broadcast fall-through (already does).
   Zero new infra -- a 2-line class-set expansion.

### ROOT-CAUSE 3 — sleep-aware physFlags (settled)
`prop_snapshot.cpp:281` stamps `kSimulatePhysics` UNCONDITIONALLY -> settled host props spawn
SIMULATING on the client = the penetrate/fly/crash. FIX: stamp `kSimulatePhysics` only when host
prop `sleep==false`; **physFlags=0 for settled props** (`sleep==true`). Needs NEW
`ue_wrap::prop::IsSleeping()` reading `Aprop_C.sleep @0x02DD` (CONFIRMED). SP parity: `init()` =
`SetSimulatePhysics(NOT(static||frozen||sleep))`. NOTE: non-Aprop_C classes (chipPile/trashBits/
clump) have `GetStaticMesh==null` (prop.cpp:256) so are already physics-free in Path C -- VERIFY
Path C (remote_prop_spawn.cpp:369+) treats them kinematic.

### RULE-2 deletion — CORRECTED scope
The exact-key branch (remote_prop_spawn.cpp:245-256) is ALREADY alignment-gated (only teleports
`!locAligned||!rotAligned`; identical base scenery is sub-mm) -> NOT unconditional -> KEEP (serves
deterministic shared base props). The genuinely-unconditional teleport is the FUZZY branch
(:287-340); after B removes divergent actors pre-snapshot, fuzzy is dead for divergent classes ->
the SetActorLocation/rekey (:307-340) is the RULE-2 delete candidate. VERIFY no non-divergent class
relies on fuzzy first (mushrooms used it historically; now in the divergent set, so OK -- confirm in smoke).

### Build order
0. (done) smoke_phystele test -- FAILs red.
1. bp_reflect VERIFY actorChipPile_C/trashBitsPile_C/prop_garbageClump_C are SPAWN-ONLY (no cooked
   placed instances) -- the only un-byte-verified discriminant assumption.
2. `ue_wrap/prop::IsRngDivergentClass` + `IsSleeping`.
3. `prop_lifecycle::DestroyDivergentRngProps` + extend `IsWireSuppressedPropClass`.
4. `event_feed.cpp:639` call the sweep.
5. ROOT-CAUSE 3 sleep-aware physFlags in `prop_snapshot.cpp:281`.
6. Build -> deploy -> `smoke_phystele` must go GREEN (diverged ~0, no double-props, no crash, FPS
   parity, no spurious host PropDestroy) -> hands-on.
7. RULE-2 delete the fuzzy teleport (after green).

### Pre-build checks still open (verify, do not assume)
(a) the 3 swept classes are spawn-only (bp_reflect). (b) Path C stamps non-sim / is kinematic for
non-Aprop_C. (c) smoke log: client `BeginSnapshot` precedes first `OnSpawn`; PropSpawn maps to
Lane::Bulk too. (d) `DestroyDivergentRngProps` walks fresh (not stale set).

KEY FILES: event_feed.cpp:622-639 (sweep trigger), prop_lifecycle.cpp:102-132 (DestroyLocalProp)
/:566-568 (IsWireSuppressedPropClass) /:207-213 (client Init-POST gate), prop_element_tracker.cpp:488-501
(SeedWalk template), prop.cpp:96-157 (add IsRngDivergentClass), remote_prop_spawn.cpp:245-256 (keep)
/:287-340 (fuzzy delete) /:369+ (Path C), prop_snapshot.cpp:122-130/:281, net_pump.cpp:1125.

---

(historical sections below: the RULE-1 verdict that opened ROOT-CAUSE 1, and the superseded two-layer design)

---

## ★ RULE-1 ADVERSARIAL VERDICT (workflow `wf_c6544ef7-f7c`, 3 agents converged 29/29/27) — SUPERSEDES the two-layer design below ★

The user challenged the two-layer design with "ask agents what's best per RULE 1."
Verdict: **Layer 1 (freeze-before-place + frozen-until-grab) IS A CRUTCH. Do not build
it.** The RULE-1-best architecture is simpler — NO freeze primitive anywhere:

**WHY Layer 1 is a crutch (the settling code fact):** the only `SetActorLocation`-on-a-
SIMULATING-body sites are the exact-key (`remote_prop_spawn.cpp:252-255`) + fuzzy
(`:309-312`) diverged branches, which fire ONLY when the client already generated a
divergent RNG prop. Suppress the spawners (root cause) -> `ResolveLiveActorByKey` returns
null + `FindNearbySameClass` finds nothing -> those branches are NEVER reached -> nothing
for a freeze belt to protect. (Also: for the dominant 392 `trashBitsPile_C`, the fuzzy
branch is structurally unreachable -- it gates on `IsDescendantOfProp` and trashBitsPile is
`Aactor_save_C`, not `Aprop_C` -> Layer 1's freeze there is DEAD CODE for the largest class.)

**THE REAL ROOT CAUSE of the fly/crash (newly found):** `prop_snapshot.cpp:281`
UNCONDITIONALLY stamps `kSimulatePhysics` on EVERY prop -> even host props that are AT REST
spawn SIMULATING on the client -> penetrate the client's layout -> PhysX ejects them (fly) /
crashes. SP does NOT do this: `Aprop_C::init()` = `SetSimulatePhysics(NOT(static OR frozen OR
sleep))`; a settled/save-loaded prop has `sleep=true` -> `SetSimulatePhysics(false)`. There
is NO "frozen-until-grab" state in SP -- only dynamic-settling then non-simulating.
"frozen-until-grab" is an INVENTED coop state (violates principle 6). MTA agrees: objects
stream in unfrozen.

**THE RULE-1-BEST ARCHITECTURE (implement, then verify, then RULE-2 delete):**
1. ROOT-CAUSE -- **arm spawner suppression BEFORE client BeginPlay.** The gate
   `MAKE_SPAWNER_CANCEL` (`garbage_sync.cpp:118-119`) reads the session ptr, NULL at
   BeginPlay (`BootStorySaveBlocking` at `harness.cpp:448` runs BEFORE `StartCoopSession`
   `:454`). FIX: set an atomic `g_clientRoleArmed` at `harness.cpp:433` (role already known:
   `pending.role==Role::Client && !join_progress::Active()`), before `:448`; the cancel
   callbacks fire on that flag even with a null session. ~1-line + gate adjust. ALSO move the
   suppressor REGISTRATION early enough to be in the table before world-gen.
2. ROOT-CAUSE -- **add the mushroom spawner** (`AmushroomSpawner_C`/`AmushroomMaster_C`) to
   the `garbage_sync.cpp:161-175` suppressor list (the ONE residual not covered today; the
   fuzzy branch still fires on a live mushroom until then). Then `RestoreCollisionIfNeeded`
   (`:94-132`) RETIRES (RULE 2).
3. ROOT-CAUSE -- **fix the physFlags stamp** (`prop_snapshot.cpp:281`): stamp
   `kSimulatePhysics` ONLY when the host prop is actively simulating (`sleep==false`);
   stamp **physFlags=0 for settled props** (`sleep==true`) -> Path C spawns them
   NON-simulating at the host transform in identical static geometry = nothing to penetrate,
   no PhysX body to eject = matches SP exactly. Needs a NEW `ue_wrap::prop::IsSleeping()`
   accessor reading `Aprop_C.sleep` (does NOT exist today).
4. DELETE (RULE 2, AFTER a smoke verifies zero divergent props generated): the exact-key
   teleport block (`remote_prop_spawn.cpp:248-255`) + the whole fuzzy diverged teleport+rekey
   block (`:291-367`). KEEP the locAligned skip (`:245-247`), the `IsActorUnderAnyDrive`
   guards, the eid/key mirror-register, and Path C.
5. DO NOT BUILD: Layer 1 freeze, the frozen-actor map, thaw-on-grab hooks. No residual needs
   a safe-placement freeze -- every case lands on locAligned-skip, the drive-skip guard, or a
   Path-C non-simulating placement. (Grab re-enables physics via the EXISTING explicit
   `DriveTogglePhysics` edge, `remote_prop.cpp:308-311/:340` -- engine does NOT auto-thaw.)

**Residual coverage (all by EXISTING code or the fix itself):** hand-placed=locAligned skip;
host-held=IsActorUnderAnyDrive skip; host-released-settled=Path C physFlags=0;
host-released-midflight=Path C kSimulatePhysics->settles; host-only-spawn=Path C into empty
space; host-MOVED save prop=single exact-key teleport on a NON-simulating body (no live PhysX
body to wake, re-rests in 1 frame, single prop not a storm); late joiner=same pre-BeginPlay arm.

**VERIFY BEFORE BUILDING (do NOT fabricate -- the prior workflow's downfall):**
(a) the `Aprop_C.sleep` byte offset (`@0x02DD` per old research docs, NOT in prop.h which has
only Static@0x02D8/frozen@0x02DA) -- confirm against current prop.json bytecode / sdk dump.
(b) that the `harness.cpp:433` role-flag set precedes EVERY client world-gen path (verify the
play/env boot paths `:643`/`:744` -- host-side, but confirm no client path skips the arm).
Test harness goes FIRST regardless.

KEY FILES: remote_prop_spawn.cpp (teleport :248-367, locAligned :245), prop_snapshot.cpp
(physFlags :281), garbage_sync.cpp (gate :118-119, targets :161-175), harness.cpp
(:433/:448/:454), remote_prop.cpp (no-auto-thaw :308-311/:340), prop.h (missing IsSleeping).

---

## (SUPERSEDED) earlier two-layer design — kept for audit trail
Source: design workflow `wf_55a014a6-eb6` (14 agents). The RULE-1 review above retired its
Layer 1 (freeze belt) as a crutch and found the real root cause (the unconditional
kSimulatePhysics stamp). Layer 2 (spawner suppression) SURVIVES + is sharpened above.

## Verdict on the user's idea ("adopt host world data into the client's fresh world")
**Right architecture, ~80% already built.** BUT the literal "download the .sav file"
is the wrong primitive: the host already holds the authoritative world LIVE as running
actors and `prop_snapshot.cpp` already streams every keyed prop's live transform at
connect. The live stream IS the world-data transfer; no .sav serialize needed.

**Decisive root-cause fact (RE 2026-06-04 §1, re-verified):** the ~1388 diverged
bodies are **NewGuid-keyed RNG-spawned props** (garbage, mushrooms, trash piles), NOT
hand-placed save props. Hand-placed props carry cooked-stable Keys, spawn at IDENTICAL
positions on both peers, and already hit the `locAligned` 2 cm skip
(`remote_prop_spawn.cpp:245`) -> no teleport, no wake. **The problem is exclusively the
RNG-divergent set.** => cleanest realization of the user's idea: **suppress the client's
divergent RNG spawners so those bodies never exist locally; let the existing host-data
fresh-spawn (Path C) be the ONLY copy.** MTA `SendMapInformation` shape (client never
world-generates; receives authoritative records: `CMapManager.cpp:548`).

This is also the user's **off->place->on** insight generalized: freeze (off) before
placing, thaw (on) only when needed; world-matching keeps the placement target clear.

## Architecture — TWO LAYERS, test FIRST
### Layer 1 (ship FIRST — crash/FPS safety belt): freeze-before-teleport + EXPLICIT coop thaw-on-grab
- In the diverged branch, `SetActorSimulatePhysics(existing,false)` (ROOT-component path,
  `engine_attach.cpp:68` -- works for `Aprop_C` AND the 392 `trashBitsPile_C`
  /`Aactor_save_C`; `prop::GetStaticMesh` is NULL for the latter, so DriveSimulate(mesh)
  would silently no-op the DOMINANT class) **BEFORE** `SetActorLocation`. Kinematic body =
  no dynamic PhysX actor = pure transform write = no de-penetration solve = the
  TaskGraph-worker null-deref that killed kAtRest CANNOT occur. MTA `CClientObject::SetFrozen`.
- **THAW IS NOT AUTOMATIC** (`remote_prop.cpp:511` -- the load-bearing fact that falsified
  3 of 4 candidate designs). Coop layer MUST re-enable: track frozen-by-snapshot actors in
  a GT-only `unordered_map<void*,uint32_t>` (actor->eid; UE4 GC non-compacting -> ptrs
  stable; cleared OnDisconnect; ~17 KB). On the local grab edge -- observe
  `GrabComponentAtLocation` POST (light) + `SetConstrainedComponents` POST (heavy), both
  already-registered (`grab_observer.cpp:255/267`) -- resolve grabbed actor; if frozen,
  `SetActorSimulatePhysics(actor,true)` + drop from map. Keep default QueryAndPhysics
  collision (NOT SetActorRootCollisionEnabled(2) = the ungrabbable clump-flight path) so the
  grab sphere-trace still hits.
- Path C fresh-spawn: freeze only DURING the connect drain (gate on new
  `prop_snapshot::IsDrainActive()`); full-sim for mid-session host spawns.

### Layer 2 (root-cause per RULE 1): pre-BeginPlay spawner suppression armed by ROLE
- The suppression MECHANISM exists (garbage_sync cancels 4 spawners) but the GATE is
  broken: `MAKE_SPAWNER_CANCEL` needs `LoadSession()->role()==Client` (`garbage_sync.cpp:118`)
  -- at BeginPlay no session exists -> returns false -> **spawner RUNS**. And
  `InstallSpawnerSuppressors` registers from `InstallObservers` (`net_pump.cpp:321`) at
  SESSION START -- after BeginPlay. So suppressors latch too late.
- FIX: `std::atomic<bool> g_suppressSpawners` armed by `WorldSpawnSuppress::ArmClient()` at
  ROLE-DETECTION time (before `BootStorySaveBlocking` world-gen). The 4 callbacks check the
  flag, not a live Session. **MOVE BOTH the flag AND the interceptor registration earlier**
  (the WEAK-verdict flaw was moving only the flag). Targets:
  `undergroundGarbageSpawner_C::Timer`, `event_trashPiles_C::BndEvt`, `arirTrasher_C::trash`,
  `baseCleaner_trashBits_C::ReceiveBeginPlay` (all ProcessEvent-dispatched; detour live at
  DLL-attach). Mushrooms unchanged (existing Init-POST destroy of mushroom7_C intermediate).
- Result: client fresh world has ZERO divergent simulating bodies; every divergent host prop
  hits Path C fresh-spawn at the host transform in shared static geometry (settles quietly).

### Layer 3 (RULE 2 retire, after Layer 2 verified): delete the exact-key/fuzzy freeze; keep Path C freeze for host-only props. Teleport-converge itself NOT deleted (correct for genuinely host-moved props); only the en-masse divergent reconcile is retired.

## Implementation blueprint (ordered)
- **Step 0: the autonomous test FIRST** (Section below). No physics code ships before the
  reproducer FAILS red on the current build.
- **Step 1 Layer 1:** modify `remote_prop_spawn.cpp` (freeze before SetActorLocation in
  exact-key ~252 + fuzzy ~309; Path C ~521 gate on IsDrainActive); `grab_observer.cpp`
  (upgrade the 2 log-only POST stubs to thaw+clear); `prop_snapshot.{h,cpp}` add
  IsDrainActive(); create `coop/prop_kinematic_state.{h,cpp}` (~70 LOC); CMakeLists.
- **Step 2 Layer 2:** create `coop/world_spawn_suppress.{h,cpp}` (~50 LOC); modify
  `garbage_sync.cpp` (4 callbacks check flag; move registration to boot); `harness.cpp`
  (ArmClient + register at role-detection before BootStorySaveBlocking); bump
  `game_thread.h` kMaxInterceptors 16->24; CMakeLists.
- **Step 3 Layer 3:** delete exact-key/fuzzy freeze; keep Path C freeze.
- **Protocol/version: NONE** (no new packet; physFlags/kSimulatePhysics already on wire;
  kAtRest already reverted). File sizes: all under 800 (remote_prop_spawn ~573->~595).

## The autonomous divergence test (`smoke_phystele`, built FIRST)
`tools/mp.py` + `mp_smoke_phystele.bat`. **Reproduce:** host s_1234, client FRESH
(`VOTVCOOP_FRESH=1` -- the asymmetry IS the bug; do NOT load same save on both = eliminates
divergence) + ~15-30 s pre-connect dwell so the spawner timer fires. **Detect 5 signatures
from existing log markers:**
1. FPS-lock: parse `[perf] PE=.../s ... frames=.../s` (REAL format, no space before `/s` --
   the rejected design's regex was broken -> false PASS); FAIL if post-settle FPS < 30;
   absent `[perf]` line -> FAIL (not WARN).
2. Scenario-armed: count `diverged (d=Ncm)` lines (`remote_prop_spawn.cpp:249`); FAIL if 0.
3. Fault spam: grep `votv-coop.log` for `game_thread: posted task FAULT` AND the GAME's
   `VotV/Saved/Logs/VotV.log` for `Fatal error`/`Assertion failed`. The kAtRest crash was a
   WORKER-thread PhysX AV -> PROCESS DEATH not caught by game-thread SEH -> **process-death
   (peer count < 2) is the PRIMARY crash catcher**; FAULT-spam catches softer GT variants.
4. RSS stability: baseline at `snapshot: drain complete`; FAIL if >500 MB growth / monotonic.
Verdict names the failed signature. Touches only mp.py + .bat + local .ini (no DLL/asset).

## Decisions for the user (recommended defaults)
1. Ship order Layer 1 -> Layer 2 -> retire: **yes** (Layer 1 de-risks the next hands-on in
   one small edit; Layer 2 is the architecture).
2. Phase B (.sav state: quest flags `passEvents`/`allEvents`/`catchedSignals`, container
   inventories, typed loadData): **DEFER** -- physics fix needs none of it.
3. Host-moved/host-only props frozen until first grab then permanently dynamic: **accept**
   (matches MTA frozen-world-object; alternative is the physics fight).

## Biggest risk + de-risk
Thaw-on-grab not actually re-simulating (the fabrication that sank all 3 physics designs).
De-risk: EXPLICIT `SetActorSimulatePhysics(actor,true)` on the verified-observable grab
hooks (root path, lineage-agnostic) + `smoke_phystele` + a hands-on grab-a-frozen-prop+move
assertion PROVE the thaw before "ready." Test FAILs red on current build, must go green
(incl. grab+move) before handoff. NO "should work."

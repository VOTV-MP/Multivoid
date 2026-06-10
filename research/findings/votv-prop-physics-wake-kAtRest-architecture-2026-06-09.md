# Connect-time prop physics-wake — root cause + `kAtRest` architecture (2026-06-09)

Status: **Phase 1 IMPLEMENTED + deployed `BEDBB74D445E` (4 folders, hash-match).
Smoke-verified; HANDS-ON FPS test PENDING.** Phase 2 NOT started (separate commit
after Phase 1 hands-on confirms). All UNCOMMITTED.

### Phase 1 smoke evidence (2026-06-09, 19:45)
- **Load-bearing assumption CONFIRMED:** host stamped **99% kAtRest** (2286/2306
  props on s_1234 -> the bulk ARE at rest on the host; the fix premise holds).
- **AV bug caught + fixed by the smoke (NOT the audit -- audit passed it):** first
  cut called `PutRigidBodyToSleep` unconditionally -> PhysX AV on static/kinematic/
  bodyless roots (host stamps kAtRest for any not-awake prop, incl. non-dynamic
  ones). FIX: `PutActorRootBodyToSleep` now self-gates on `IsAnyRigidBodyAwake`
  (only sleeps a currently-AWAKE = dynamic-simulating body -- exactly the teleport-
  woken target; skips the rest). Re-smoke: **0 AVs**, 99% stamp intact, 746 diverged-
  converges, log clean, RSS stable.
- The autonomous smoke client is idle -> does NOT reproduce the 5-FPS hands-on; the
  FPS RECOVERY itself must be confirmed hands-on (client connect to s_1234 + play).

Source: design workflow `wf_d8aa795e-01d` (14 agents: 5 understand + 4
design + 4 judge + 1 synth) + hands-on perf_probe measurements this session.

---

## THE BUG (measured, not theorized)

The **client locks at low FPS from the moment it connects to a populated host
save, and never recovers.** Measured progression as the fix evolved:

- Pre-any-fix (host s_1234, client fresh New Game): client **20 FPS** locked.
- After epsilon-gate (skip no-op teleports): autonomous idle smoke **44-47 FPS**
  (MISLEADING — idle didn't reproduce), but **hands-on = 5 FPS** locked from
  connect (worse, because that New Game RNG diverged MORE).
- Host holds **110-119 FPS** the whole time on the SAME world.

### perf_probe attribution (the decisive measurement)
At 5 FPS hands-on (≈200 ms/frame), with `perf_probe=1 perf_probe_selftime=1`:
- Our DLL game-thread code ≈ **30 ms/frame** (detour self 0.7 ms; `interactable`
  poll ~70 ms/s; a `ReceiveTick` observer body spiking ~58 ms/s — both scale
  with awake/ticking prop count).
- **~170 ms/frame is ENGINE-side** (GPU ruled out by the user: "my 4090 can run
  10 instances") → **physics**: the rigid-body scene of the woken mirror props.
- `obs/intc body total` = 0.00 steady; detour self ≈ 0.55-0.73 ms — our dispatch
  substrate is NOT the cost. Host-side identical run = 116 FPS.

### Root cause
Both peers run their OWN New Game world-gen with INDEPENDENT RNG. Same prop has
a **stable deterministic Key** but a **divergent spawn POSITION** per peer. At
connect the host streams its authoritative transform; the client de-dupes by
exact Key and **teleports its own copy to the host position**
(`remote_prop_spawn.cpp:252-255`, `SetActorLocation`/`SetActorRotation`,
`bTeleport=true`). For a SIMULATING body that **wakes the rigid body**. The host
loaded the same props **asleep from disk** (why it's 116 FPS); the client wakes
them and they **never re-sleep** → permanent physics-scene cost.

Measured divergent set (hands-on run, 18:57): **1388 props** diverged, mean
**1017 cm (10 m)**, max **20550 cm (205 m)** — mostly simulating
(`prop_C` 451, `trashBitsPile_C` 81, containers, food, barrels). Count is
**luck-of-the-RNG per New Game** (768 → 20 FPS, 1388 → 5 FPS). Teleporting ~1388
bodies an average of 10 m into the client's divergent layout = physics chaos.

---

## WHAT'S ALREADY SHIPPED (the PARTIAL fix — keep it)

**Epsilon-gate** in `remote_prop_spawn.cpp` exact-key de-dupe branch
(this session, build `6EDA8AF893AB`): before the teleport, read the existing
actor's current transform; if within **2 cm / 1 deg** of the host target, **skip
the write** (no wake). Eliminated the dominant storm — the ~2116 `d=0.00 cm`
identical props that were being needlessly re-woken every connect. This is
RULE-1-correct and **load-bearing — keep it.** It is NECESSARY but NOT
SUFFICIENT: it never touches the genuinely-divergent props (768-1388), which
still wake on the teleport branch. THAT is what Phase 1 below fixes.

(`<cmath>` include added; gate logs "already aligned, skipping teleport" vs
"diverged (d=Ncm), converging".)

---

## THE FIX — `kAtRest`: carry the host's rest-state on the wire

The wake's root cause is **the wire omitting the host's rest observation**, so
the client re-derives rest by settling (and never does). Carry it.

**(a) Positions** stay host-authoritative (stream + converge) — we do NOT
seed-sync `rand()` (impossible: shared stream, divergent call counts, no asset
edits). MTA shape: `CMapManager::SendMapInformation` on join streams
authoritative element positions; `CEntityAddPacket` writes `GetPosition()`;
client never independently world-gens dynamic objects.

**(b) No wake:** host stamps a `kAtRest` physFlag when the prop's body is asleep;
client, immediately after the teleport, calls `PutRigidBodyToSleep(NAME_None)`
when `kAtRest` is set → body lands at authority and goes straight back to sleep.

**(c) Still grabbable:** `PutRigidBodyToSleep` leaves the body a **dynamic**
PhysX body (sleeping-dynamic still grabs/collides/wakes-on-touch) — it does NOT
call `SetSimulatePhysics(false)` (that's the kinematic grab-drive path, scoped to
held props only). MTA analogue: `CClientObject::SetFrozen` = rest without
de-simulating. **This resolves the earlier "freeze breaks grab" worry — verified.**

**(d) RULE compliance:** RULE 1 point-fix at the exact teleport call site (not a
broad suppress-wakes filter). RULE 3 all via reflection
(`IsAnyRigidBodyAwake` / `PutRigidBodyToSleep` ProcessEvent dispatch — confirmed
callable, already used in `autotest_ragdoll_spawn_probe.cpp:134`,
`autotest_vitals.cpp:90`). No asset edits.

### LOAD-BEARING ASSUMPTION (verify FIRST, no guessing)
The whole fix rests on: **the 1388 divergent props are actually ASLEEP on the
host at snapshot time** (`IsAnyRigidBodyAwake == false`). They should be (disk
load = asleep → host 116 FPS). **Bake a diagnostic into `DrainChunk`** that logs
`IsBodyAsleep` for the divergent-keyed props; the first hands-on run must show
`kAtRest` stamped for the BULK of the 1388. If they read awake on the host,
kAtRest is a no-op and the root cause is elsewhere — STOP and revisit.

---

## IMPLEMENTATION BLUEPRINT

### PHASE 1 — kAtRest (the measured fix). SHIP ALONE, hands-on-verify, THEN Phase 2. (USER-APPROVED sequencing)
1. `include/coop/net/protocol.h`: add `inline constexpr uint8_t kAtRest = 0x08;`
   to `propspawn_flags` (slot free — `// 0x08 (was kFreshLanded) RETIRED v52`).
   Update comment; **bump protocol version** (bit 0x08 semantics change).
   PropSpawnPayload stays 168 B (physFlags is an existing byte).
2. `include/ue_wrap/prop.h` + `src/ue_wrap/prop.cpp`:
   - `bool IsBodyAsleep(void* actor)` — resolve `GetStaticMesh`, fallback
     `K2_GetRootComponent`; dispatch `IsAnyRigidBodyAwake`; return `!awake`;
     conservative `false` (= treat as awake, don't stamp) on any resolution fail.
   - `void PutBodyToSleep(void* mesh)` — dispatch `PutRigidBodyToSleep` with
     `BoneName=NAME_None`, cached UFunction; gate on non-null + physics-enabled.
3. `src/coop/prop_snapshot.cpp` DrainChunk (~line 283, after existing physFlags
   stamps): `if (ue_wrap::prop::IsBodyAsleep(obj) && !coop::remote_prop::IsActorUnderAnyDrive(obj)) p.physFlags |= kAtRest;`
   — the `IsActorUnderAnyDrive` guard prevents mis-stamping a host-HELD prop
   (reads kinematic-asleep). **Add the diagnostic log here** (count stamped).
4. `src/coop/remote_prop_spawn.cpp`:
   - **Exact-key diverged branch (after line 255): THE critical graft** —
     `if (payload.physFlags & propspawn_flags::kAtRest) ue_wrap::prop::PutBodyToSleep(<mesh/root of existing>);`
   - **Path C fresh-spawn (after `DriveSimulate(mesh, true)`):** same conditional sleep.
   - **Fuzzy path (~lines 303-306):** add epsilon-gate parity (cheap regression
     guard) + the kAtRest sleep; keep rekey/`RestoreCollisionIfNeeded`/
     `RegisterPropMirror` UNCONDITIONAL (verified invariant).

### PHASE 2 — spawner-suppression endgame (SEPARATE commit, only after Phase 1 verified)
Targets a DIFFERENT population: divergent-**NewGuid-key** mushrooms that hit the
FUZZY path (not the exact-key wake). Orthogonal hygiene; carries unverified-
UFunction-name risk → do NOT bundle with Phase 1.
5. `include/ue_wrap/game_thread.h`: bump `kMaxInterceptors` 16→24 (already
   mandated by the wind/firefly audit note I-1; table ~15/16).
6. **bp_reflect probe FIRST** to confirm exact UFunction names on
   `AmushroomSpawner_C` / `AmushroomMaster_C` (do not guess).
7. `src/coop/garbage_sync.cpp`: +2 `MAKE_SPAWNER_CANCEL` callbacks + targets[]
   entries; bump the all-or-nothing latch 4→6.
8. After Phase 2 hands-on-confirms mushrooms mirror: **DELETE (RULE 2)** the fuzzy
   block (`remote_prop_spawn.cpp` ~270-360), `IsCollisionRestoreClass`/
   `RestoreCollisionIfNeeded` + 3 call sites, `FindNearbySameClass` (grep other
   callers first), `PropFoodMushroomClass`. Net-negative LOC.

### File-size: `remote_prop_spawn.cpp` = 564 LOC now; Phase 1 adds ~10, Phase 2
deletes ~140. Re-check `wc -l` per commit; flag for extraction only if a commit
crosses 800 before the Phase 2 deletions.

### Verify (pre-deploy checklist): build clean → hot-path table (Phase 1 adds ONE
`IsBodyAsleep` dispatch per snapshot candidate = one-shot connect burst;
`PutBodyToSleep` one-shot per divergent prop; ZERO steady-state) → deploy 4
folders → 30s+ LAN smoke → **DECISIVE hands-on: client connects to s_1234 host,
measure steady-state client FPS (target >100, matching host disk-asleep load),
confirm no permanent ~170 ms physics spike.** Idle autonomous smoke is NOT
sufficient — the wake is connect-triggered + RNG-divergence-dependent. Log diff:
confirm "diverged → converging" lines followed by sleep application + kAtRest
stamp count for the bulk of divergent props.

### Open decisions (workflow #2/#3 — recommended defaults, proceeding unless user objects)
- Stale-rest race (a prop asleep at enumerate woken before DrainChunk sends):
  **accept the brief cosmetic skew** (self-corrects on first touch) — recommended.
- Full rest-mirror is the GOAL; the one-time transient settle is only an
  acceptable DEGRADED fallback if `IsBodyAsleep` proves unreliable on some class.

### Secondary cost note (post-physics-fix)
At 5 FPS our `interactable` poll (~70 ms/s) and a `ReceiveTick` observer (~58 ms/s
spike) were elevated — they scale with mirrored-interactable / ticking-prop count,
NOT physics. The physics fix (170 ms) is ~85% of the deficit; if these remain
significant AFTER kAtRest recovers FPS, address separately (verify, don't guess).

---

## CLUMP STATUS (the user's other open item — BLOCKED by the FPS bug)

User report: "the pile a peer grabs doesn't get destroyed." **The sender-side
grab-destroy logic IS firing correctly** (client log 18:57 proof):
```
CONVERT ball eid=40961 -> pile eid=40962
watching pile eid=40962 ... for re-grab destroy
watched pile eid=40962 grabbed locally (died near camera) -> PropDestroy (drop peer mirror)
```
Works for BOTH convert-born piles (peer-range eid 40962) AND pre-existing keyed
piles (host-range eid 4462). So the failure is **receiver-side** (the OTHER peer
applying the incoming `PropDestroy`) **or just 5-FPS degradation** (stalled/
mistimed packets, proximity death-watch misfire). **Cannot reliably test clump
at 5 FPS.** → Fix FPS (Phase 1) FIRST, then re-test clump clean. If it still
fails on a stable build, likely the eid-range acceptance when the HOST applies a
client's host-range-eid `PropDestroy` (check the v52 "relaxed to either range"
actually shipped + works on the receive path in `event_feed.cpp`).

All v52 clump fixes (morphing/eid-deque/PropConvert/watch-every-chipPile) ARE in
the deployed `6EDA8AF893AB` build.

---

## ENV / INFRA STATE
- `perf_probe=1` + `perf_probe_selftime=1` in host (`Game_0.9.0n`) + client
  (`Game_0.9.0n_copy`) inis. **Turn selftime OFF before shipping** (adds per-
  dispatch counting). copy2 has no perf_probe lines.
- Launcher (`tools/mp.py`): host `VOTVCOOP_SAVE=s_1234`, clients `VOTVCOOP_FRESH=1`.
- Deployed build `6EDA8AF893AB` = epsilon-gate (NOT kAtRest yet). All UNCOMMITTED.

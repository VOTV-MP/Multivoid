# Baritone analysis + a VOTV autonomous bot-director — DESIGN (2026-07-23)

**Status: DESIGN, not built.** Deliverable of a 12-round `/qf` design pass (thread:
`scratchpad/qf_thread.md`) + 4 parallel fact-gatherer maps of `reference/baritone/`. Nothing here is
built; every load-bearing inference is tagged and gated behind a HALT probe. Read the
measured/inferred/open ledger (§6) before trusting any capability claim.

**What the user asked for (verbatim, with a mid-thread reframe):**
- Original: "полный разбор баритона … документация полная того что нам надо, чтобы создать аналог для
  автономного тестирования."
- Reframe (mid-thread, material): the end goal is NOT a "test analog" but an **autonomous bot-DIRECTOR**
  — "чтобы ты мог срежиссировать любой сценарий". Concrete example: "два пира подходят к контейнеру,
  входят и ОДНОВРЕМЕННО берут предмет X" — which is literally the `CONFLICT>0` / STACK-RACE recorded in
  today's bug ledger, a concurrent container take **unreproducible without two live humans**.
- Steer: "слоеность правильно делаем" — respect principle-7 (`ue_wrap` vs `coop`) + folder-per-domain +
  the 800/1500 LOC caps + one-feature-per-file, and RULE 3 (dev tools do not ship in the gameplay path).

So this doc is two things: **Part A** a full architectural analysis of Baritone (what ports, what does
not), and **Part B** the design of a VOTV analog — an autonomous director Claude can script to stage ANY
scenario, above all the concurrent multi-peer races a human pair cannot reliably produce.

---

## 0. TL;DR

Baritone is a goal-driven autonomous Minecraft player: **Command → Process (brain) → Goal → Pathfinder
(voxel A*) → Movement → synthetic input**, ticked by an event bus, attached by mixins. Its **bulk is
voxel-specific** (A*, block-offset moves, break/place-to-traverse, 2-bit chunk cache) and does NOT port —
because **VOTV ships a baked NavMesh** the engine pathfinds on for free. What DOES port is the
**architecture**: the goal abstraction, the process/arbitration brain, the executor state machine, the
input-override + player-context seams, and the command channel.

The VOTV analog is therefore **much smaller than Baritone**. It is an autonomous **director**: each peer's
bot drives its own possessed player over the NavMesh, interacts at the human-input seam, and an
orchestrator stages cross-agent rendezvous — verified by the **log-truth** layer we already have and
Baritone lacks. Build **bottom-up**: a single-agent walked grab first (only NAV is unproven; input +
verifier are already proven by `autotest_chippile`), the concurrent races second, and the general
interpreter abstracted only at N=3 real scenarios.

---

# PART A — Baritone, analysed

Source: `reference/baritone/` (355 Java files; `api/` = contracts, `main/` = impl, `launch/mixins/` = the
Minecraft attach layer). Every component tagged **[ARCH]** (engine-agnostic; ports to UE4) or **[VOXEL]/[MC]**
(intrinsically Minecraft; replaced by an engine equivalent).

## A1. The brain — process arbitration `[ARCH, ports verbatim]`

- **`IBaritoneProcess`** (`api/process/IBaritoneProcess.java`): a decision unit competing for exclusive
  control of pathing. Methods: `isActive()`, `onTick(calcFailed, isSafeToCancel) -> PathingCommand`,
  `isTemporary()`, `priority()`, `onLostControl()`, `displayName()`.
- **`PathingCommand`** = `(Goal, PathingCommandType)`; the 7-verb command enum: `SET_GOAL_AND_PATH`,
  `REQUEST_PAUSE`, `CANCEL_AND_SET_GOAL`, `REVALIDATE_GOAL_AND_PATH`, `FORCE_REVALIDATE_GOAL_AND_PATH`,
  `DEFER`, `SET_GOAL_AND_PAUSE`.
- **`PathingControlManager`** (`utils/PathingControlManager.java`) — the priority-stack scheduler (the
  single most portable class): active processes front-inserted by recency, sorted by `priority()` desc,
  first non-`DEFER` wins; a non-temporary winner evicts lower processes; a temporary winner overlays
  (pause interrupts). Two-phase `preTick`/`postTick`.
- **Behavior vs Process split**: a `Behavior` is an always-on event listener; a `Process` competes for
  exclusive pathing control. `PathingBehavior` owns the async calc thread + current/next executor;
  `LookBehavior` owns aim, decoupled from movement.
- **Tick loop**: poll processes → sort by priority → first non-`DEFER` → apply the command → advance the
  executor. Only two things are MC-coupled: the tick SOURCE (one mixin) and each process's goal
  PRODUCTION + actuation.

## A2. The pathfinder — voxel A* `[VOXEL, replaced by UE NavMesh]`

- A* (`AStarPathFinder`, `AbstractNodeCostSearch`, `PathNode`, `BinaryHeapOpenSet`) with **coefficient
  backoff** (anytime fallback), **segmented calculation + splicing** (compute the next segment while
  executing the current), **incremental-cost backoff**.
- **Successor function** = the 26 hardcoded block-offset `Moves` (traverse/ascend/descend/diagonal/
  pillar/parkour/fall). **Cost oracle** = `MovementHelper` (block passability/breakability, MC physics
  ticks in `ActionCosts`). These are the ONLY two engine-specific injection points; the graph search
  around them is `[ARCH]`.
- **World model** = a 2-bit AIR/SOLID/WATER/AVOID chunk cache (`CachedChunk`/`CachedRegion`/`ChunkPacker`)
  so it pathfinds beyond loaded chunks. Entirely `[VOXEL]`.
- **PathExecutor** (`path/PathExecutor.java`) `[ARCH state machine]`: per-tick advance-on-SUCCESS,
  off-path resync, stuck/timeout detection, live cost re-verification, cancel-and-replan. **This part we
  keep** (thinned) — see B3.

## A3. The seams — input & read-model `[ARCH, the two we most need]`

- **Input override**: a `Map<Input,bool>` of forced virtual inputs, applied by **swapping the player's
  input object** (`PlayerMovementInput`) so the engine's OWN per-tick movement code consumes them
  (`forwardImpulse/leftImpulse/jumping/sneak`). **It never calls move() directly** — the pawn stays
  engine-owned; only its input SOURCE is swapped. UE4 analog: push the same axis/action values into the
  pawn's movement input each tick.
- **Player-context read-model** (`IPlayerContext`): position/feet/head/motion/rotation + world + entity
  iteration, backed by a thin adapter over the live game objects. UE4 analog: reflected pawn properties +
  world-actor enumeration (we already have this).
- **Waypoint store** (`WaypointCollection`) `[ARCH]`: tagged named-location store — directly portable,
  swap int-xyz for `FVector`.
- **Multi-instance registry** (`IBaritoneProvider`): one `IBaritone` per player. UE4 analog: one bot per
  driven body; drive BOTH peers.

## A4. The attach layer — 18 mixins → 13 engine-agnostic integration points

The mixins do only two things: **emit an event** into the bus, or **expose/override an engine field**.
Distilled, a bot needs exactly these seams from any host game (UE4 does each via a ProcessEvent hook or
reflected access). **We already own 10 of 13:**

| # | Integration point | Have it? |
|---|---|---|
| 1 | Tick callback (pre/post) | YES — game-thread pump, per-frame |
| 2 | World load/unload event | YES — `StartCoopSession` |
| 3 | World-read snapshot | YES — `GUObjectArray` + reflection |
| 4 | **Movement-input override** | **NO** — we inject E-press, not walking |
| 5 | Rotation override | PARTIAL — `lookAtActor`; `SetControlRotation` exists |
| 6 | Input suppression | N/A for autonomous (no human at the box) |
| 7 | **Command/task channel** | PARTIAL — scenario.txt/env/ini, not tasks |
| 8 | Autocomplete | N/A |
| 9 | Packet tap | YES — the whole net layer |
| 10 | Discrete game events | YES — ProcessEvent observers |
| 11 | Private-state accessors | YES — reflection |
| 12 | Per-object bot state | YES — the eid registry side-table |
| 13 | Server-dependency bypass | N/A |

The gaps that the analog must build: **(4) navigation/walking, (7) a task/command channel**, and the
**brain/director** itself (today's autotest is bespoke C++ per feature).

---

# PART B — The VOTV analog: an autonomous bot-director

## B1. Anchor & shape

An autonomous **director**. Claude composes ANY scenario as a **data script** over a command channel; the
director interprets it into per-agent task queues + cross-agent rendezvous. The killer class = concurrent
multi-peer races (the container `CONFLICT>0`, the world-grab pose-contention) that two humans cannot
reliably stage. The **target architecture is a Baritone-analog**; the **build is bottom-up** (§B7).

**Why the analog is small.** VOTV ships a baked NavMesh its NPCs pathfind on (see §6 measured). So
Baritone's entire pathfinder + movement + world-cache layer (its `[VOXEL]` bulk) collapses to an engine
call. We port the architecture (brain/goal/executor/seams/command) and let the engine navigate.

## B2. Body & navigation

- **Body = the local POSSESSED `mainPlayer_C`** (`GetController() != nullptr`). It has a `PlayerController`;
  `AddMovementInput` feeds its `CharacterMovementComponent` — the measured self-move mechanism. In a
  two-peer test each peer drives its OWN possessed body; the puppets stay as remote mirrors (pose stream),
  driven by no bot. **The puppet / AIController path is DROPPED**: it re-enables exactly what
  `engine.cpp:257-270` deliberately zeros (`AutoPossessPlayer/AutoPossessAI`, `AIControllerClass=null`),
  and the puppet tick does not feed the pose stream (measured 2026-06-22 grab-feasibility).
- **Navigation is NOT AIController `MoveTo`.** `MoveToLocation`/`MoveToActor` (AIModule) need an
  AIController (puppet/NPC only). Instead:
  - **Path** = `UNavigationSystemV1::FindPathToLocationSynchronously` / `FindPathToActorSynchronously`
    (controller-agnostic; returns a `UNavigationPath` of points; uses the baked NavMesh).
  - **Steer** = per-tick `AddMovementInput(direction toward the next path point)` on the possessed body.
  This IS Baritone's model (compute path, feed movement input). It keeps a thin `PathExecutor` (§B3).

## B3. The thin PathExecutor (kept from Baritone)

Resolve the pawn ONCE at task start, hold it by INDEX + `IsLiveByIndex` (no per-frame `GUObjectArray`
walk — the post-ship perf-audit rule). Per tick: cheap reflected location read → direction to next path
point → `AddMovementInput`. Path computed once per `goto`, cached; re-`FindPath` only on off-path.

- **STOP** = `ARRIVED` (`distance < grabRange`, using the interaction verb's OWN reach, not a nav
  constant). **ADVANCE** = next-point < threshold.
- **FAILURE paired with arrival** (never hang): a **deadline** (max ticks/seconds → `FAILED_TO_REACH`) +
  **stuck detection** (distance-to-goal not decreasing over N ticks — Baritone's `ticksAway`). FAILED →
  void the scenario.
- **SETTLE gate**: after `ARRIVED`, stop input and wait `velocity < threshold` (bounded by the deadline)
  **before** acting — reproducing `chippile`'s proven STATIONARY grab condition, so residual braking
  momentum can't turn a nav success into an ambiguous grab-miss.

## B4. Interaction — drive at the human-input seam ONLY

**The spine principle (the design's core invariant):** the director drives ONLY at the human-INPUT/intent
seam (`AddMovementInput`, `InpActEvt_use` E-press, forced `lookAtActor` aim), **NEVER the downstream
effect/state seam** (never call `takeObj`/mutators directly). Consequences:

- Every scenario runs the REAL detection → broadcast → authority wire path. The bot is
  **authority-equivalent to a human client** — a client-bot pressing E on host-authored state produces an
  INTENT the host validates, exactly as a human client would. No test artifacts, no accidental
  host-state-from-client. This is what makes a staged race a VALID repro, not a fake.
- `take`/`grab`/`press`/`tune` are **sugar over "aim + input-verb"** composition, NOT direct effect calls.

**`invoke(agent, actor, reflectedVerb, args)`** is the one generic interaction primitive — because our
coop layer already drives the game by reflected UFunction calls. It resolves the verb on the verb's
**DECLARING class** (from the CXX header-dump ownership), because `FindFunction` does NOT walk the
superclass chain (`Outer == class` exactly, `reflection.cpp:427`) — resolving a base-declared verb on a
derived leaf class returns nullptr = a silent permanent no-op
(`[[lesson-findfunction-does-not-walk-the-superclass-chain]]`).

**Structurally OUT OF SCOPE (named honestly, not "any verb"):** `EX_Local*`-only / BP-inlined verbs (they
have no callable UFunction entry — observe-not-drive, per `docs/COOP_DISPATCH_VISIBILITY.md`); the
superclass-nullptr trap (guarded by declaring-class resolution); and continuous/analog/held/mouse-drag
inputs (hold-to-charge, desk-cursor drag) — the discrete `CallFunction` model injects discrete events, not
held analog input.

## B5. The barrier — orchestrator-level, not a wire message

Cross-agent rendezvous lives at the **orchestrator (`mp.py`, which already owns both processes)** — NOT a
game `ReliableKind`. So there is **no protocol bump** and the dev/gameplay layering stays clean.

- Sequence: `goto → ARRIVED(logged token) → barrier release → act`. Each bot logs `ARRIVED eid=X` when its
  read-model `distance < interactionRange`; the orchestrator releases when all named agents have ARRIVED.
- **Two-level bounding**: the per-agent loop voids on deadline/stuck; the **barrier has its OWN deadline**
  → the orchestrator aborts (void) on "any FAILED / barrier-deadline elapsed", never hanging on a voided
  agent.
- **Simultaneity**: on the same box (our standard LAN test, both peers on one machine) the clock is
  shared → a future-timestamp GO gives real sub-ms simultaneity. Cross-box is bounded by clock skew (fine
  for the 1.5 s container CAS window; NTP for tighter windows).
- **A race is OBSERVED, never faked.** The barrier maximises the probability both actions land in the
  contested window; whether it becomes a real conflict is decided by the host's arbitration and confirmed
  by the log (repeat-until, `[[feedback-probe-must-count-not-confirm]]`). The barrier buys **autonomy /
  reproducibility** (no two humans, runs in CI) and **reach of tight windows** (the container
  "host-in-flight" branch is ~one broadcast tick, tens of ms; the world-grab pose-contention needs real
  simultaneity) — not a manufactured conflict.

## B6. Verification — reuse the log-truth layer (our strength)

The bot DRIVES; the existing **log-truth** assertion layer (`pile-test-assert.ps1` pattern, 13 invariants
over host+client logs) VERIFIES. Baritone has NO verifier — this is our edge. **Each scenario ships its
own invariant set** (there is no universal verifier):

- **Walked grab (flagship)**: reuse `pile-test-assert.ps1` (already proven).
- **Container STACK-RACE**: `container_contents: CONFLICT eid=N slot U … Total refused: K`
  (`container_contents_sync.cpp:577`) discriminates the refused peer; `applied N records` (`:547`, no slot
  — add slot cheaply, or cross-ref) the accepted; client `shipped records` (`:373`) the positive control.
  **3-way discrimination** (per `[[lesson-e2e-assert-must-discriminate-the-axis]]`): both-shipped(eid) +
  `CONFLICT>0` = race WON; both-shipped + `CONFLICT=0` = staged, CAS held / window loose;
  not-both-shipped = never staged / VOID. Without the positive control, `CONFLICT=0` is ambiguous
  ("CAS held" vs "one bot late").
- **World-grab race**: the grab has **no arbiter** (grab is a per-slot `PropPose` tug-of-war, no claim
  table) → the verifier is a **pose-contention probe** (two slots streaming `PropPose` for one eid), which
  needs its own instrument — an OPEN milestone-2 item.

## B7. Build plan — bottom-up, abstraction earned at N=3

Do NOT build the general interpreter + folders around zero proven scenarios (premature abstraction,
§11 fix-then-generalize). Build up:

**Phase 0 — the HALT probes (must-measure-before-build, two independent gates):**
- **Gate A**: `FindPath` RETURNS a traversable path — assert `path-points > 1` between two *known-reachable*
  points; discriminate empty-path (no route) from call-failed (`[[feedback-probe-must-count-not-confirm]]`,
  `[[lesson-read-cooked-umap-and-bytecode-before-concluding-live-probe]]`). This settles whether the baked
  navmesh is BUILT (actors are authored — measured; built+traversable — this gate).
- **Gate B**: reflected `AddMovementInput` actually moves the possessed body (dispatch-visibility).
- A & B are **necessary-not-sufficient**: they gate the ATTEMPT. Closed-loop convergence (steer → arrive)
  is proven ONLY by the flagship RUN, which fails safe (deadline + stuck → void).
- **Fallback ladder** by which gate fails: A&B → **rung0** real walk; A-only → **rung1** swept
  `SetActorLocation` along the real `FindPath` (real path traversal, feeds the pose stream, bypasses
  movement physics); A-fail → **rung2** standoff-teleport (= today's `chippile`, P0 total failure — NOT a
  flagship). **Rung1 crutch criterion (decidable):** rung1 is legitimate IFF the scenario's invariant-set
  contains NO movement/pose/velocity invariant; a movement-SYNC scenario forbids rung1 and makes gate B a
  hard gate.

**Phase 1 — the day-1 flagship: a single-agent WALKED GRAB.** = `autotest_chippile`'s proven
grab+carry+throw+`pile-test-assert`, but the bot WALKS to the prop instead of teleporting to a standoff.
Sequence: `FindPath → per-tick AddMovementInput to grabRange → ARRIVED → stop → SETTLE → force lookAtActor
(proven) → InpActEvt_use (proven)`. **The only genuinely new thing is nav**; input-seam invoke and the
verifier are already proven by chippile (proto v85, PASS 2026-06-23). This proves the whole spine end-to-end.

**Phase 2 — the races (each with its own verifier):**
- **World-grab race** (two peers grab one world prop): input is in-scope (chippile-proven); needs the
  pose-contention verifier probe. This is the provable-now concurrent race.
- **Container STACK-RACE** (the user's named example): **probe-gated** — `takeObj` is
  `propInventory_C::takeObj` behind `EX_LocalVirtualFunction` dispatch (`takeObj POST` fired 0× in take-4;
  PE-visibility uncertain) and the human take is likely a **widget-slot** interaction (open container UI →
  take), which sits in the OUT-OF-SCOPE bucket. **Milestone-2 probe**: does the container take reduce to
  an in-scope input verb, or a widget interaction (needing a scoped UMG-click extension) + is `takeObj`
  driveable? The verifier (`CONFLICT` line) is already measured; the INPUT is the gate.

**Phase 3 — abstraction at N=3.** With three real scenarios exercising them, extract the primitives —
`{goto, barrier, wait, assert}` + generic `invoke` — and the interpreter + `ue_wrap/nav` + `ue_wrap/input`
+ `coop/dev/director` folders. The command channel (a polled task file the director reads) replaces
bespoke per-feature C++ scenarios: Claude writes a DATA script; the director executes it.

## B8. Layering (RULE 3 + "слоеность")

- **`ue_wrap/nav`** — `FindPath` wrap, `UNavigationPath` reads, `ProjectPointToNavigation`. Engine
  substrate.
- **`ue_wrap/input`** — `AddMovementInput`, control-rotation / `lookAtActor` aim. Engine substrate.
- **`coop/dev/director`** — the interpreter, task queue, primitives, per-agent executor. **Dev-only**,
  `ini [dev]` / env gated, compiled but NOT in the shipping gameplay path (precedent: `coop/dev/` holds
  freecam / restore_vitals / probes). It must NOT live in `coop/` gameplay.
- **`mp.py` (orchestrator)** — the cross-agent barrier + the command/task channel + launching both peers.
- Each scenario ships its own invariant set (no universal verifier). one folder = one concept; 800/1500
  LOC; one feature per file.
- **Note (RULE 3 & principle 8):** the director is a dev tool that *exercises* mid-activity-join and
  concurrent-race behaviour (principle 8) — it is not itself a shipping lane subject to those rules.

---

## 6. Measured / inferred-P0-gated / open (the honest ledger)

**[measured] (this session):**
- Baked NavMesh ACTORS are EXPORTS in the cooked level umaps (`research/bp_reflection/_map_*.json`):
  `RecastNavMesh`×49, `NavMeshBoundsVolume`×42, `NavModifierVolume`×136, `NavigationSystemV1`×14 across
  7+ maps. NPCs (`npc_zombie`/`npc_orborb`/`p_kerfus`) carry `AIMoveTo` + `GetRandomReachablePointInRadius`
  verbs. SDK has `NavigationSystem.hpp` (`FindPathToLocationSynchronously`/`FindPathToActorSynchronously`/
  `UNavigationPath`/`ProjectPointToNavigation`) + `AIModule.hpp` + `nav_door`/`nav_heavyProp`.
- `mainPlayer_C` = NavAgent with `AddMovementInput`×7 + `CharacterMovement`×82 + `GetController`×6,
  `AIMoveTo`=0 (a possessed Character that self-moves via AddMovementInput, not AI).
- `engine.cpp:257-270` zeros `AutoPossessPlayer/AutoPossessAI` + `AIControllerClass=null` for the puppet.
- `autotest_chippile` drives the real `InpActEvt_use` + forced `lookAtActor` + `playerGrabbed` through the
  REAL wire path, verified by `pile-test-assert.ps1` (proto v85, PASS 2026-06-23) — the input-seam invoke
  is proven for grab.
- `container_contents: CONFLICT eid=%u slot %u …` exists (`container_contents_sync.cpp:577`); the accept
  line (`:547`) lacks a slot.
- `takeObj` is `propInventory_C::takeObj` behind `EX_LocalVirtualFunction` dispatch; `takeObj POST` = 0 in
  take-4 (PE-visibility uncertain). The world-prop grab has NO arbiter.

**[measured, Phase-0 HALT probe RAN 2026-07-23 — `harness/autotest_navprobe.cpp`, solo, DLL
`multivoid-0.9.0n-125`, host log `nav_probe: VERDICT`]:**
- **Gate A PASS** — `FindPathToLocationSynchronously` (static, on the `NavigationSystemV1` CDO) returned a
  valid traversable path (`IsValid=1 points=2 len=583cm`) to a `K2_GetRandomReachablePointInRadius`-chosen
  endpoint (`reachOk=1`). So the baked NavMesh is **BUILT + queryable + FindPath works** at runtime — the
  navmesh-built inference is now measured, not inferred.
- **Gate B PASS** — reflected `AddMovementInput` (resolved on the DECLARING class `Pawn`, not the leaf —
  the findfunction-superclass trap held) MOVES the possessed body: `dir(-1,0)=527cm` ≈ a clean ~4 m/s walk
  over ~1.3 s. (`dir(+1,0)=78696cm` is a start-on-a-high-ledge artifact — Z=6420, +X fell into a
  kill-volume respawn; the player was restored to the exact start afterward, and the outlier argues FOR
  FindPath-guided steering, which stays on-mesh, over naive directional push.)
- **VERDICT: both gates PASS → rung0 (real walk) → the Phase-1 walked-grab flagship is UNBLOCKED.**

**[inferred, still-gated]:**
- `invoke` generalizes beyond grab (bounded by a callable-vs-observe-only UFunction census; N=3 gate).
- Closed-loop convergence (steer → arrive within grabRange) — proven only by the Phase-1 flagship RUN
  (gates A&B are necessary-not-sufficient; they gated the ATTEMPT, which is now green).

**[open, milestone-gated]:** the container-take input mechanism (in-scope verb vs widget); the world-grab
pose-contention verifier; cross-box clock skew for tight windows; the callable-UFunction census;
walked-lands-in-range reliability (checkable via read-model distance).

---

## 6b. 2026-07-23 build log — Phase-0 PASS, then a USER-DIRECTED brain-first pivot

This supersedes the bottom-up sequencing in §B7. Recorded here so a later session reads it as
INTENT, not drift.

- **Phase-0 probe RAN + PASSED** (both gates) — see §6. Verdict rung0. BUT the first walked-grab
  attempt (a procedural monolith) FROZE: the save's player was holding a disc in R-hold placement
  mode, and `AddMovementInput` cannot walk a hand-locked character (measured: moved/tick ≈ 0). This
  also showed Gate B's aggregate position-delta conflated *walking* with *any displacement* (a
  kill-volume respawn) — the per-tick `moved` is the honest signal; **Gate B's capability claim needs
  re-validating on a walkable player** (a controlled sustained walk, not an aggregate).
- **The root was architectural, not a save bug** (user, 2026-07-23): a procedural script has no
  read-model, so it can't know its hand is full. The fix is the BRAIN — `PlayerContext` (IPlayerContext
  analog) + priority-arbitrated processes (`ClearHand ⊳ Goto ⊳ Grab`, IBaritoneProcess analog) +
  `ControlManager` tick loop. The held disc is then a high-priority `ClearHandProcess` that preempts
  `Goto` until the hand clears — state-awareness, structural, not a bolted-on step.
- **Sequencing deviation, stated honestly:** §B7 said abstract the brain at **N=3 scenarios RUN**. As of
  this note there are **0 director runs, 1 scenario** — so N=3 is NOT met. Building the brain now is a
  **user-directed** deviation, justified by (a) the primitives being de-risked by the Phase-0 probe and
  (b) the held-disc failure proving monolith-patching is the wrong shape. "Primitives de-risked" is a
  DIFFERENT criterion than N=3; the deviation is a deliberate call, not a claim that N=3 was satisfied.
- **Status: the brain SKELETON is built, layered `ue_wrap/engine/engine_nav` + `coop/dev/director/`
  (director.h, player_context, control_manager, proc_walkgrab, director_run). It has the FORM (read-model,
  emergent activation, thin executor, input-override) but is NOT yet proven by a green run.** The monolith
  was retired (RULE 2). Open honesty items carried in the code: ClearHand MEASURES whether the input-seam
  `InpActEvt_drop` works and only falls back to the effect-seam release with a LABEL + a verdict flag
  (`drop-input-seam-faithful=0/1`) — because the reflection input stub may not run the drop body (the
  chippile `InpActEvt_use`-body-inert precedent); if inert, that is a substrate HALT-fact about
  input-ACTION verbs, recorded, not masked.

## 7. Baritone → VOTV port table (what maps, what's replaced)

| Baritone | Tag | VOTV analog |
|---|---|---|
| `IBaritoneProcess` / `PathingControlManager` / `PathingCommand` | [ARCH] | the director's task/arbitration skeleton (thinned — see §B1 note) |
| Behavior/Process split; event bus | [ARCH] | our tick-pump listeners |
| `Goal` predicate+heuristic | [ARCH] | reach-FVector / reach-actor / within-range target |
| A* + `Moves` + `Movement` + `ActionCosts` + chunk cache + `MovementHelper` | [VOXEL] | **DELETED** — `FindPathToLocationSynchronously` over the baked NavMesh |
| `PathExecutor` state machine | [ARCH] | the thin steering executor (§B3): advance / off-path / stuck-timeout / settle |
| Input override (swap input source) | [ARCH] | `ue_wrap/input`: per-tick `AddMovementInput` (never call move()) |
| `IPlayerContext` read-model | [ARCH] | our reflected pawn props + world-actor enumeration (have it) |
| `WaypointCollection` | [ARCH] | named test-location store (FVector-keyed) |
| Command system + `invoke` | [ARCH] | the data-script command channel + generic reflected-verb `invoke` |
| 18 mixins | [MC] | our ProcessEvent hooks + reflection (10/13 seams already owned) |
| Settings reflection | [ARCH] | our `ini [dev]` / env config |
| — (no verifier) | — | **our log-truth layer — the thing Baritone lacks** |

---

## 8. Provenance

Design converged over a 12-round `/qf` pass (thread `scratchpad/qf_thread.md`); the key measurement-driven
corrections: R1 reframe to a director + NavMesh measured + puppet Path B dropped; R2 barrier = orchestrator
(no proto bump) + positive-control + general-interpreter; R3 nav = `FindPath` + `AddMovementInput` (NOT
AIController MoveTo) measured; R4 `invoke` = callable-UFunctions only + the drive-at-input-seam principle;
R5 declaring-class resolution guard + resolve-pawn-once; R6 container take is `EX_Local`/widget → flagship
rescoped off it; R7 no race is end-to-end measured → flagship = single-agent walked grab, build bottom-up;
R8 body = possessed player, aim = forced (proven); R9 two-part HALT probe + bounded loop + rung1-crutch
criterion; R10 the RUN is the convergence measurement + SETTLE gate; R11 navmesh tag split
(actors-authored measured vs built inferred) + two-level barrier deadline; R12 "that holds".

**Next step (when the user green-lights building):** run the Phase-0 HALT probes (gate A: `FindPath`
returns a path; gate B: reflected `AddMovementInput` moves the possessed body). Their verdict picks the
fallback rung and unblocks the Phase-1 walked-grab flagship. Nothing is built until those two probes run.

# COOP_SYNC_DOCTRINE — how a system/element gets synced in Multivoid, distilled

> **NEW 2026-09-02.** The method below is not aspiration — it is the shape every lane that SURVIVED
> converged on, extracted so that any session (any model tier) can follow it without re-deriving
> four months of lessons. MTA (15+ years, `reference/mtasa-blue/`) is the precedent it leans on.
>
> This doc ORDERS the existing canon; it does not replace it. Deep truth lives in:
> `ARCHITECTURE.md` (the authority model and the intent rule), `COOP_DISPATCH_VISIBILITY.md` (will my hook
> fire), `COOP_ENTITY_EXPRESSION_MAP.md` (identity/expression/destroy), `COOP_WORLD_PROP_DIVERGENCE.md`
> (the brain-on trap), `join.md` (the join and its late-join rule), `CODE_MAP.md` (where lanes live),
> `STATUS.md` (per-facet status), CLAUDE.md (the rules this doc applies).

## The doctrine in one paragraph

Find the base the system rests on and build that first. RE the system until its verbs, writers and
state are censused facts, not guesses. Assign every element exactly one authority; a client never
writes shared-world state — it authors an INTENT (or takes an assigned lease) and the arbiter
performs or validates. Pick the interception seam per verb from the dispatch ladder — visibility is
a property of the DISPATCH PATH, not the function. Park the receiver's brain restorably; mirror
values only into parked brains. Give every entity its identity AT BIRTH and never mint a second row
for the same actor. Every lane ships with its mid-activity join answer, its suppression on the
PRODUCER side, and evidence from a real run. No crutches: a filter/skip/suppress bolted where the
symptom shows is a defect, not a fix.

## Step 0 — Foundation first (USER RULE 2026-09-02)

Before designing sync for X, ask: does X read a value whose own sync is absent/partial/wrong?
Test: would X need a hold/retry register to tolerate the base's divergence? If yes — STOP, park X
(keep its RE + design, name the dependency edge), build the base properly, then resume X on top.
Worked instance: the laptop PC power lane parked on the power-chain base.
`[[feedback-foundation-first-build-the-base-a-sync-rests-on]]`.

## Step 1 — RE until the facts are censused (never design on a skim)

- `python tools/bp_cpp.py <BP>` (whole-BP pseudo-C++; `--offsets` for the bytecode-offset listing —
  the citation currency), `tools/bp_cfg.py` for control flow, reflection dumps for layouts, IDA for
  native. Escalation ladder: reflection → IDA → UE4SS probes.
- The census you owe before design: every WRITER of the state (all of them, by grep + read, not the
  first hit), every VERB (player-facing entry points + their dispatch opcode), every READER that
  matters cross-peer, the actor's birth/death seams, and which pieces persist in the save.
- Facts derived with pre-2026-09 instruments must be re-based on the new ones when load-bearing
  (`[[feedback-rebase-old-tool-facts-on-new-instruments]]`).
- A claim is `[V]` only with the instrument named. `measured` vs `inferred` tags are mandatory in
  the design doc; the /qf critic attacks the difference.

## Step 2 — Authority: exactly one owner per element (the syncer model)

Decision table — pick the FIRST row that fits:

| The state is… | Owner | Shape |
|---|---|---|
| Shared-world progression (weather, power, events, world props that rot/dry/grow, NPC brains, RNG) | HOST | Host simulates/rolls; clients mirror. `COOP_RNG_AUTHORITY.md`. |
| A discrete, persistent, shared-world CHANGE a client initiates (buy, sell, destroy, place, equip from a container…) | HOST via **act-as-host intent** | Client suppresses its own producer, sends an intent naming WHAT (never what it costs), the arbiter validates and performs; results flow back as ordinary state. Reference lane: `order_sync` (proto 136). `ARCHITECTURE.md`, the authority model. |
| A continuously-simulated element one peer is INTERACTING with (held prop, driven vehicle, pressed device panel) | The interacting peer, by ASSIGNMENT (syncer/lease), never by assertion | Presser/holder authors the stream; the host arbiter validates inbound writes and may re-assign. On interaction end the authority RETURNS. Adopt lease hygiene: idle-expiry back to the declared owner, epoch-stamped grants, grant-names-final-revision fencing (MTA `CUnoccupiedVehicleSync`). |
| A peer's OWN body/pose/camera/voice | That peer | Sender-authored stream; receive side never gated (a discontinuity costs TRUST, never display). Bounds are CLIENT-scoped only — **the host may cheat and we relay it** (USER 2026-08-24). |
| Presentation-only local echo (UI, sounds, particles) | Nobody | Mirror on receive; never wire it back. |

Hard rules riding this step: the suppressed side is always the CLIENT-SIDE PRODUCER, never a
receive gate (a receive gate turns a cheat fix into a loss defect). An intent names WHAT, never
WHAT IT COSTS. If the arbiter cannot hook the trigger, ask whether the actor can point at an
ARTIFACT the arbiter can resolve (the order-row lesson) before declaring a lane blocked.

## Step 3 — Seam choice per verb (the dispatch ladder)

Read `COOP_DISPATCH_VISIBILITY.md` FIRST — visibility belongs to the dispatch path, not the
function. Then pick the cheapest seam that actually fires:

1. **ProcessEvent interceptor** (`RegisterInterceptor`, can CANCEL) — engine-originated calls:
   Tick/BeginPlay/input/delegates/timers/interface events. Fires only on PE dispatch.
2. **Func seam** (`ufunction_hook`, POST, native callees) — catches `EX_CallMath` /
   `EX_VirtualFunction` / `EX_FinalFunction` to NATIVE functions on every route (Func funnels
   them). Cannot cancel (a cancel would have to consume the caller's param stream — the death lane's
   OpenLevel lesson (`players.md`): detour the C++ function with MinHook instead when you must cancel a native).
3. **0x45 VM seam** (`vm_dispatch`, observe-only, call-site) — sees every `EX_LocalVirtualFunction`
   dispatch including calls made FROM ubergraphs; carries NO args, cannot cancel; consumers pair it
   with a Func seam or per-site reconcile for values.
4. **(reserved.)** A tier that could observe + cancel a SCRIPT function on every dispatch route
   -- the class VISIBILITY marks "invisible to both PE and Func" -- is an open design question, not
   a built seam. Until one exists, cancel of a BP-internal verb is achieved by per-site reconcile
   downstream, never by a receive gate.
5. **Per-site reconcile** (last resort): let the verb run, snapshot/diff observable state, converge
   to the authority's answer. This is also the fallback when a seam exists but the verb is
   ubergraph-resident.

Choose by CENSUS, not habit: `bp_cfg` the verb, read its dispatch opcode(s) at every call site,
and write the chosen seam + why into the design doc. If a hook "should fire" — prove it fires with
a probe before building on it (PROBE-DONT-GUESS).

## Step 4 — Park the receiver's brain (the divergence trap)

`COOP_WORLD_PROP_DIVERGENCE.md`: a value mirrored into an actor whose own Blueprint brain still
ticks will diverge or fight — both peers self-simulate. Every mirror therefore declares its
parking, and parking must be RESTORABLE (the quiesce rule: a latch that cannot restore what it took
is worse than one that refuses):

- Field latches: write the parked value, re-assert if the game flips it back, restore on authority
  arrival — only types whose prior value you can restore.
- Scheduler kills: clear/gate the timer or tick that drives self-simulation; assert-dead on a slow
  watch; restore on ownership return.
- Physics receivers: mirrors are driven kinematic (physics off) while remote-owned; re-latch if the
  game re-enables simulation; give velocity back on release. (Do NOT write a linear velocity into a
  body at rest — assigning a velocity is a WAKE.)
- NEVER park by neutering the entity (deleting constraints, swapping in a fake actor class) — that
  is the C1/C2 crutch shape: keep the engine entity, drive it (principle 3).

## Step 5 — Identity at birth, one row per actor

`COOP_ENTITY_EXPRESSION_MAP.md` + the stable-ID thread. Non-negotiables: identity is assigned at
the birth seam (spawn-catch / drop-intent / birth channel), never minted passively by a census
(v122: a passive census minted ~2200 zombie rows per join); one actor = one row (adoption, not a
second mint); a destroy/recreate transition (hold→drop→store→equip) CARRIES the identity across
the actor gap; keys are load-bearing including case. If two peers can create "the same" object
independently, the design must say which one is canonical and how the loser dissolves — before
shipping, not after the first dupe.

## Step 6 — The late-join row is part of the lane (principle 8)

A lane is not DONE until its mid-activity join answer exists in writing: snapshot / seed / park /
replay / unlatch — chosen, implemented, and listed in the lane's doc (`STATUS.md`'s late-join column
pattern). "Don't join during X" is a crutch. The join order itself is structured: identity → save
transfer → pre-world per-player state → world load → connect replay → per-lane seeds → ready gate;
a new lane picks its slot in that order explicitly.

## Step 7 — Wire discipline

New wire format or field = `kProtocolVersion` bump, same commit (`[[feedback-wire-format-change-bumps-protocol-version]]`).
New ReliableKind = walk the router checklist (`[[feedback-reliablekind-router-checklist]]`) — a
kind that parses but doesn't route is a silent black hole. Compatibility is byte-EQUALITY on the
Paper pair per lobby; the update check informs, never gates. Receive boundaries are strict on
FORMAT (refuse ill-formed wholesale) and permissive on MOTION/state (log, don't block — display
follows the sender; trust is a separate ledger).

## Step 8 — Evidence, or it didn't happen

- A build that compiles proves it compiles. The lane exists when the pre-handoff checklist passes:
  hot-path audit (no per-frame GUObjectArray walks, no heavy work per PE/tick), file-size check,
  deploy, ≥30 s two-peer smoke, log diff clean — with the evidence pasted (CLAUDE.md checklist; the
  forbidden handoff phrases list applies).
- Every detector/gate is shown RED before it is trusted (a gate that cannot fire is a PASS forever).
  Differential evidence beats absolute: baseline vs change, negative control included.
- Post-ship audit agents run on shipped code — that standing rule outranks session-level
  no-agents instructions.
- The design doc runs /qf to convergence BEFORE implementation (up to 15 rounds; "that holds").

## Forbidden patterns (the crutch smells — stop and re-derive the root)

- A filter/skip-if/suppress-X added where the SYMPTOM appears instead of where the cause lives.
- A receive-side gate protecting shared state (the producer is the side that gets suppressed).
- Two implementations of one concept compiled together; a flag re-enabling retired behavior.
- A mirror that "works" because the entity was replaced or lobotomized instead of parked.
- A per-frame full-array scan or any FindObjectByClass on a hot path; a cached engine pointer
  without a world stamp.
- A bound/clamp applied symmetrically to the host (the host may cheat; bounds are client-scoped).
- An unverified status label carried forward ("was open/done last session" is not evidence).
- Designing around a stated user requirement that blocks the proper fix — surface it; the
  requirement is an input, not an axiom (`[[feedback-drop-my-requirement-if-it-blocks-rule-1]]`).

## Worked references (read one before building your first lane)

- Act-as-host intent, complete and minimal: `coop/items/order_sync.cpp` (the laptop shop order).
- Presser-authored device state with claim-free deltas: the desk lanes (`docs/signals/`).
- Host-authoritative world family with echo interception: `coop/world/weather_rain.cpp`.
- Assigned-syncer vehicle direction (in progress, read the failures too): `docs/vehicles/ATV.md` §17.
- Identity across destroy/create: `coop/props/prop_drop_intent.{h,cpp}`.
- The join spine: `docs/join.md` + `coop/save/save_transfer.cpp`.

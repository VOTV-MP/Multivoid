# Destroy-seam blast radius (HOST-WIPE) + R-drop rock invisibility — RE 2026-07-08

**Two DISTINCT bugs that share the v106 `K2_DestroyActor` Func seam. Do NOT unify their fixes**
(the adversarial-agent caught an over-unification: a "client never authors keyed destroys" blanket
would be a RULE-1 suppress-X AND wrong for the rock). Both are RE/log-derived this session; NEITHER
fix is designed or built. Piles are OUT of scope (already fixed; do not touch).

Deployed at investigation time: DLL `753bb549` (rock `[ROCK-DROP]` diagnostics, UNCOMMITTED). HEAD `8cae8597`.

---

## BUG A — HOST-WIPE: client join-window purge churn wipes the host's keyed props

**Status: FIXED + USER-VERIFIED (hands-on, 2026-07-08). Root CONFIRMED on a CLEAN BARE-JOIN log [V log 11:54 +
probe 15:43] — zero player action. Fix = SOURCE-ANCHORED CLIENT-SCOPED WORLD-LOAD EPISODE LATCH (v107, DLL
`04ebfdb0`), shipped after /qf rounds 0-13. `coop::world_load_episode`: arm at the client join boot
(harness `DriveMenuModeJoinWorldBoot`, before `BootStorySaveBlocking` -- causal), clear at load-tail quiescence
(`join_membership_sweep.cpp:633` g_sweepFired), suppress the OUTBOUND broadcast of KEYED destroys in
`DestroySeamBody` while in-episode (eid-only pile destroys untouched). User confirmed the host world survives a
bare join. Punch-list open (non-blocking): host-symmetric arm + within-session world-change = unmeasured
extensions; the [HOSTWIPE-CALLER] probe was RETIRED. Rock (Bug B) is SEPARATE + still OPEN.**

### Symptom (user, hands-on)
"Last time host world had no PROPS at all, they got removed." The host's keyed props vanished en masse. Reproduced
on a deliberate bare-join test 11:54: client connected, user did NOTHING, host world emptied anyway.

### What the CLEAN bare-join log proves (11:54 run — no rock, no manual grab/throw either side)
- **Bare, verified**: `grab_hook[grab]`/`grab_hook[throw]` = **0 on BOTH host and client**; `[ROCK-DROP]` = **0**.
  Zero player action — the "did NOTHING" claim is log-verified, not asserted.
- **Timeline**: join `BeginConnect` 11:54:24 -> peer slot 11:54:26 -> `BeginSnapshot` (3183 objects) 11:54:33 ->
  CLIENT broadcasts **3,140 DESTROY** = **2,269 keyed props @11:54:38** (`eid=0`, `key='FXMI...'`, the wipe) +
  871 eid-only trash clumps @11:54:35 (pile-dedup) -> HOST receives 3,140 `OnDestroy`: **RESOLVES 2,066 by key
  -> "destroying local actor"** (the wipe), DEFERS 1,074 eid-only (no matching actor = harmless).
- **Host wiped, verified**: host re-seed went **3,345 -> 1,255 live keyed props** (2,099 dying) at 11:54:42, and
  **1,255 is the LAST re-seed before shutdown (11:54:52)** — the host never recovered. ~2,090 keyed props gone.
- This ISOLATES the leak: it is inherent to the join reconcile/purge, fires on a bare join, and is NOT the
  pile-throw stress (there was none this run).

### What the earlier BRAIDED log first showed (11:06 run — join + pile-throw, superseded by the clean 11:54 run)

### What the log proves (HOST=`Game_0.9.0n_HOST`, CLIENT=`Game_0.9.0n_CLIENT_1`, 2026-07-08)
- **11:06:44** — CLIENT join hits a world/level-change reconcile: `re-seed found 2556 live keyed props ... 857 dying`.
- **11:06:46-47** — CLIENT fires **2,270 keyed-prop DESTROY broadcasts** (`grab_hook[destroy-seam]: CLIENT
  broadcasting DESTROY ... key='FXMIrEnEjutSmIrUQwLOvw' eid=0`), and the **HOST executes 2,066 `OnDestroy ...
  eid=0 -> destroying local actor`** in the SAME 2-second window. Near 1:1. Host had ~3,343 keyed props seeded
  (11:05:29) -> ~2,066 wiped -> host world emptied.
- **NOT the claim sweep**: the sweep fired 9 s LATER (11:06:55) and destroyed only **110** locals; the pile
  completeness floor kept the chipPiles (docs/piles/10 fix working).
- **NOT piles**: victims are KEYED props (`eid=0`, key-authored — `drone_InventoryContainer` etc.), NOT
  chipPiles (eid-only, key=`None`, floor-protected).

### Mechanism — REFINED by reading the 11:54 origin (two DISTINCT destroy populations)
Reading the destroy burst in the 11:54 log (not inferring from the count) splits it into two populations
with DIFFERENT origins, timings, and host impact:
- **871 eid-only trash-clump destroys @11:54:35** = the CLIENT's pile-twin DEDUP (`[PILE] DESTROY native
  level-pile twin ... proxy is the sole mirror now`). These carry the client's LOCAL eids (e.g. 42821) which
  the host does not have -> host `OnDestroy` **DEFERS (1,074 "no local actor")** = HARMLESS. Piles stay out of
  the wipe (consistent with piles-already-fixed; do not touch).
- **2,269 keyed-prop destroys @11:54:38 (single frame), 2,268 in ONE frame** = the actual WIPE. They carry
  real save Keys (`key='FXMI...'`, `drone_InventoryContainer`) -> host `OnDestroy` **RESOLVES 2,066 by key ->
  "destroying local actor"** -> host world emptied. The burst fires immediately after an ENGINE-side GC hitch
  (`[HITCH] frame=69ms ... GC/render/physics -- permanent-both-peers GC signature`) during the save-load
  world-swap (host save `zcoop_6800.sav` written 11:54:30, client loading it).
- **What the log does NOT record: the CALLER of those 2,269 `K2_DestroyActor` calls.** The destroy-seam log
  line prints the broadcast, not the callstack. WHICH code path issues the burst is STILL RE-pending (see the
  dispatch-route method below). "reconcile/purge" was an inference; the measured facts are the timing, count,
  key-resolution, GC coincidence, and the destroy+recreate trace below.

### Mechanism — MEASURED: a client-local DESTROY+RECREATE churn, only the destroy half reaches the host
Tracing one wiped key end-to-end in the 11:54 client log (`FXMIrEnEjutSmIrUQwLOvw` = `secBoothDoor`, a `prop_C`):
1. **11:54:33** `remote_prop::OnSpawn key='FXMI...' resolves to LIVE actor CE54D280 -- already aligned (d=0.00cm)`
   then `CreateOrAdoptPropMirror: eid=2297 bound to actor=CE54D280` — the client ALREADY had this keyed actor
   (its own save) and ADOPTED it WITH eid=2297.
2. **11:54:38** `broadcasting DESTROY actor=CE54D280 key='FXMI...' eid=0` — the SAME actor is destroyed; the seam
   broadcasts `eid=0` -> host resolves by KEY and destroys ITS copy.
3. **11:54:45** `join_membership_sweep: keyed churn RE-BIND -- unclaimed 'prop_C' key='FXMI...' is the RE-CREATE
   of already-expressed eid=2297 (mirror row held a dead actor) -> row rebound, actor claimed, NOT doomed`.
So the world-swap (client loading the host save) DESTROYS **and RE-CREATES** the client's keyed props same-key.
Locally that is net-zero (sweep rebinds). But on the wire it is ASYMMETRIC: the **destroy half fires the seam ->
host destroys its authoritative copy**, while the **recreate is NEVER broadcast to the host**. Net: host loses
the prop permanently; client keeps it. This is the WIPE, and it is a firing-set problem (H1): the game's
world-swap destroy+recreate churn should be net-zero on the wire, but the seam carries only the destroy half.

> **CORRECTION (QF-workflow measured 2026-07-08, run `wf_8e0bab5d`):** the claim that the recreate is "a client
> keyed SPAWN skipped at `prop_lifecycle:195`" is **REFUTED for this run** — `:195` PROVABLY did not fire
> (`[ROCK-DROP]`=0 both logs; only 2 `Aprop.Init POST` lines all run, both `actorChipPile_C`, neither FXMI). The
> recreate reaches the client via the **`join_membership_sweep` scan-rebind** (a client-LOCAL reconcile that
> rebinds eid=2297 to a DIFFERENT actor `723C782000`), which has **no host broadcast at all** — that, not a
> `:195` authority skip, is why the host is never re-told. Both H1's "recreate gated at :195" and H2's
> authority-asymmetry justification rest on a gate that did not execute. See the MEASURED FACT BASE below.

### The eid=0 discriminator — MEASURED here, but a CONFLATED wire sentinel (not type-safe)
- **This instance is reading (a):** the element WAS assigned (eid=2297 @11:54:33) and was drained/in-flux by the
  destroy @11:54:38 — NOT "adopted-by-key, never assigned an eid." So for the wiped props, `eid=0` genuinely
  means "had a live element, lost it during the destroy+recreate churn."
- **BUT `eid=0` is a CONFLATED wire sentinel** (`prop_lifecycle.cpp:373`: `dp.elementId = (destroyEid ==
  kInvalidId) ? 0 : destroyEid`, per the protocol.h contract "elementId==0 -> sender had no Element"). Both
  "element drained/raced to kInvalidId" AND "genuinely never had an Element" map to wire 0. The v12 comment at
  `:363-368` names the race explicitly: "actor may have been Unmark'd already by the time we get here
  (parallel-anim race)". So a LEGIT intent destroy that races to kInvalidId would ALSO arrive `eid=0`.
- => "eid=0 ⇒ teardown" is a THIS-INSTANCE measurement, NOT a type guarantee. An eid-liveness gate is plausible
  but NOT proven safe (see the census gap below). A cleaner discriminator the trace suggests: **a destroy
  immediately followed by a same-key RE-CREATE is not a real destroy** (`join_membership_sweep` already detects
  this post-hoc) — or the authority symmetry (client never authors keyed destroys, mirroring the `:195` spawn
  skip). Both still need the caller RE + the census.

### SAFETY GAP for any eid=0 gate (uncensused)
The pile-twin dedup carried LIVE client eids (42821…), not eid=0 — one data point, not a census. No enumeration
of client-authored keyed-destroy paths against eid=0 exists yet. Because eid=0 is conflated (above), a legit
keyed destroy CAN arrive eid=0 via the parallel-anim race -> an eid=0 gate could wrongly suppress it. Precondition
for the gate: census every client keyed-destroy path (rock R-pickup = live eid=X, morph destroys, intent relays)
and confirm none legitimately presents eid=0. Until then the gate is UNSAFE to build.

### Dispatch-route RE — the CONCRETE method to name the caller (answers "how, not hand-wave")
- The seam callback `OnK2DestroyFunc(context, srcObj, result)` (`prop_lifecycle.cpp:385`) already RECEIVES
  `srcObj` = the FFrame caller object, currently IGNORED. Cheapest probe: log `srcObj`'s class/name +
  `RtlCaptureStackBackTrace` (symbolized into the game module) at the callback — a one-line diagnostic addition
  (RULE-2-exempt), rebuild + the user re-runs the bare-join.
- **Guardrail against "route confirmed, caller unknown":** the seam is a Func patch on the `K2_DestroyActor`
  UFUNCTION — it fires only when that UFunction is INVOKED (BP / EX_CallMath / ProcessEvent), NOT on a raw
  native `AActor::Destroy()`. The 2,269 firings therefore PROVE a UFunction-level invoker exists -> there IS a
  BP/UObject caller for `srcObj`+backtrace to name (pure-native world teardown would be INVISIBLE to this seam,
  so "fired but no nameable caller" is self-contradictory). Worst case the backtrace lands in a native thunk,
  which still localizes the call site. Confirms H1 (game world-swap teardown) vs H2 (a deliberate reconcile-diff).

### Root FRAMING — NAMING THE CALLER IS A PRECONDITION (both live hypotheses presuppose it)
- **The candidate `InPurgeEpisode()` gate is TIMING-INVALIDATED by the log.** net_pump raises the mass-purge
  flag at **11:54:40** ("reaped 256 >= 64"), but the keyed-destroy wipe fires at **11:54:38** — the flag is
  NOT set during the burst. Whether gated on the client SEND or the host EXECUTE, `InPurgeEpisode()` as wired
  would be inactive at the moment of the wipe. It also only removes a timing-classified subset. => skip-flag in
  a nicer hat (RULE 1), and it does not even cover this window. DEAD.
- **PRECONDITION (adversarial-agent, correct): the fix framing cannot be chosen until the CALLER of the 2,269
  keyed `K2_DestroyActor` calls is named.** The two live hypotheses each PRESUPPOSE a different caller:
  - **(H1) FIRING-SET / §8 narrowing** — if the caller is the GAME's world-swap TEARDOWN (client tearing down
    its OLD world to load the host save), then the v106 seam — built to carry **eid-only clump/pile morphs** —
    is now ALSO firing on **keyed-prop world-teardown it was never meant to carry**. Fix = narrow the firing
    set back to its intended payload, NOT an authority gate. This is the LEANING hypothesis: (a) no our-code
    reconcile-destroy marker precedes the burst in the 11:54:37-39 window (only `CreateOrAdoptPropMirror` binds
    + pile morphs); (b) 2,268 destroys in ONE frame right after a `[HITCH] 69ms GC` during the save-load = a
    bulk engine teardown, not an incremental reconcile. CIRCUMSTANTIAL — the seam logs no caller.
  - **(H2) AUTHORITY asymmetry** — if instead a deliberate client reconcile-diff compares client-world to
    host-world and destroys "dupes", the root is that the client authors keyed destroys at all (it already
    SKIPS keyed SPAWNS at `prop_lifecycle:195`); fix = client keyed destroys are LOCAL-ONLY, symmetric with
    the spawn skip.
  - Both converge on roughly the same code IF the discriminator is keyed-ness; they DIVERGE on justification
    and edge cases. **A candidate INVARIANT worth testing in the RE (not yet confirmed): the wipe destroys are
    `eid=0` (key-only) because the element was ALREADY DRAINED before the actor died = teardown/GC ORDER; a
    genuine tracked player-intent destroy would carry a LIVE eid.** If that holds, "client broadcasts a keyed
    destroy only when the element is still LIVE" gates teardown without a timing flag and without blanket
    keyed-suppress. Must verify (does the rock R-pickup carry a live eid? do all teardown deaths present eid=0?).
- **DECISION GATE: the dispatch-route RE (name the caller + the eid-liveness at the seam) is a PRECONDITION for
  the root analysis, not a follow-up.** Only after it does H1-vs-H2 resolve and the discriminator get chosen.

### v106-regression evidence (now STRONGLY supported)
| | client `broadcasting DESTROY` | host `OnDestroy` executed |
|---|---|---|
| pre-v106 (`research/crash_2026-07-03_rehost_wispkill`, PE observer) | 840 | **10 — no wipe** |
| post-v106 bare join (2026-07-08 11:54, Func patch `29dfd079`) | 3,140 | **3,140 -> host 3,345->1,255 = wipe** |

Logic: the mod shipped working joins before v106 (joins did not empty the host). Post-v106, a BARE join with
zero player action empties the host. The only thing that changed the destroy DISPATCH set is the v106
`K2_DestroyActor` Func patch (it catches a wider firing set than the old PE observer). Combined with the pre-v106
counterfactual (10 host destroys on a rehost, no wipe), this is strong regression evidence. Caveat: the pre-v106
row is a crash-REHOST, not a clean join, so the counterfactual is directional — but it no longer gates priority
or the "is it real" question; the clean 11:54 bare-join log settles both.

### ISOLATION status
1. **USER — clean bare-join repro: DONE (11:54 log above).** Host wipes on a plain join, zero player action,
   no rock, no manual pile-throw. The leak is inherent to the join reconcile/purge, not a pile-throw trigger.
2. **ME — dispatch-route RE: still useful, but NO LONGER blocking.** It is no longer needed to establish THAT
   v106 regressed or the priority (the clean log + counterfactual carry those). Its remaining purpose is to
   LOCATE the fix: confirm the client's join-purge keyed destroys dispatch via the `EX_CallMath`/native route
   the PE observer could not see -> the classification gate (`InPurgeEpisode()`) belongs AT the v106 seam, not
   bolted on elsewhere. Feeds the fix design, not the diagnosis.

---

## BUG B — ROCK R-drop invisibility (client places a pre-existing rock -> host can't see it)

**Status: ROOT RE-derived from bytecode + code [RD 2026-07-08]. NOT captured in a real log this session
(`[ROCK-DROP]`=0 in the 07-08 run — that run was pile-throw-during-join, not a rock repro). Fix shape
UNSETTLED. Needs a CLEAN rock-only repro.**

### Symptom (user, "from other times")
CLIENT picks up a rock with **R**, holds it, places it with **R** -> the rock is invisible to the HOST until an
E-grab (and "sometimes even E can't recover it").

### Ground truth (RE agent, mainPlayer bytecode + `ue_wrap/prop.cpp`)
- A "rock" is a generic **`prop_C`/`Aprop_C`** instance (name-driven from `list_props`), a KEYED interactable
  (Key @0x02E0). `IsDescendantOfProp`/`IsKeyedInteractable` both true.
- **R = the `drop` input ACTION.** Decision tree: `grabbing_actor` valid -> Hold-into-hand; else `holding_actor`
  valid -> `simulateDrop` (PLACE); else `lookAtActor` valid -> `Hold Object` (PICK UP into hand).
- **R-pickup**: `Hold Object` -> `getData`@1653 -> `addEquip`@1699 (which SYNCHRONOUSLY calls `updateHold` ->
  `FinishSpawningActor`@1217 = hand DISPLAY actor born) -> `ac.K2_DestroyActor()`@1754 (world rock DESTROYED).
  So the hand successor is born BEFORE the world prop dies (migration window EXISTS) — but the successor is a
  display-only, per-switch-churning hotbar actor, not a world entity.
- **R-drop/place**: `simulateDrop` -> `BeginDeferredActorSpawnFromClass` + `FinishSpawningActor`@117551 ->
  `loadData`(restores save Key) -> a **FRESH `Aprop_C` world actor**. **R-DROP IS A SPAWN** (this REFUTES the
  v106 model's "an R-drop/place is NOT a spawn" claim in COOP_ENTITY_EXPRESSION_MAP.md:29 — corrected 2026-07-08).
- One R-pickup destroys **exactly one** world prop; no mainPlayer mass-destroy loop (rules the blast-radius OUT
  of the mainPlayer R path — the host-wipe comes from the join reconcile, Bug A, not here).

### Mechanism (code-proven)
1. R-pickup destroys the world rock -> the bidirectional destroy seam broadcasts `DESTROY(eid=X)` (client
   `prop_lifecycle:376` -> host `remote_prop_destroy:84`) -> **host loses its copy** (eid retired both peers).
2. R-drop spawns a FRESH `Aprop_C` on the client -> its Init-POST spawn-catch **silently returns at
   `prop_lifecycle:195`** (`if (IsDescendantOfProp(self)) return; // Aprop_C host-authoritative, skip client
   broadcast`) -> **host never told -> invisible.** (Added a `[ROCK-DROP]` diag there to make the silent skip
   visible.)
3. E-grab re-expresses via the `grabbing_actor` lane (`EnsureHeldItemBroadcast`) — "sometimes" because if the
   fresh rock became tracker-known, that path DECLINES at `trash_collect_sync:339` ("pose stream suffices" — a
   false promise for a no-longer-streamed prop).

### Fix shape — UNSETTLED (do NOT build without green-light; feature-grade)
- Candidate discriminator "owns an eid at pickup" REJECTED (doesn't separate suspend-into-hand from a genuine
  delete — both drain the row).
- §9-clean shape (successor claims eid at its birth -> the pickup-destroy resolves a husk, no skip-flag) is
  FEASIBLE (migration window exists) BUT the successor is the churning display hand actor -> fragile; collides
  with the v105 "hotbar = player expression, not a world entity" decision (`project-hand-item-axis-2026-07-06`).
- One-owner (RULE 2026-05-28): route through the EXISTING held-prop author seam E uses, NOT a new drop-seam path.
- Spans hand_item + destroy seam + element registry + author seam = feature-grade -> §1 written root analysis
  + explicit per-rule-1 green-light BEFORE design. Fix N=1 rock first, generalize at N>=3 (§11).

### Coupling to the host-wipe fix — CORRECTED: NOT forced to pair; host-wipe ships ALONE
Earlier claim "the authority-fix STRANDS the rock -> they must pair" was an OVERSTATEMENT. Reality:
- The rock's R-pickup broadcasts `DESTROY(**eid=X**)` — a LIVE eid (the rock is tracked). The host-wipe destroys
  are `eid=0` (element already drained = teardown). So the eid-liveness invariant (gate `eid=0` keyed destroys)
  fixes the host-wipe and **does NOT touch the rock's eid=X pickup broadcast at all -> rock UNCHANGED.**
- Even under a blanket keyed-suppress, removing the client's keyed-destroy broadcast would only make R-pickup
  leave the HOST's copy in place (host shows a stale rock at the pre-pickup position instead of nothing) — a
  DIFFERENT desync, not a WORSE one; the rock is already host-invisible on R-drop regardless.
- => Nothing FORCES pairing. **Host-wipe fix is shippable ALONE; the rock defers** to the clean intent-channel
  hook (route R-pickup/R-drop through the held-prop author seam). Verify the "unchanged vs stale-at-old-pos"
  distinction with the rock repro when the rock work starts; it does not gate the host-wipe.

### Diagnostics added this session (RULE-2-exempt, log-only, UNCOMMITTED, in DLL `753bb549`)
- `prop_lifecycle.cpp:195` — logs the silent client-Aprop spawn skip (`[ROCK-DROP] CLIENT Aprop spawn NOT
  authored ... key eid loc`).
- `trash_collect_sync.cpp:339` — logs the tracker-known DECLINE.
- `hand_item.cpp` `ExpressReleasedHandActor` — logs whether prev survived (release) or died (destroy+respawn).

### Clean rock-only repro (PENDING user): pre-existing rock, NO join, NO piles -> R-pick -> R-drop -> check host
-> E-grab -> reload. Read HOST+CLIENT `votv-coop.log` for the `[ROCK-DROP]` lines + the destroy/spawn pair.

---

## MEASURED FACT BASE — QF-workflow run `wf_8e0bab5d` (2026-07-08, capped 4 rounds, NOT converged)
Adversarial SEARCH+AUDIT loop over the 11:54 host+client logs + current code + BP bytecode. It REFUTED two of
my inferences and upgraded others to measured. Each fact below is `[V-log]`/`[V-code]`/`[V-bytecode]` measured
unless tagged. Full per-question evidence + citations: workflow journal (this run).

**MEASURED (settled):**
1. **Host world INTACT at join-window close** `[V-log]`: host `save_transfer: slot 1 -- blob-vs-live diff sent 0
   explicit PropDestroy (blob 2232 keyed == host live 2232)` @11:54:33. The host's OWN join handshake wiped
   nothing -> the wipe is 100% the client's post-join destroy flood, not a host-side diff.
2. **v106 is a MEASURED regression** (upgraded from "suggestive") `[V-log+git]`: THREE pre-v106 client-join runs
   (v88 failrun-s31, v80 baseline, v88 prestrip-s32) each ran the IDENTICAL local world-purge (`reaped 256 dead
   ... 256 keyed ... structural world event`) yet broadcast **ZERO** destroys. Commit `29dfd079` changed ONLY
   the dispatch mechanism (PE PRE observer -> `K2_DestroyActor` UFunction Func patch); the pre-v106 body was
   ALREADY bidirectional/client-capable (gated only on `connected()`). Route = BP-VM EX_* dispatch: PE-invisible,
   Func-visible. So v106 is what first put the client's join-purge destroys on the wire.
3. **The seam is the SOLE broadcast source** `[V-log+code]`: exactly 3,140 `broadcasting DESTROY` lines, ALL in
   11:54:35-39; ZERO after 11:54:40. GHOST-RETIRE + the mass-purge reap route through the same native
   `K2_DestroyActor` but BOTH pre-suppress (`MarkIncomingDestroy` echo-mark / `UnmarkKnownKeyedProp` eid-drop ->
   `keyless && !hasEid` return). => a firing-set fix covering the burst leaves NO residual wipe.
4. **Caller STATICALLY NAMED** `[V-bytecode]` (per-call runtime attribution still UNMEASURED — see U1): the
   load-tail caller is the game's `mainGamemode.loadObjects` (+ `Load Primitives`), which the MOD itself triggers
   (`engine_save.cpp:433` sets `mainGameInstance_loadObjects=1`). kismet-analyzer disasm: `loadObjects` issues
   `K2_DestroyActor` at 3 sites; victim mix maps cleanly (2269 keyed = `loadObjects`/objectsData; 871 chipPile =
   `Load Primitives`/primitivesData).
5. **All 3 loadObjects destroy sites are CHURN, none a player death** `[V-bytecode]`: `[249]`/`[277]` = fresh
   same-call dedup twins; `[315]` = world-restore pre-delete over `GetAllActorsWithInterface(int_save_C)` (ALL
   live pre-existing save-actors, superseded by the incoming save). A player-REMOVED prop CANNOT be a `[315]`
   victim (it is already gone -> not returned by GetAllActors). => an "inside a load episode" classifier's blast
   radius is STRUCTURALLY bounded to churn; it cannot drop a legit single-entity removal.
6. **eid=0 is a DEAD classifier** `[V-log]` (and my count was off): the keyed burst is **2,110 eid=0 + 159
   live-eid** (not "2269 eid=0"). Of the 2,110 eid=0, **2,066 (97.9%) held a REAL adopted eid seconds earlier**;
   2,065/2,066 destroyed the SAME actor address that held the eid = the `:373/:333` drain race. An eid=0 gate
   would suppress genuine drain-race destroys -> provably UNSAFE.
7. **H2 (client keyed destroys LOCAL-ONLY) drops no WIRED legit event** `[V-code]`: `ReliableKind` has NO
   pickup/take/remove intent for a keyed prop; `InventoryPickup=47` is cosmetic-only; `GrabIntent=78`/`ThrowIntent=79`
   are pile-only; `Aprop_C` is declared host-authoritative. BUT see U2 — this is not the same as proven-safe.

**REFUTED (my prior claims, now measured-false):**
- **":195 gates the recreate -> host never re-told"** — REFUTED (fact above + the CORRECTION banner in the
  Mechanism section). `:195` never fired this run; the recreate route is the `join_membership_sweep` scan-rebind,
  which simply has no host broadcast. H2's authority-asymmetry *justification* leaned on this and is thus unproven.
- **"eid=0 could be a teardown discriminator"** — REFUTED (fact 6): eid=0 is 97.9% drain-race over live eids.

**RESIDUAL UNKNOWNS — all require a REBUILD (outside this read-only run):**
- **U1 — per-call caller attribution.** loadObjects is NAMED but the seam DISCARDS `srcObj` (`OnK2DestroyFunc(...,
  void* /*srcObj*/, ...)` @`:385`), so what FRACTION of the 2,268 resolves to `loadObjects[315]` vs GC-finalize
  vs an our-code reconcile-adopt path is UNMEASURED. An "inside a load episode" gate is unbounded until this is
  pinned. FIX: log `R::ClassNameOf(srcObj)` + `RtlCaptureStackBackTrace` in `OnK2DestroyFunc`, rebuild, re-run
  the bare-join, histogram the callers. (Static disasm can only bound the site-set, not attribute a runtime call.)
- **U2 — de-braid control repro (NOT passed).** Both logs are churn-ONLY (0 GrabIntent/ThrowIntent sends,
  ROCK-DROP=0). `COOP_DISPATCH_VISIBILITY.md:86`: the rock R-pickup (`putObjectInventory2@719`) fires
  `K2_DestroyActor` through the SAME seam -> any seam-side classifier could silence it identically. No
  intent-bearing destroy exists here to diff against, so no invariant can be shown safe. FIX: a bare-SESSION run
  (no join) where the client fires exactly ONE real R-pickup keyed destroy, with the same srcObj+backtrace
  instrumentation; diff its seam-instant fields vs the burst.
- **U3 — positive recreate route / would :195 EVER gate a same-key recreate.** Only the NEGATIVE ("not via :195/
  Init-POST this run") is measured. FIX: srcObj+backtrace at Init-POST `:127` and `:195`, or IDA-callstack the
  `FinishSpawningActor` site for a same-key `prop_C` during `loadObjects`.

**Where this leaves the root analysis (for the visible main session, NOT decided here):** H1 (firing-set: the
seam carries the destroy half of a game load-episode churn that is net-zero locally) is the strongly-supported
frame — caller named, sites classified churn-only, seam is sole source, v106 measured. The gating open item is
U1 (prove ~100% of the burst is the load episode, not GC/reconcile) + U2 (prove any chosen classifier does not
also silence the rock). Both need one instrumented rebuild + re-run. No fix is chosen or designed here.

---

## ROOT ANALYSIS + FIX DESIGN (2026-07-08, `[HOSTWIPE-CALLER]` probe run 15:43-15:45 + gamemode disasm)

**Status: FIXED + USER-VERIFIED (2026-07-08, DLL `04ebfdb0`).** The fix REVERSED from the Node-gate to a
SOURCE-ANCHORED CLIENT-SCOPED EPISODE LATCH across /qf rounds 0-13 (the Node-set was a §9 site-list; the
latch is the invariant, armed at the causal loadObjects trigger, cleared at the deadline-capped quiescence,
covers every in-window churn issuer with no leak). See the FIX AS-BUILT section below. The `[HOSTWIPE-CALLER]`
probe (DLL `f2fda78`) was retired once the caller was measured.

### The measured causal chain (host-wipe)
1. Client join → the mod triggers the game's world-load (`engine_save.cpp:309/433` set `mainGameInstance_loadObjects=1`).
2. `mainGamemode.loadObjects` (+`Load Primitives`/`loadTriggers`) runs its world-restore **pre-delete**: it
   destroys the client's live keyed props via `K2_DestroyActor` and respawns them from the transferred save.
   This is a **local, net-zero** rebuild — the client re-binds via `join_membership_sweep` (always-run path).
3. **v106 regression** (`29dfd079`): the `K2_DestroyActor` **UFunction Func-patch** catches these EX_*-dispatched
   destroys the pre-v106 ProcessEvent observer never saw, and **broadcasts each to the host**. [MEASURED: 3
   pre-v106 runs ran the identical purge, broadcast ZERO.]
4. Host `remote_prop::OnDestroy` resolves each by Key and destroys its authoritative copy → **3345→1255, never
   recovers.** The identity damage ripples: later legit interactions land on wiped actors (the 15:44 food-box
   morph hit "no local actor → DEFERRING" because the wipe had already deleted key `NLj6` at 15:43:54; the box
   recovered only via a new-key `7MUo` respawn). [MEASURED, 15:43-15:45 run.]

### Why the discriminator is the EPISODE, not the caller class (the decisive measurement)
The `[HOSTWIPE-CALLER]` probe measured **2270/2270 wipe destroys = caller class `mainGamemode_C`**, and the two
legit post-join destroys = **self-caller** (`prop_foodBox_C` food morph @15:44:30; `trashBitsPile_C` trash-container
E-press @15:44:51). That looked like "deny caller==gamemode." **But disasm of `mainGamemode` refutes a class
gate:** the gamemode issues `K2_DestroyActor` from **9 functions** — world-load churn (`loadObjects`×3,
`Load Primitives`×1, `loadTriggers`×2) AND **player intent** (`putObjectInventory`×1 = the **R-pickup/rock**,
`RemoveEquipment`×1, `undo`×1) plus events (`processDream`×2, `spawnRedSky`×1, `ExecuteUbergraph`×3). ALL carry
caller class `mainGamemode_C`. So **"deny caller==`mainGamemode_C`" is TOO BROAD — it would suppress the rock
R-pickup (`putObjectInventory`), `RemoveEquipment`, `undo`.** The class separates gamemode-issued from
prop-self-issued, but NOT load-churn from gamemode-issued INTENT.

### The fix: peer-symmetric WORLD-LOAD EPISODE gate (invariant, not a site-list)
**While a peer is inside its own `loadObjects` world-load episode, that peer does NOT broadcast keyed-prop
destroys** — they are local world-rebuild churn, never a cross-peer event. Concretely:
- **Set** a `g_inWorldLoad` latch where the mod triggers the load (`engine_save.cpp:309/433`, right at
  `loadObjects=1`); **clear** it at load-complete / first post-load quiescence (reuse the existing
  purge/drain-edge machinery — `net_pump` mass-purge/drain already brackets this window).
- At `DestroySeamBody`, when `g_inWorldLoad` is set, **skip the broadcast** (the local `K2_DestroyActor` still
  runs; only the wire send is suppressed). **Peer-symmetric**: the seam already carries role; the gate keys on
  "am I running MY world-load," not on role — so a HOST mid-session reload also stops wiping its CLIENTS.
- This restores the exact pre-v106 property (world-load churn stays local) as an **episode invariant**, not a
  caller allowlist: any churn source inside the load window is covered; anything OUTSIDE it (player verbs) is not.

### What it provably preserves (measured, all fire OUTSIDE the load episode or self-caller)
- **Food/container morphs** (`prop_foodBox_C`, `trashBitsPile_C`) — self-caller, post-load → broadcast normally.
- **Rock R-pickup** (`putObjectInventory`, gamemode-caller) — fires post-load, outside the episode → NOT
  suppressed. (Resolves the earlier fear that the rock shares the gate; it does not.) Rock stays independent (Bug B).
- **Pile morphs** — post-load, self-caller → untouched (piles already fixed; the eid-only clump path v106 was
  added for keeps working).
- `RemoveEquipment` / `undo` / event destroys — post-load → unaffected.

### Residuals / DESIGN choices for the `/qf` vetting round (NOT decided here)
1. **Window bounding (the one real risk):** `loadObjects` is ASYNC (materializes over ~2-4 s; the 15:43 burst
   spanned :51-:55). The `g_inWorldLoad` latch must stay set across the whole load burst but clear before any
   player verb — mis-timing either leaks (clears early → residual wipe) or over-suppresses (clears late →
   drops an early post-load intent). The existing drain-edge/quiescence signal is the candidate clear-point;
   must confirm it brackets the FULL destroy burst. (Echoes the InPurgeEpisode timing failure — verify the
   window, don't assume it.)
2. **Latch vs FFrame::Node:** alternative to the episode latch = gate on the executing UFunction
   (`FFrame::Node == loadObjects/Load Primitives/loadTriggers`) directly at the seam — more precise (no window
   to bound) but reads more engine internals per destroy. Weigh precision vs per-call cost.
3. **Re-entry / nested loads:** confirm the latch is set/cleared correctly if a load re-triggers (rejoin,
   cave/level travel) — a bare bool must be a depth counter or idempotent set+quiescence-clear, not toggle.

## Source map
`prop_lifecycle.cpp` (DestroySeamBody:313 / OnK2DestroyFunc:385 = the v106 Func seam `29dfd079`; :195 client
Aprop spawn skip; :376 destroy broadcast) · `remote_prop_destroy.cpp:84` (host OnDestroy) · `net_pump.cpp`
(InPurgeEpisode set/clear) · `trash_collect_sync.cpp:339` (tracker-known decline) · `hand_item.cpp`
(ExpressReleasedHandActor) · mainPlayer bytecode (Hold Object / addEquip / updateHold / simulateDrop) ·
`docs/COOP_DISPATCH_VISIBILITY.md:86` (the seam row) · `docs/COOP_ENTITY_EXPRESSION_MAP.md:29` (R-drop row,
corrected) · OPUS_48_DISCIPLINE §8 (widened-seam blast radius) · docs/piles/10 (the SEPARATE, fixed client sweep).

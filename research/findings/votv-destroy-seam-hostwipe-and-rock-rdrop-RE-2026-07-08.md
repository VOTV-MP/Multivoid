# Destroy-seam blast radius (HOST-WIPE) + R-drop rock invisibility — RE 2026-07-08

**Two DISTINCT bugs that share the v106 `K2_DestroyActor` Func seam. Do NOT unify their fixes**
(the adversarial-agent caught an over-unification: a "client never authors keyed destroys" blanket
would be a RULE-1 suppress-X AND wrong for the rock). Both are RE/log-derived this session; NEITHER
fix is designed or built. Piles are OUT of scope (already fixed; do not touch).

Deployed at investigation time: DLL `753bb549` (rock `[ROCK-DROP]` diagnostics, UNCOMMITTED). HEAD `8cae8597`.

---

## BUG A — HOST-WIPE: client join-window purge churn wipes the host's keyed props

**Status: OUTCOME + LEAK CONFIRMED from a REAL log [V log 2026-07-08 11:06]. v106-regression = SUGGESTIVE, not proven.
Fix = DESIGN only (not started). Needs a CLEAN bare-join repro before the root analysis is called settled.**

### Symptom (user, hands-on)
"Last time host world had no PROPS at all, they got removed." The host's keyed props vanished en masse.

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

### Mechanism (log-derived, high confidence)
The CLIENT's join-window world-reload/reconcile purges its LOCAL keyed props. Each local death dispatches
`K2_DestroyActor`, which the **v106 bidirectional destroy seam** (`prop_lifecycle::DestroySeamBody`) catches
and **broadcasts to the host** (key-only, `eid=0` because the element was already drained). The host's
`remote_prop::OnDestroy` resolves the key to its OWN prop and destroys it -> host world wiped. The client's
local GC/reload churn should NEVER tell the host to delete its authoritative world; the seam has no gate that
distinguishes a client's local purge-churn from a real destroy.

### Root FRAMING (corrected by the adversarial agent — this is the important part)
- It is a **§8 classification gap**, NOT an authority hole: the v106 `K2_DestroyActor` **Func patch**
  (`29dfd079`) fires on a WIDER dispatch set than the pre-v106 ProcessEvent observer — including the
  join-window purge/reconcile churn the PE observer was blind to. The seam **cannot tell a player-INTENT
  destroy from purge/GC/reconcile CHURN**.
- **The fix is to CLASSIFY the dispatch context, not to blanket-suppress.** A blanket "client doesn't author
  keyed destroys" reads as suppress-X (RULE 1) and throws out legitimate client destroys (clump/pile morphs,
  intent relays). Candidate invariant already exists and the seam never consults it: **`InPurgeEpisode()`**
  (`net_pump` sets it on a mass-purge detection, clears at drain-complete). §8 also says prefer an INVARIANT
  over a skip-flag/site-list.
- **If v106 introduced it, the cleanest fix is at the seam change**, not a new gate bolted on top.

### v106-regression evidence (SUGGESTIVE, not proof)
| | client `broadcasting DESTROY` | host `OnDestroy` executed |
|---|---|---|
| pre-v106 (`research/crash_2026-07-03_rehost_wispkill`, PE observer) | 840 | **10 — no wipe** |
| post-v106 (2026-07-08, Func patch `29dfd079`) | 3,142 | **2,066 — wipe** |

The jump (840->3142 broadcasts, 10->2066 host executions) matches the §8 signature: the Func patch catches a
new firing set (the join-purge churn). BUT the pre-v106 log is a crash-REHOST, not a clean join -> directional
only.

### ISOLATION before the root analysis is settled (both PENDING)
1. **USER — clean bare-join repro**: two peers, client joins, NO rock, NO manual pile-throwing. Does the host
   lose its keyed props on a PLAIN join? Proves bare-join vs pile-throw-stress trigger.
2. **ME — dispatch-route RE**: confirm the client's join-purge keyed destroys dispatch via the
   `EX_CallMath`/native route the PE observer could NOT see (would pin the v106 regression -> fix at the seam).

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

### Diagnostics added this session (RULE-2-exempt, log-only, UNCOMMITTED, in DLL `753bb549`)
- `prop_lifecycle.cpp:195` — logs the silent client-Aprop spawn skip (`[ROCK-DROP] CLIENT Aprop spawn NOT
  authored ... key eid loc`).
- `trash_collect_sync.cpp:339` — logs the tracker-known DECLINE.
- `hand_item.cpp` `ExpressReleasedHandActor` — logs whether prev survived (release) or died (destroy+respawn).

### Clean rock-only repro (PENDING user): pre-existing rock, NO join, NO piles -> R-pick -> R-drop -> check host
-> E-grab -> reload. Read HOST+CLIENT `votv-coop.log` for the `[ROCK-DROP]` lines + the destroy/spawn pair.

---

## Source map
`prop_lifecycle.cpp` (DestroySeamBody:313 / OnK2DestroyFunc:385 = the v106 Func seam `29dfd079`; :195 client
Aprop spawn skip; :376 destroy broadcast) · `remote_prop_destroy.cpp:84` (host OnDestroy) · `net_pump.cpp`
(InPurgeEpisode set/clear) · `trash_collect_sync.cpp:339` (tracker-known decline) · `hand_item.cpp`
(ExpressReleasedHandActor) · mainPlayer bytecode (Hold Object / addEquip / updateHold / simulateDrop) ·
`docs/COOP_DISPATCH_VISIBILITY.md:86` (the seam row) · `docs/COOP_ENTITY_EXPRESSION_MAP.md:29` (R-drop row,
corrected) · OPUS_48_DISCIPLINE §8 (widened-seam blast radius) · docs/piles/10 (the SEPARATE, fixed client sweep).

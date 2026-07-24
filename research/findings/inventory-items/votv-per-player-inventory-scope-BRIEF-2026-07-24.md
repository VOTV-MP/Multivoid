# What must per-player inventory COVER? — a question brief for the user (2026-07-24)

**This is not a design.** It is a list of questions whose answers only the user can give, plus the
measured ground any answer will have to stand on. Nothing here proposes a mechanism, and nothing here
is written around the container-take duplication — that bug becomes **one requirement inside** the
answers, not the subject.

**Why this brief exists instead of a design.** A rollback fix was designed across several rounds
*into* `player_inventory_sync`, on the assumption it was a working substrate with a coverage gap.
The author's report (`votv-player-inventory-two-layer-RE-2026-07-24.md` §6.1) and the measured
topology below both point at the subsystem itself being the question. Designing a correction into a
store whose required behaviour has never been stated is the wrong altitude, and the questions below
are the missing statement.

---

## 1. The measured ground (any answer builds on this)

From `votv-player-inventory-two-layer-RE-2026-07-24.md` — all bytecode/code-measured, call targets
resolved through `Imports[]`:

- **The player's items live in `UpropInventory_C` → `saveSlot.GObjStack[propInventory.Index]`.** That
  is the live store the game mutates during play.
- **`saveSlot.inventoryData` is a separate field and a save-side projection.**
  `mainGamemode::saveObjects` is the **only measured live→save writer** (writes `inventoryData` while
  reading `propInventory`/`GObjStack`).
- **The live item flow never touches `inventoryData`** — zero references across `prop_container`,
  `prop_inventoryContainer_player`, `uicomp_playerInvContainerSlot`, `ui_playerInventory`. A container
  slot press resolves to `getObject → addObject → K2_DestroyActor`.
- **A client's native save cycle is switched OFF at the source** — `save_block` sets `disableSave=true`
  on the gamemode; the `SaveGameToSlot` detour is only a backstop and has **never fired in any logged
  session**. (Corrected 2026-07-24: this line previously named the detour as the mechanism.)
- **Our lane polls the projection**, not the live store (`ue_wrap::inventory::ReadAll` over
  `inventoryData`/`equipment`/`hold`).
- **NEW 2026-07-24 — `GObjStack[0]` IS the player's inventory, by construction.** The player
  container's component template bakes `index = 0`, `player = True`, `customVolume = 50000`; the base
  container's template has no overrides at all (so world containers are `player = false` and take
  their index from the save). This answers the standing "what is `GObjStack[idx=0]`" question
  outright, and explains why the player container's `loadData` override is an empty stub — the slot
  is born, not restored (RE §5.4).
- **NEW 2026-07-24 — `inventoryData` has NO gameplay reader anywhere in the cooked game.** Complete
  census over all 3050 packages (RE §4.1): it is named in three packages only. Writers =
  `saveObjects` (the projection copy) and `putObjectInventory` (dead, zero callers). Clears = three
  `saveSlot::reset_*`. The single reader = `ui_saveSlots::regenSave`, the save-slot menu's *repair*
  tool. **Nothing ever reads `inventoryData` back into the live store**, on either peer.
  Positive control for that negative: the same census finds ~10 gameplay touchers for the sibling
  fields `equipment`/`hold` (`AddEquipment`, `RemoveEquipment`, `updateEquipment`, `processArmor`,
  `addEquip`, `updateHold`, `checkEquip`, `saveHoldObj`, both ubergraphs).
- **Consequence:** the lane's `equipment` and `hold` thirds address fields the game genuinely reads;
  its `inventory` third addresses a field the game only ever writes. Whatever is written into
  `inventoryData` at join-apply cannot become a player's carried items by any measured path.
- **The engine addresses inventory records by INDEX and by live ACTOR, never by key** — SDK
  signatures: `takeObj(int32 Index, …, Fstruct_save& Output, AActor*& Object)`,
  `addObject(AActor* Actor, int32 insertIndex1, …)`, `getObj(int32 Index, …)`.

## 2. The size of what is proven to work

Stated precisely because it sets the bar a replacement would have to clear — and it is much smaller
than "the lane works":

- **Join-time apply**: `ApplyToSaveObject` writes the registered save object before the world
  materializes; the first-join starter kit arrives (its persisted blob decodes to
  `prop_equipment_compass_C` + `prop_equipment_flashlight_C`).
- **`equip=8 hold=2` survives the round trip.**

**Both of these rest on TWO independent samples with ZERO variation between them.** That shows the
lane *carries* those arrays; it does **not** show the lane tracks a change. So this is not a verified
contract — it is the only thing anyone has ever checked, and therefore the only thing currently known
about the lane in either direction.

## 3. The open items (unchanged, with the kind of work each needs)

| # | Item | Needs |
|---|---|---|
| 1 | Does `saveObjects` run on a client? | **CLOSED 2026-07-24** — answered by mechanism: `save_block` sets `disableSave=true` on the gamemode, so the save cycle that would call it is off. Called manually, `saveObjects` DOES refresh the projection on both peers (host `inv 4→5`, client `0→6`) |
| 2 | Who calls `putObjectInventory` (writes `inventoryData` ×6, zero `GObjStack`)? | **CLOSED 2026-07-24 — NOBODY.** Zero callers across all 3050 cooked packages; the 24 apparent hits are all the near-twin `putObjectInventory2`, a different function on `mainPlayer` that writes `GObjStack`. It is dead legacy code (RE §4.1, §4.2, §5.1) |
| 3 | What does `loadObjects` populate, and from what? | **CLOSED 2026-07-24** — a container's `loadData` restores only `propInventory.index` (contents ride in `GObjStack` with the save); the player's own container overrides `loadData`/`getData` with **empty stubs that do not call the parent**, so it is outside the per-actor save path entirely (RE §5.4) |
| 4 | The instrument's `player=BLIND(READ-FAILED)` branch has never fired | **CLOSED 2026-07-24** — unreachable by construction: `CountItemInstances` and `INV::ReadAll` gate on the SAME `ResolveSaveSlot()` in one GT task, and `ReadAll` has exactly two returns (offsets are constants). Every historical `player=0` printed with a scan summary already meant "read OK, found zero" |
| 5 | **`join-apply` and `world load` are not content-consistent at t=0** — see §3a | **RUN WITHDRAWN 2026-07-24** — the gap is measured against a field the game never reads, so sizing it buys nothing. The real content of the item folds into Q1/Q4 (§3a) |

### 3a. The origin mismatch — a SEPARATE item, not a restatement of staleness

Measured 2026-07-24: at session start a client's projection held **0** rows (our join-apply wrote
`inventory=0`) while its live store held **5** records built by the world load — before any play, before
any take. The sixth appeared only when the bot took an item.

So the bulk of the live-vs-projection difference is **present at t=0**. It is an inconsistency between
**what our join-apply writes into the projection** and **what the world load builds into the live
store** — *not* a projection going stale over time. Those are different defects with different fixes,
and the earlier framing of this document ("the projection drifts") merged them.

Consequence for the questions below: refreshing the projection more often would not fix an origin
mismatch. Whether anything is fixable by projection updates at all depends on Q5.

**Unmeasured:** whether the difference then GROWS with session length (monotonic) or stays at its
origin value, and whether a reconnect / world change / manual save bounds it. That decides "bug"
versus "bounded window."

**Reclassified 2026-07-24 — this is no longer a defect to size, it is a symptom to stop measuring.**
The mismatch is between two views of the same items, and the census now shows one of those views
(`inventoryData`) is never read by the game. A gap against an unread field has no player-visible
consequence *through that field*; whether it grows is therefore not worth a run. What the item was
really pointing at — that our join-apply and the world load disagree about what a player has — stays
open, but it belongs to Q1/Q4, not to a projection-drift measurement. The carried run about the
host's `gap=1` is withdrawn for the same reason.

### 3b. The lane treats three fields as one class — and only two of them are addressable

> **PRE-CHANGE BASELINE, captured 2026-07-24, first on DLL `88dbcbb5e7efe13f`** and REPRODUCED
> unchanged on `6f2add8d6fae9b45` and `f274b25814a83429` (proto 125 throughout, `s_1234`,
> `live_store_readout=1`, five autonomous LAN smokes). Pin the build, because "before" and "after"
> must diverge by the CHANGE and not by the binary — and note the reproduction across three builds
> is what makes this a baseline rather than one sample. Current deployed = `f274b25814a83429`.
>
> | peer | reading |
> |---|---|
> | host | `GObjStack[0] live=4 proj=4 gap=+0`, both diffs empty |
> | client | `GObjStack[0] live=4 proj=0 gap=+4` -> CHANGE `live=5 proj=0 gap=+5` |
>
> The client's `proj=0` is produced by our own join-apply writing `inventoryData`. Retiring the
> `inventory` third is expected to CHANGE that number, so this table is the control it will be
> compared against. Raw lines are in both peers' `multivoid.log` from 15:47 and 15:55.

Recorded as a classification, not a proposal. `player_inventory_sync` reads, wires, persists and
applies `inventory` / `equipment` / `hold` **uniformly**, as three arrays of the same kind. The
census says they are not the same kind:

| field | game reads it? | evidence |
|---|---|---|
| `equipment` | **yes** | `AddEquipment`, `RemoveEquipment`, `updateEquipment`, `processArmor`, both ubergraphs |
| `hold` | **yes** | `saveHoldObj`, `addEquip`, `updateHold`, `checkEquip`, both ubergraphs |
| `inventory` (`inventoryData`) | **no** | writers + reset-clears + a save-repair menu tool only (§1) |

So this is a **boundary, not a defect**: two thirds of the lane address fields the game consults, and
one third addresses a field it never reads back. The uniform treatment is the original assumption,
and it is the thing that is wrong — which means whatever comes next is a *separation* of two
mechanisms with different substrates, not a repair of one.

That has a consequence worth stating plainly, because it changes the verify-before-retire cost in
§5: the `equipment`/`hold` behaviour recorded in §2 is carried by the two-thirds that DO address
readable fields, so retiring or replacing the `inventory` third does not put the demonstrated
behaviour at risk. Which of the two mechanisms gets built, and how much it guarantees, stays gated
on Q1-Q4.

---

## 4. The questions — these are yours, not engineering calls

> **STATUS 2026-07-24 — Q1/Q2/Q3 ANSWERED by the user (below, verbatim intent). Q4 is NOT
> answered: it is being held as a HYPOTHESIS pending a measurement, deliberately, so it is not
> recorded as a decision here. Q5 was withdrawn (answered by measurement).**
>
> - **Q1 reconnect** — the player sees what they had at the moment of disconnect. Source = the LIVE
>   store `GObjStack[0]` host-side, not the projection.
> - **Q2 host restart** — survives as of the host's LAST SAVE, no later. No separate disk path is
>   invented for personal inventories: `saveObjects` already rewrites the projection from the live
>   store at save time, so the host's save IS the consistency point. `<guid>.json` is a
>   within-session reconnect CACHE, not a second source of truth.
> - **Q3 item in hand at disconnect** — stays with the player. It does NOT drop into the world:
>   `hold` already travels on the wire, whereas dropping would need an actor spawn with position and
>   owner — a new path and a new race class for no gain.
> - **Q4** — held as a hypothesis (intra-session name `class|save-key`, position across session
>   boundaries), NOT written up, pending the key measurement at the transfer. See the note below Q4.

### Q1. What must a player SEE after reconnecting?
Everything they were carrying at disconnect? Only worn equipment? A defined subset that survives, with
the rest explicitly not guaranteed? "Whatever the host's world save happened to contain" is also a
possible answer, and it is a different product than the other three.

### Q2. What must survive a HOST restart?
Per-player inventory is persisted host-side per GUID. Should a returning player get back what they
had, and if so, as of **when** — their disconnect, the host's last world save, or their last
observed state? These diverge in practice, and the answer decides whether anything must be captured
at disconnect at all.

### Q3. What happens to an item IN HAND at disconnect?
Held items are a distinct store (`hold`) from carried ones. Does a held item come back held, come
back in the inventory, drop into the world at the disconnect position, or vanish? Each is defensible;
they are not the same product.

### Q4. **The question never asked in the entire investigation: what does "the same item" MEAN between two peers?**

The engine addresses inventory records **positionally (`Index`) and by live actor** — never by a
name. So there is no engine-supplied answer to "peer A's item X and peer B's item X are the same
object." Everything downstream hangs on which answer you want:

- If **identity is nominal** (an item is a durable named thing that two peers can both refer to), then
  custody, transfer and rolling back a race loser are all expressible — but the naming is **ours** to
  invent and maintain, and the project already has a lesson that key uniqueness is our invariant, not
  the game's (VOTV's own save ships duplicate keys; the host re-keys at enrol, and that re-key was
  inert for an entire take once).
- If **identity is positional/local** (an item is just a record in someone's array), then "the same
  item on two peers" is not a well-formed statement, and a rollback cannot name what to remove — a
  concurrent-take duplication would have to be prevented at the transfer rather than corrected after.

This is a product decision because it decides what the game promises about objects, not just how the
code is arranged. It also gates the duplication work: the fix shape differs completely between the
two, and neither can be designed before it is answered.

#### Q4 — ANSWERED 2026-07-24 (user), on the measurement below and not on the retracted framing

**Answer: the name holds WITHIN a session; position across session boundaries. The duplication is
prevented by serialising the transfer on the SOURCE RECORD's key. Cross-session custody is not
built, because there is no requirement for it.**

The user first gave this answer from a premise the primary had mis-generalised, then explicitly
DOWNGRADED it to a hypothesis and required the load-bearing half to be verified separately rather
than inherited. It was, statically (RE §3.2b): the source record and its key sit in the container's
`GObjStack` slot **before any spawn happens**, so two peers contending over a take are contending
over an entity that already carries a stable name; the freshly-minted carrier key is downstream of
the contention, not part of it. Q4 was then closed on that measurement.

The retracted reasoning is kept below so it is not re-derived.

#### The measurement trail behind it (both of the primary's over-claims withdrawn)

The user gave an answer (intra-session name, position across session boundaries) and then explicitly
DOWNGRADED it to a hypothesis when the observation it rested on turned out to be mis-generalised by
the primary. Recorded here so the reasoning is not re-inherited as settled:

- **What is measured:** in two runs of the same save, the four save-loaded personal items had
  byte-identical `class|save-key` values, and the ONE item that passed through a container `extract`
  had a DIFFERENT key in each run.
- **What that does NOT show (both directions were over-claimed and are withdrawn):** it does not show
  a key "is reborn at each world load" (falsified — the four were stable), and it does not show a key
  "survives a restart" either. Both runs load the SAME save FILE, so this demonstrates a
  deterministic READ of a stored field. Survival across a save/restart cycle needs
  load -> save -> reload -> compare, which has not been done.
- **The open question, correctly framed:** is the SOURCE record's key (the one sitting in the
  container, which is what two peers would contend over) stable at dispute time? That quantity has
  never been measured; it was asserted as support for the conclusion while simultaneously being
  proposed for measurement.
- **The transfer half is now MEASURED (2026-07-24, static -- RE doc SS3.2b), and it was not a log
  line.** `Aprop_container_C::extract` decoded statement-by-statement: the deferred-spawn window
  contains NO property application (only pawn-transform math + the two spawn calls, taking just the
  CLASS off the source record); `putObjectInventory2` -> `addObject` -> **`getData`** captures the
  freshly-spawned carrier; `K2_DestroyActor` follows; and `loadData(takeObj_Output)` runs only
  AFTER both. So: **mint at spawn, captured at add, restored too late.**
- **Consequence FOR Q4, stated as a fact and not as a decision:** the SOURCE record and its key exist
  in the container's `GObjStack` slot BEFORE any spawn, so two peers contending over a take are
  contending over an entity that already carries a stable name. The freshly-minted destination key
  is downstream of the contention, not part of it. That is the separate proposition the user
  correctly refused to inherit -- it is now measured rather than assumed. **Whether Q4 closes on it
  remains the user's call; this brief does not record Q4 as decided.**
- **Sample size:** ONE distinct transfer (the same scripted `extract(0)` of the same item from the
  same container), observed twice. Not a class.

### ~~Q5. Which store is the per-player source of truth?~~ — WITHDRAWN 2026-07-24, answered by measurement

**This is no longer a question for you, and leaving it in the list would be asking you to choose
between a live option and a dead one.**

Q5 was posed as a real fork with a stated cost on each side: capture per-player state from the
**live** store, or keep the **save-side projection** and accept the client's save-block behaviour.
The census in §1 removes the second branch outright — `inventoryData` has no gameplay reader, so a
value placed there is never read back into anything a player can carry. That is not a trade-off with
a cost; it is a store the game does not consult.

Carried items therefore have to come from the live store (`propInventory`/`GObjStack`) if they are
to exist at all. **Nothing is decided by that except which store gets read** — how much is
guaranteed is Q1/Q2/Q3, and whether it is expressible across two peers at all is Q4.

**So what is actually owed by you is Q1-Q4. Q5 is closed and its engineering residue is mine.**

**This is the product half of a fork whose other half is mine, and they must not be asked as one
question.** Splitting them explicitly:

- **PRODUCT (yours, Q5 above):** which store is the source of truth for per-player state.
- **ENGINEERING (mine, not a question for you):** *who writes the projection on each peer.* Measured:
  the host's is populated by the game's own save/load cycle; the client's by our join-apply, which
  wrote `inventory=0` while the world load built 5 live records. Different authors, different content —
  so "refresh the projection more often" can only ever help the peer whose projection has the right
  author, and on the client `saveSlot_C::save` returns before the refresh anyway (RE doc §5.2). I owe
  the resolution of that, not a question about it. **Narrowed 2026-07-24:** with no reader for
  `inventoryData`, "refresh the projection more often" is not merely weak — it is not a candidate at
  all, on either peer. The engineering I owe is against the live store.

The earlier phrasing of this brief merged the two into one line; that merge is what made "feed the
lane" look like a candidate fix when it is not one.

---

## 4b. HOOK — the fidelity axis, deferred behind arc 1 (user decision 2026-07-24)

**Not "deferred because it is hard" — NOT ESTABLISHED.** The claim that a container -> inventory
take loses the item's saved payload was predicted from call ordering and then **falsified**: the
taken record printed `{b5,f3,nm2}`, not empty (RE §3.2b). The weaker form — do the saved VALUES
survive, as opposed to the class-shaped slots — is still open, and the readout cannot answer it,
because group counts are a class fingerprint.

- **The instrument that would settle it** (real, not a log line): read the SOURCE container slot too
  and compare a record's values before vs after a take. Best subject: **`prop_drive_C`**, the one
  class whose saved payload carries something a fresh spawn should not have (`sig1`).
- **Why it sits behind arc 1:** it blocks neither the transfer transaction nor the lane split, and
  the rule-1 question it would raise — replicate the vanilla defect faithfully, or correct it —
  **does not arise until a defect is established**.
- **If it turns out real**, it is its own axis, not arc 1's: serialising the transfer fixes the
  COUNT (two peers, one winner) and would leave the CONTENT wrong (a winner holding an emptied
  item). Arc 1 must not be widened to cover it on speculation.

## 5. What is deliberately NOT in this brief

- **No mechanism, no lane design, no wire format.** Those follow the answers; several were drafted
  during the investigation and every one rested on a premise that later moved.
- **No rewrite proposal for `player_inventory_sync`.** Whether it is replaced (RULE 2, deleted in the
  same commit) or kept depends on Q1-Q5. Note the verify-before-retire rule: the small surface in §2
  is the only demonstrated behaviour, so it is also the only thing a replacement must be **proven** to
  reproduce before the old path goes.
- **Nothing pre-written for the duplication.** It reproduces (`sum=2`), it is real, and it stays open;
  it enters as a requirement under Q4 and Q5 rather than as a topic.

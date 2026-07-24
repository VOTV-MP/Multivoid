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
- **Client world-saves are blocked** at the `SaveGameToSlot` seam (`coop/save/save_block.cpp`).
- **Our lane polls the projection**, not the live store (`ue_wrap::inventory::ReadAll` over
  `inventoryData`/`equipment`/`hold`).
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
| 1 | Does `saveObjects` still run on a client whose `SaveGameToSlot` is blocked (i.e. does `inventoryData` refresh in memory upstream of the block)? | **RUN** — two-peer; a client only exists inside a session |
| 2 | Who calls `putObjectInventory` (writes `inventoryData` ×6, zero `GObjStack`; absent from all four UI/container assets)? | **READ** — dump more packages with kismet-analyzer |
| 3 | What does `loadObjects` populate, and from what? It dispatches via `loadData` ×3 — the earlier "0 references" was a false negative | **READ** — resolve `loadData` on the loading classes |
| 4 | The instrument's `player=BLIND(READ-FAILED)` branch has never fired | **RUN** — solo, ~1 min, boot-time before `saveSlot` resolves |

---

## 4. The questions — these are yours, not engineering calls

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

### Q5. Which store is the per-player source of truth — the live one or the save-side one?
Today the lane reads the projection. That is a design choice with consequences a player can feel
(§1 + §6.1 of the RE doc). Should per-player state be captured from the **live** store, accepting the
cost of doing so, or should it remain the save-side projection with the client's save-block behaviour
made explicit and accepted?

---

## 5. What is deliberately NOT in this brief

- **No mechanism, no lane design, no wire format.** Those follow the answers; several were drafted
  during the investigation and every one rested on a premise that later moved.
- **No rewrite proposal for `player_inventory_sync`.** Whether it is replaced (RULE 2, deleted in the
  same commit) or kept depends on Q1-Q5. Note the verify-before-retire rule: the small surface in §2
  is the only demonstrated behaviour, so it is also the only thing a replacement must be **proven** to
  reproduce before the old path goes.
- **Nothing pre-written for the duplication.** It reproduces (`sum=2`), it is real, and it stays open;
  it enters as a requirement under Q4 and Q5 rather than as a topic.

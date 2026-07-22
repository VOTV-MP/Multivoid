# DESIGN: R11b -- a client's container extraction, and who owns a cross-boundary transfer

**Date:** 2026-07-22 (evening) · **Status:** BUILT + smoke-green both directions, **NOT hands-on** ·
**Commits:** `411743af` (lane, proto 124->125) + `163fc974` (instrument) · **DLL** `f79eb2ce86cdc46e` x4

Point-in-time DESIGN, not durable RE. Method: a 6-round `/qf` in which the USER drove the critic, plus
the 2026-07-22 17:29 hands-on logs and four instrumented smokes.

Confidence tags: `[V]` measured this session (citation inline) · `[RD]` RE-derived · `[?]` unverified.

---

## 0. What the user actually asked

> "протестил, теперь клиент видит бургеры, и клиент специально взял один бургер из двух к себе в
> инвентарь, затем хост заглянул в sack и там было 2 бургера из двух, затем хост взял все бургеры и
> клиент заглянул - в sack не было ни одного бургера. **Что-то не так работает и требует должного
> подхода.**"

The symptom in the user's own words: the same burger existed twice. "Должный подход" invokes RULE 1.

**How the shipped design answers it, in one line:** the client takes one of two burgers, the container
now loses that record on the HOST as well, so the host looking in the sack sees one -- and three burgers
cannot come from an order of two.

---

## 1. The reframes -- this design moved twice before it settled

Recording these because both were killed by a measurement the critic demanded, and both would otherwise
look like the obvious answer to the next reader.

**Reframe 1 (proposed, then self-refuted).** "The client's take is not invisible; it already crosses the
wire into a different store -- so this is two lanes on one move." `[V]` `player_inventory_sync` really
did fire at the take (client `17:29:33 streamed inventory blob (1240 bytes)`, was 1053; host
`17:29:33 flushed slot 1 ... (1240-byte blob) to disk`).

**Why it was wrong.** `player_inventory_sync` reads `saveSlot.inventoryData` / `equipment` / `hold`
(`inventory.cpp:89,96,103`) -- **different properties from `saveSlot.GObjStack`** `[V]`, and demonstrably
not mirrors of it (at join the blob carried `inventory=0 equip=8 hold=2` while `GObjStack[0]` held 3
records). So the blob caught the move that came NEXT -- container-inventory -> hand -- not the take. The
real chain is three stores:

| moment | movement | carried by |
|---|---|---|
| 17:29:31 | world container `[1]` -> the client's own container-inventory `[0]` | **NOBODY** |
| 17:29:33 | `[0]` -> hand | `player_inventory_sync` (blob +187 B) |

`[?]` That the +187 B is the same burger landing in `hold` is INFERENCE, not measurement: the stream log
prints only `inv.inventory.size()`. Settling it costs one log line.

**Reframe 2 (proposed, then self-refuted).** "So build both halves at once -- the client authors slot 0
too, and the host parses the personal blob." Killed on a measurement: the host's `[1]` did not change
between `17:29:25` and its own take at `17:29:39` `[V]`, i.e. **the container decrement never reaches the
host at all**. "Open the envelope" would therefore give the host a `+1` in the client's inventory with no
matching `-1` -- a phantom gain indistinguishable from a legitimate pickup, not a detected dupe. There is
no cheap detection step ahead of the lane.

**What survived both.** The observed dupe lives ENTIRELY in slot 1, a world container. The player's own
container (`Aprop_inventoryContainer_player_C : Aprop_container_C`, slot 0) sits in the SAME global
`GObjStack`, is excluded fail-closed by BOUNDARY 1, and is read by no lane -- an unsynced store inside a
synced array. That is the SECOND story, deferred with a hook.

---

## 2. Why an intent lane cannot exist here

`takeObj` dispatches `EX_LocalVirtualFunction` `[V]` (bytecode census, the RE doc §5). By the time our
0x45 bracket sees it, the item has already materialised on the presser. A request the host could refuse
is not buildable; the only available shape is **presser-authored state**
([[lesson-presser-authored-state-not-intent-for-invisible-verbs]]). Candidates (A) "client sends an
extraction intent" and (C) "deny the client the local action" were both retired on this.

A corollary measured the same session: `grab_hook[takeObj POST]` fired **zero times on both peers** while
registered on both `[V]`, agreeing with the census finding that no cooked asset calls `takeObj` through
ProcessEvent. The "partial PE-visible route" recorded in `COOP_DISPATCH_VISIBILITY` row 88 was carried
framing; it is downgraded there, with its own retirement check owed.

---

## 3. The user's decision -- inventory as canon, rollback deferred

Handed to the user as a plain-text fork, because it is a question about the game, not about code. Their
ruling, verbatim in substance:

- **privacy is not a blocker** (coop with friends; the host owns the world anyway);
- treat the fork not as "with rollback or without" but as **"is the inventory canon or not" -- and the
  answer is YES**, direction = the arbiter model;
- **do not build the rollback now.** First build is indistinguishable from the no-rollback option, and it
  is what MEASURES whether conflicts ever occur. "У нас двое-трое игроков, не сотня; окно конфликта может
  быть настолько редким, что откат — код, которого нет, ради ситуации, которой нет."

This matters for the arbiter arc beyond R11b: `player_inventory_sync` is an opaque-blob custody
arrangement -- the host stores `coop_players/<guid>.json` and **never calls `inventory_wire::Deserialize`
on the receive path** `[V]`. By [[lesson-opaque-blob-custody-donor-dictates-the-remainder]] the client
therefore authors 100% of its own inventory today. An arbiter cannot arbitrate a store it cannot read
(`COOP_SERVER_MODEL.md` §4), so making the inventory readable canon is a PRECONDITION of the arbiter
phase, not a preference.

---

## 4. AS-BUILT

**Wire.** `ReliableKind::ContainerContents = 118`, blob `[u8 op=0][u32 eid][u64 baseHash][u16 n]` + n
records in the `save_record_wire` grammar. Proto 124 -> **125** (bidirectional acceptance + a longer
blob are both wire-visible).

**Authoring.** `OnVerbEntry` has NO role gate. Its FIRST statement -- ahead of every filter -- is the
empirical-gate log, because "all N verb(s) resolved" proves an FName resolved, not that a callback runs
([[lesson-late-registrant-inert-after-all-resolved-latch]] cost two RED takes on exactly that).

**Arbitration.** A client slice declares the baseHash it edited from. The host accepts only if that still
equals what it PUBLISHED for the eid and no host-side change is in flight; otherwise it refuses,
re-publishes its own truth to that author, and counts the refusal. Without the compare, a stale
full-slice write would silently erase a host addition the author had not yet seen -- swapping the old
loss class for a new one.

**Relay** excludes the author (the eaten-scroll race).

**Four maps, deliberately not fewer.** Fusing them produced two separate failures, one per smoke:

| map | question | on a local edit |
|---|---|---|
| `g_publishedHash` | what did the host tell peers the world is? (the CAS baseline) | n/a (host) |
| `g_sentHash` | may the host skip the next fan-out? | n/a |
| `g_appliedHash` | does an incoming blob change anything for me? | **CLEARED** |
| `g_baseHash` | which host truth was I editing? | **KEPT** |

**Co-requisite fixed in the same commit.** `R::FindFunction` matches `Outer == owningClass` EXACTLY and
does not walk the superclass chain (`reflection.cpp:427`) `[V]`. `updateVolumesAndMass` is declared only
on `Aprop_container_C` (SDK `prop_container.hpp:32`) while every real container is a subclass, so the
re-derive resolved `nullptr` for every container and **had never once run** -- silently. Now resolved
from the declaring class, and the resolution is logged including failure.

---

## 5. Evidence, and its limits

Smoke, both directions, zero conflicts, digests identical cross-peer (see the take-4 doc's R11b section
for the log block). The instrument (`[dev] container_selftest=1`) dispatches `prop_container_C::extract(0)`
so the mutation the lane must catch is the game's own inner `takeObj`, never our own ProcessEvent call.

**What the smoke does NOT establish:**
- **`currVol` convergence is weaker than it looks.** The numbers matched (28579.0 both peers), but they
  matched partly because NEITHER peer recomputed the volume on extraction. The user's original symptom
  (686 vs 0.0) is not reproduced by this instrument and needs the hands-on.
- **VERIFIED is not earned.** Smoke = AS-BUILT. The ladder needs the user's three-step scenario.
- Simultaneous grab, `putObjectIn_overlap`, nested containers, slot 0 -- all untouched.

---

## 6. NEXT

1. **HANDS-ON (proto 125, RELAUNCH BOTH PEERS)** -- the runbook is
   `research/handson_runbook_2026-07-22_container_v125.md`.
2. `container_contents_sync.cpp` is **832 LOC**, over the 800 soft cap -- extraction proposal owed at the
   next touch.
3. **Separate finding, own audit:** `reflection.cpp`'s no-superclass-walk behaviour across the 19
   `ClassOf(instance)` call sites and the sites naming a BP class for an inherited function
   (`door_probe.cpp:81` `SetActorTickEnabled` is a second confirmed-inert candidate). Possible RED in
   lanes currently labelled verified. See [[lesson-findfunction-does-not-walk-the-superclass-chain]].
4. The SECOND story (slot 0 + blob canon + BOUNDARY 1) -- design owed, gated on the user's
   inventory-as-canon ruling above.

# RETIRED — `COOP_SYNCER_MODEL.md` §5 (the enforcement point) and §8 (the migration stages)

**Cut 2026-09-04 under RULE 2**, after `/qf` pass 1 round 2. Both sections were already
described by the live document as *"superseded in shape"* by its own §4b, which meant a reader
had to reach §4b to learn that §5 — three sections earlier — was wrong. That is the accretion
shape this project keeps paying for, and RULE 2 says retired content goes, fully.

**What replaced them:** §4b's corrected direction, plus R2a's falsification and R2c (the axis is
the ELEMENT, not the packet kind) added the same day.

**Why they are kept here at all:** §5's count of the dispatch surface and §8's staging risks were
the reasoning trail for decisions still live in the parent doc. Nothing here is current. Do not
cite it as design; cite `COOP_SYNCER_MODEL.md` §4b.

**Known-wrong in the text below, measured 2026-09-04:** §5's "68 case branches across three
files" is 100 across five (`event_dispatch_world.cpp` and `event_dispatch_intent.cpp` were never
named); §5's per-kind `bAllowClientAuthored` flag chooses an axis MTA does not use (R2c); §8's
stages 3-4 build on that flag.

---

## 5. The enforcement point

`[corr 2026-09-04: re-counted. The number moved AND the surface is bigger than the three files this
section names — a running total in a doc, exactly `[[lesson-a-running-total-in-an-append-only-register]]`.]`

`[V]` The dispatch surface is **100 `case` branches across FIVE files**, not 68 across three:

| file | cases (2026-07-20) | cases (2026-09-04) |
|---|---|---|
| `event_dispatch_state.cpp` | 24 | **26** |
| `event_dispatch_signal.cpp` | 29 | 29 |
| `event_dispatch_entity.cpp` | 15 | 15 |
| **`event_dispatch_world.cpp`** | *not named* | **16** |
| **`event_dispatch_intent.cpp`** | *not named* | **14** |

**`event_dispatch_intent.cpp` is the one that matters to this design**: it is the act-as-host
lane's own dispatcher — the mechanism §2b describes — and it did not exist in this section's
frame. So "one check in the dispatch switch" is today a check in *three of five* switches, and
the enforcement point must name all five or it is a site list wearing an invariant's clothes.
Auditing 100 handlers by hand guarantees a miss and creates 100 places to keep in sync.

MTA's answer (`MTA_PRECEDENT.md` §5) is a **default-deny per-kind flag** checked **once** in the
dispatch switch, before the handler runs:

1. **Per-kind `bAllowClientAuthored`, defaulting to `false`.** A kind that a client may legitimately
   author must say so explicitly. Everything else is refused without the handler ever seeing it.
2. **For kinds that ARE client-authorable: the syncer check**, at the same single point —
   `SyncerOf(elementId) == senderSlot`.
3. **A generation counter beside the check** — MTA's `CanUpdateSync(ucTimeContext)`, bumped whenever
   the arbiter itself changes the element, so a stale-but-authorized packet cannot be replayed.

This mirrors what `session_lanes.h:181-185` already does for **relaying**. The same idea applied to
**applying** is the missing half — and note the ordering rule from `RULES.md` S2: validate *before*
relay. MTA itself gets this wrong (`MTA_PRECEDENT.md` §2 caution); we should not copy that.

---


---

## 8. Migration — staged, because this touches lanes the user has hands-on tested

**Do not big-bang this.** Ordered so that behaviour-changing steps land last and observably.

| Stage | What | Risk |
|---|---|---|
| **0** | Write the arbiter abstraction + the `SyncerOf`/`AssignSyncer` API. No callers. | None |
| **1** | Arbiter becomes the **single writer** of syncer state; assignments broadcast. **No enforcement** — pure observation. Verify by log diff that assignments match today's implicit behaviour. | Low — no behaviour change |
| **2** | Principle-8 answers (§7): reap on departure, seed on join, TTL, race rule. Still no enforcement. Smoke: a peer leaving mid-desk-use frees the desk. | Low |
| **3** | Per-kind `bAllowClientAuthored` flag, **defaulting to false**, one check in the dispatch switch. Enumerate the 68 kinds and mark the legitimately client-authored ones. | **Medium — this is where a missed `true` breaks real play** |
| **4** | Syncer check on the client-authorable kinds + generation counter. Family by family, smoke between. Start with the desk chain, where the claim machinery is most mature. | **Highest** |
| **5** | Takeover request/grant (§6) + the explicit refusal reply. | Medium |
| **6** | RULE 2: retire the advisory reads of `device_occupancy` that stage 4 makes redundant. | Low |

Stages 3-4 are a wire-format change → **`kProtocolVersion` bump**, per the standing rule.

---


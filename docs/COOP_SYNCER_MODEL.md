# The syncer model — per-element authority, MTA-shape

**Status: DESIGN, 2026-07-20. Not built. Not yet `/qf`-ed.** User directive: introduce the syncer
model into the mod's architecture and begin the full transition to the MTA model.

This is the single largest architectural change since the coop layer was built. It is authorized
under RULE 1 as a hard architectural change.

---

## 1. Why now — three roads turn out to be one road

The directive's own justification is correct, and worth stating precisely because it changes the
cost calculus:

| Road | What it needs | 
|---|---|
| **Security A3/A4** | Something on the receive path that answers *"is this peer allowed to change this element?"* |
| **Dedicated server (ROADMAP phase 8)** | *"authority INVERSION to the true MTA shape — the server holds state + rules + arbitration, clients simulate the world with per-element syncers"* |
| **The MTA migration** (standing rule 2026-05-28) | The syncer model is MTA's core abstraction; see `docs/security/MTA_PRECEDENT.md` §1-§3 |

**These are the same mechanism.** Building it once for security and again for the dedicated server
would be building it twice. The syncer model is the abstraction that survives the inversion: today
the **host process** runs the arbiter; in phase 8 the **standalone server** runs the same arbiter
against the same element table. Phase 8 stops being a rewrite and becomes *"move the arbiter out of
the game process"*.

That is the strongest available argument for doing it now rather than after the release-gate work —
it is not extra scope, it is the same scope spent once.

---

## 1b. Ordering — architecture FIRST, then security (USER DECISION 2026-07-20)

> *"Стоит архитектуру mta и авторитет модели, синкеры и тд перенять сначала, А ЗАТЕМ УЖЕ ВОПРОСАМИ
> БЕЗОПАСНОСТИ ЗАНИМАТЬСЯ."*

**The security findings that are authority-shaped are DOWNSTREAM of this document, not parallel to
it.** `TRACKER` **A3**, **A4** and **A5** are not bugs to be patched — they are the absence of this
architecture. Patching them first would build a mechanism this migration then replaces, which is
migration baggage (RULE 2). `docs/security/PLAN_03_AUTHORITY.md` is therefore reduced to the
*security framing* of this work; the mechanism lives here.

**The carve-out, and it is narrow:** findings that live at the **parse layer**, below the authority
question, are untouched by this migration and are not held behind it — **W4** (`blob_chunks`
amplification), **W5** (65 536 spawns per peer), **W6** (no role gate, NaN into `SetActorLocation`).
They fire before "who owns this element?" is ever asked, they are hours of work on already-verified
sites, and nothing here rebuilds them.

**P1 (peer auth) stays orthogonal.** A syncer assignment is only as trustworthy as the identity it
names, so P1 raises this model's ceiling — but the model is worth building before P1 lands, because
it is the architecture either way. See §9 question 5.

## 2. The model

Three concepts, borrowed wholesale (`MTA_PRECEDENT.md` §1):

- **Element** — a synced thing with a stable id. We already have this (`coop/element/`, `ElementId`).
- **Syncer** — the ONE peer currently authorized to author changes for that element. Assigned by the
  arbiter, *pushed* to the peer, never claimed by it.
- **Arbiter** — the authority that assigns syncers and validates inbound writes. Today: the host
  process. Phase 8: the standalone server. **The code must not care which.**

The invariant, and everything below is a consequence of it:

> **An inbound write is applied only if its sender is the element's current syncer, as recorded by
> the arbiter. Authority is assigned, never asserted.**

---

## 3. What we already have (measured 2026-07-20)

Encouraging: most of the primitives exist. This is a promotion, not a green field.

| Piece | Where | State |
|---|---|---|
| Stable element identity | `coop/element/`, `ElementId` | **Built** |
| A per-element owner field | `element.h:201-202` `GetOwnerSlot`/`SetOwnerSlot` | **Built — but WRONG SEMANTICS, see §4** |
| Per-slot teardown by owner | `mirror_manager.h:357` `DrainMirrorsForSlot` | **Built** |
| A holder table with a lookup | `device_occupancy.h:72` `HolderOf(key)` | **Built, but ADVISORY** — read by senders, never on receive |
| Leaver teardown for claims | `device_occupancy.h:79` `OnDisconnectForSlot` | **Built** — a large part of the Principle-8 answer already exists |
| Join-time claim seeding | `device_occupancy.h:75` `QueueConnectBroadcastForSlot` | **Built** |
| A correct receive-side check, at ONE site | `trash_grab_intent.cpp:298-302` | **Built** — this is the pattern to generalize |
| Sender identity bound to transport | `VerifySenderEidRange`, `HandleAssignPeerSlot` | **Built** (per the audit's "checked and clean") |

**What is missing is not the data. It is the enforcement point and a coherent notion of who assigns.**

---

## 4. The trap: `ownerSlot` is NOT the syncer — do not fuse them

`[V]` `element.h:220` — `m_ownerSlot` is documented as *"originating peer slot for mirrors (D1-7);
-1 = none"*, and `mirror_manager.h:357` `DrainMirrorsForSlot` uses it to **destroy a departed peer's
mirrors**.

That is **birth authorship**, and it is load-bearing for teardown. The syncer is **current write
authority**, and it must be *reassignable* — that is the whole point of MTA's `OverrideSyncer`.

Fusing them would mean: reassigning a syncer silently re-targets which mirrors get destroyed when a
peer leaves. That is a data-loss bug, and it is exactly the failure family in
`[[lesson-single-latch-fused-states-gate-semantics]]` — one field, two meanings, and the gate reads
the wrong one.

**Decision: the syncer is a SECOND field**, beside `m_ownerSlot`, with its own lifetime. They will
frequently hold the same value; they are not the same concept and must never be aliased.

### A second identity space to reconcile

`[V]` `device_occupancy` is keyed by **device key** (`HolderOf(const wchar_t* key)`), while elements
are keyed by **`ElementId`**. These are two parallel authority spaces today.

Per `[[feedback-qf-enumerate-identity-maps-on-migration]]`, this must be resolved *before* building,
not discovered during: either devices become elements, or the arbiter owns two keyed tables with one
predicate over both. **Open question — see §9.**

---

## 5. The enforcement point

`[V]` The A4 surface is **68 `case` branches** (`event_dispatch_state.cpp` 24,
`event_dispatch_signal.cpp` 29, `event_dispatch_entity.cpp` 15). Auditing 68 handlers by hand
guarantees a miss and creates 68 places to keep in sync.

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

## 6. Claim transfer — the part that makes it usable

A pure "only the syncer may write" rule produces permanently stuck objects. MTA's answer
(`MTA_PRECEDENT.md` §3) is that a **non**-syncer may *request* takeover, granted only after the
arbiter validates, against its **own** state:

1. requester is not already the syncer,
2. a **cooldown** since the last takeover (`MIN_PUSH_ANTISPAM_RATE` = 1500 ms),
3. **arbiter-side proximity** — checked against the arbiter's copy of both positions, never the
   requester's claim,
4. same world context.

And on refusal, MTA sends an explicit failure **reply** (`CGame.cpp:3042`) so the requester rolls
back its local prediction. **We currently drop silently**, which desyncs the asker.

---

## 7. Principle 8 — the late-join answers this owes

Per CLAUDE.md principle 8 and `RULES.md` S6, a new authority lane is not done until every row here
is implemented, not merely acknowledged.

| Case | Answer | Existing machinery |
|---|---|---|
| Syncer departs mid-activity | Arbiter reaps the syncer on departure and reassigns or clears | `device_occupancy::OnDisconnectForSlot` already does this for claims |
| Peer joins mid-activity | Syncer assignments ride the join snapshot | `QueueConnectBroadcastForSlot` is the precedent |
| Syncer stalls / crashes without a clean departure | **TTL on the assignment**, reaped by the arbiter's tick | `device_occupancy::Tick` exists |
| Two peers race a takeover | Arbiter is the single writer; first-writer-wins, loser gets an explicit refusal | §6 |
| Element born mid-session | Birth assigns a syncer atomically with the eid | `[[lesson-identity-migrate-at-birth-covers-every-map]]` |

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

## 9. Open questions — resolve in `/qf` BEFORE stage 0

Per `[[feedback-qf-before-implementation]]`, this design gets a full pass before any code.

1. **The two identity spaces (§4).** Do devices become elements, or does the arbiter hold two keyed
   tables behind one predicate? This is the enumerate-identity-maps question and it is load-bearing.
2. **What is the syncer of a world prop nobody holds?** MTA assigns an unoccupied vehicle's syncer by
   proximity; our equivalent may be "the host, always" for most props — in which case the model
   collapses pleasantly for the majority of elements.
3. **Does the host, as a player, get syncer status for free on its own actions?** It is both arbiter
   and peer. The asymmetry needs one explicit rule, not per-site judgement.
4. **Enumerating the 68 kinds** into client-authorable vs not. This is the bulk of stage 3 and the
   most error-prone part; it wants its own review pass.
5. **Interaction with `PLAN_01` (peer auth).** A syncer assignment is only as trustworthy as the
   peer identity it names. Syncers make authority *consistent*; certificates make identity *real*.
   Neither substitutes for the other — but does stage 4 have any value before P1 lands?
6. **Phase-8 forward compatibility.** Is the arbiter API genuinely process-agnostic, or does it
   quietly assume in-process engine access? If it assumes, the phase-8 payoff evaporates and we
   should know that now.

---

## 10. What this does NOT do

- It does not authenticate *who* a peer is (`docs/security/PLAN_01_PEER_AUTH.md`).
- It does not protect a client from a hostile **host** — the host is the arbiter. The save-blob trust
  surface (`TRACKER` **S1**) is untouched and remains the hardest open problem.
- It does not make the host's simulation authoritative over physics. Per ROADMAP phase 8, server-side
  authoritative physics without the engine is *"a decade-class trap"*; the inversion is about **rules
  and state machines**, not simulation.

---

Related: `docs/security/MTA_PRECEDENT.md` (the citations) · `docs/security/PLAN_03_AUTHORITY.md`
(the security framing of the same work) · `docs/ROADMAP.md` phase 8 ·
`docs/COOP_ENTITY_EXPRESSION_MAP.md` (identity per entity) · `docs/COOP_SYNC_MAP.md` (which file owns
a lane).

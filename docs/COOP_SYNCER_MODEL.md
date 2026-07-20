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

## 1c. DEPLOYMENT REQUIREMENT (USER 2026-07-20) — the arbiter must be engine-free

> *"хочу чтобы в будущем хост мог создать лобби и dedicated server у него был встроен, и еще
> отдельный dedicated server был как бинарь"*

Three deployment modes for **one** arbiter implementation:

| Mode | Where the arbiter runs | Engine available? |
|---|---|---|
| **Embedded** | Inside the host player's game process (listen-server shape) | Yes — and that is the trap |
| **Headless host** | ROADMAP phase 6 (Wine, the game running headless) | Yes |
| **Standalone binary** | ROADMAP phase 8 | **No** |

**This retires §9 question (f).** Whether the arbiter API is process-agnostic is no longer an open
question — it is a **requirement**, and it decides the API shape before stage 0.

### The consequence, and it is severe

> **The arbiter NEVER reads engine state. It holds its own authoritative copy of everything it
> arbitrates on.**

MTA's proximity check works precisely because the server owns its own positions
(`[V]` `CUnoccupiedVehicleSync.cpp:490-492` compares `pVehicle->GetPosition()` against
`pPlayer->GetPosition()` — both server-side records, never a client's claim and never a render
transform). Every authority predicate we write must be answerable the same way.

**The embedded mode is where this dies quietly.** With the engine right there, reading a live actor
is one call away and always correct *in that mode* — so the portability breaks silently, with no
compile error and no failing test, and is discovered only when the standalone binary is attempted.

### Make it a build invariant, not a discipline

Proposed, and cheap: the arbiter lives in a subtree that **cannot include `ue_wrap/`**, enforced by
the build — not by review. Then "the arbiter is engine-free" is a mechanical, falsifiable property
checked on every compile, and the embedded mode cannot silently cheat.

This also means the arbiter needs its **own element state mirror** (positions, holders, whatever the
predicates read) fed by the engine side in embedded mode and by clients in standalone mode. That
mirror is the real deliverable of stage 0 — bigger than the API, and it is what phase 8 actually
needs to exist.

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

## 4b. `/qf` R1-R2 REFRAMED THIS DOC THREE TIMES — read before §5-§8

§5 and §8 below are **superseded in shape** by what two critic rounds measured. Kept for the
reasoning trail; the corrected direction is here.

### R1 — the syncer is not the claim (a retirement I nearly reverted)

`[V]` v116 RETIRED the claim-gated receive check as a RULE-1 root fix:
`[[lesson-claim-anchored-gate-races-its-own-release]]`. Measured live 17:04:46/47 — the client's
successful ping wrote `coord_signalData`, the *same* success dropped `coord_isPing`, so both the
catch detector and the host validator `holder==sender` raced a release **the detected event itself
triggers**, and the baseline roll-forward made the loss permanent.

`[V]` I then verified the two MTA citations personally (my own Rule S5 debt):
`CUnoccupiedVehicleSync.cpp:285` and `:490-492`, both verbatim as reported. **MTA's syncer is a
LONG-LIVED ASSIGNMENT, not released by the events it syncs** — takeover is a separate rate-limited
path. Our `device_occupancy` is a per-interaction claim whose lifetime is event-coupled.

**Building syncers on the claim table would inherit that lifetime and revive the v116 race at 68x
scale.** Hence three concepts, never aliased: `m_ownerSlot` (birth authorship, drives teardown) ·
**claim** (occupancy, event-coupled, UI/coordination) · **syncer** (write authority,
arbiter-assigned, event-DECOUPLED).

### R2a — assigned authority fits streams, not discrete verbs

MTA's model self-heals because the traffic is a continuous position stream: a rejected packet is
replaced 50 ms later. **Our discrete state writes do not self-heal — a rejection is a lost player
action.** So one syncer check across all 68 kinds is wrong on its face.

### R2b — the per-kind authority taxonomy ALREADY EXISTS, in comments

`[V]` `session_lanes.h:179` `IsClientRelayableReliableKind` is already a per-kind, receive-side
table, and its comments encode an authority model per kind:

| Model in the tree | Example kinds |
|---|---|
| `PRESSER-authored` | `DeskInput`, `DeskSndFx`, `PlayDeckEvent` |
| `CLAIM-OWNER-authoritative` | `DishAimState` — **the exact shape v116 retired** |
| `OCCUPANT-OR-GRABBER-authoritative` | `AtvState` |
| `ANY-PEER-announced idempotent state` | `DriveSlotState` — **authority is not needed at all; idempotent lines converge** |
| `WRITER-authored` | `DrivePayload` |
| `SYMMETRIC` | doors, lights, containers, garage, appliance, locker, power — **this is finding A4** |

**~6 authority models already live here.** The syncer would be a seventh. And a new per-kind table
at stage 3 would be a **second parallel table** beside this one — RULE 2.

### The corrected direction

1. **Promote the existing taxonomy from comments into the type** — each kind declares its authority
   model explicitly, beside the relay flag, in **one** table.
2. **Make it enforced on receive, one MODEL at a time** — not one handler at a time out of 68.
3. **Introduce the syncer only where a peer genuinely SIMULATES what the arbiter cannot compute.**
   MTA needs syncers because the client simulates an unoccupied vehicle and the server does not.
   Where our host already computes the truth, "syncer" would be a mere permission label — a simpler
   thing, and it should stay simpler.
4. **`SYMMETRIC` is not a model, it is the absence of one.** Those 13 kinds are A4. That is where
   the work actually is.

### Still unanswered (carried into R3+)

- **Which of the 68 kinds have peer-local simulation the arbiter cannot run?** Decides where syncers
  are real vs a label.
- **Can a discrete lane accept rejection + retry at all?** Or must the invariant be that a
  reassignment can *never* invalidate an in-flight write? Must be read from a real discrete lane
  before stage 0 shapes the API around the wrong answer.

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

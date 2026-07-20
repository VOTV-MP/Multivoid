# Plan 03 — authority enforcement (the security framing)

**Closes:** `TRACKER` **A3** (entity commands trust any client) + **A4** (symmetric families accept
any peer's writes) + **A5** (unbounded client-authored economy write).

> **THE MECHANISM NOW LIVES IN `docs/COOP_SYNCER_MODEL.md`** (USER DECISION 2026-07-20: adopt the MTA
> authority model FIRST, then return to security). A3/A4/A5 are not bugs to patch — they are the
> **absence of that architecture**. Building a security patch here first would create a mechanism the
> migration then replaces (RULE 2, migration baggage).
>
> **This file keeps only the security framing:** what the attack is, what it costs a victim, and what
> "closed" must mean. For the design, the staging and the Principle-8 answers, read the syncer model.

**Status: the fix is DESIGN in `COOP_SYNCER_MODEL.md`, gated on its `/qf`.** Both rows are `[A]` —
re-read before building (Rule S5).

---

## 1. The root

Coop was built on the assumption that **the peer is a well-behaved copy of this build.** Under that
assumption a symmetric lane — either peer may author, both mirror — is the simplest correct design,
and it is what most of the state families use.

The threat model breaks the assumption: peers are strangers. A symmetric lane then reads as *"any
stranger may author any change to your persistent world."*

**The mechanisms to fix it already exist.** This is the important part:

- `trash_grab_intent.cpp:298-302` consults a **holder table** correctly.
- `device_occupancy` maintains a **claim table** — but `[A]` it is read only by *senders* deciding
  whether to stream, and never by a host receive path. It is **advisory**.

So the work is not "invent authority". It is **"make the tables that already encode authority
enforcing at the receive seam."**

---

## 2. A3 — entity commands (`PropDestroy` / `PropConvert` / `PropRelease`)

### The finding `[A]`

- Only an **eid-range check** guards these (`event_dispatch_entity.cpp:253,316,73`; ranges
  `:286-292,348-363`). The range proves the sender is *a* client, not that it holds *that* entity.
- **Aggravating and the reason this is CRITICAL:** these kinds are client-relayable, so a forged
  destroy is **fanned out to other clients before the host validates it**
  (`session.cpp:454` relays, `:463` validates). See Rule S2.

### The fix, in two independent halves

**Half 1 — validate before relay (Rule S2).** Move the relay below the validation for these kinds.
Smallest possible change, closes the multi-peer amplification even before authority lands. Ship it
first and separately.

**Half 2 — route through the holder predicate.** The predicate exists at
`trash_grab_intent.cpp:298-302`; extract it to a shared seam and consult it in the entity dispatch
path. A destroy of an entity you do not hold is rejected at the host, logged, and not relayed.

### Command vs intent

The deeper RULE-1 shape: these are **commands** ("this entity is destroyed") where they should be
**intents** ("I wish to destroy this entity"), with the host authoring the outcome. That is how the
grab lane already works, and `[[lesson-presser-authored-state-not-intent-for-invisible-verbs]]`
records when each shape is right.

Converting all three to intents is the correct end state. It is also a wire-format change (protocol
bump) and a behavioral change to a lane that works today. **Sequence it after Half 1 + Half 2**,
which together already close the exploit; the conversion is then a clean RULE-2 retirement of the
command form rather than an emergency fix.

---

## 3. A4 — the symmetric families

### The finding `[A]`

Whole families accept any peer's writes: doors, lights, containers, keypads, power, ATV, trash piles,
sleep, inventory blob, email delete, and the **entire desk / laptop / drive / rack / meadow chain**
(`event_dispatch_state.cpp:40-74,75,97,118,143,226,259,288,361,406,424`;
`event_dispatch_signal.cpp:40-520`).

Impact in user terms: **a peer can drive another player's desk mid-session, delete their email, wipe
DB rows, and unlock doors.**

### The fix — one predicate at the family seam

`event_dispatch_signal.cpp:390-391` is where the claim is already known. The change is to make
`device_occupancy` **enforcing on receive**: an inbound write to a claimed unit from a peer that does
not hold the claim is rejected.

**One predicate at the family seam, not per-lane checks.** The signal chain alone is nine lanes
(L4-L9 plus the desk); patching them individually guarantees a missed one and creates nine places to
keep in sync. This is the invariant-not-a-site-list discipline.

### MTA already solved this shape — port it rather than inventing

`[A]` `MTA_PRECEDENT.md` §1-§3, §5. Four things land directly here:

1. **Authority is host-assigned, never client-claimed.** MTA's server picks a syncer, *pushes* the
   assignment, and checks `GetSyncer() == pPlayer` on receive (`CUnoccupiedVehicleSync.cpp:285`,
   `CPedSync.cpp:243`, `CObjectSync.cpp:214`). Our `device_occupancy` is the same data structure —
   theirs is merely host-owned and read on the receive path.
2. **A generation counter beside the check.** `CanUpdateSync(ucTimeContext)` is bumped whenever the
   server itself changes the element, so a stale/replayed but otherwise-authorized packet is rejected.
   Cheap; port it with the predicate, not after.
3. **A default-deny per-kind flag is the cheapest broad win.** `CGame.cpp:2663-2710` — a client-sent
   action runs only if its kind was registered `bAllowRemoteTrigger`, internal kinds register `false`,
   and the offender gets an event fired **at them** (rate-limited, `:2696`). A per-kind
   `bAllowClientAuthored` defaulting to false, checked **once** in our dispatch switch, collapses much
   of A3+A4 into one enforcement point instead of ~30 handler audits. Our `session_lanes.h:181-185`
   relay whitelist is this idea applied to forwarding; it is missing for *applying*.
4. **Do NOT cite MTA for our relay ordering.** `[A]` `CUnoccupiedVehicleSync.cpp:472` broadcasts even
   when every entity failed the syncer check — the same relay-before-validate shape as our A3, and
   arguably a bug there too. Rule S2 stands on its own reasoning.

### The Principle-8 problem (Rule S6) — this is what makes it hard

CLAUDE.md principle 8: mid-activity join is always handled. A naive enforcing claim table produces
**permanently locked devices**:

| Case | Naive result | Required answer |
|---|---|---|
| Holder departs mid-activity | Desk stays claimed forever; nobody can use it | Host reaps the claim on peer departure — the leaver-teardown fanout already exists (`[[lesson-every-session-end-path-full-teardown-fanout]]`) |
| Peer joins mid-activity | New peer has no claim state; its first write is rejected | Claims ride the join snapshot — the desk lane already has an FSM-hold reconciler to model on (v115b) |
| Claim never released (crash, timeout) | Same as departure, without the departure event | TTL on the claim, reaped by the host's reconciler |
| Two peers race a claim | Both rejected, or both accept | Host is the single claim authority; first-writer-wins, loser is told |

**Each row above must be implemented, not just acknowledged.** A validation that locks out a
legitimate player is an availability bug wearing a security fix's clothes.

**MTA's answer to the takeover row, which was our open question** `[A]`
(`CUnoccupiedVehicleSync.cpp:476-510`): a non-holder may **request** takeover, and the server grants it
only after four checks it evaluates itself — not already the holder, a **cooldown**
(`MIN_PUSH_ANTISPAM_RATE` = 1500 ms), **host-side proximity** against the host's own copy of both
positions, and same-dimension. Only then `OverrideSyncer()`.

So a claim transfer is **a rate-limited request validated against host state**, never a client
assertion. That plus a TTL covers every row in the table above. And `CGame.cpp:3042` adds the piece we
lack: on refusal MTA sends an explicit **failure reply** so the client can roll back its local
prediction — we drop silently, which desyncs the requester.

### Sequencing within A4

1. Make the claim table **host-authoritative** (host is the single writer of claim state) — no
   enforcement yet, so no behavior change. Verifiable by log diff alone.
2. Add the four Principle-8 answers above (reap on departure, seed on join, TTL, race rule).
   Still no enforcement. Smoke: a peer leaving mid-desk-use must free the desk.
3. **Then** flip the receive-side predicate to enforcing, family by family, starting with the desk
   chain where the claim machinery is most mature.

Only step 3 can break legitimate play, and by then steps 1-2 have been observed working.

---

## 4. What this plan does not close

- A peer who **legitimately holds** a claim can still abuse it. Out of scope
  (`THREAT_MODEL.md` §3, "protecting a host from a peer who was invited").
- The save blob trust surface (**S1**) is untouched.
- Nothing here authenticates *who* the peer is — that is `PLAN_01_PEER_AUTH.md`. A3/A4 make authority
  *consistent*; P1 makes identity *real*. They are complementary and neither substitutes for the
  other.

---

## 5. Risk

**The highest-regression-risk work in the folder.** Every step touches lanes that work today and that
the user has hands-on tested. Mitigations:

- Ship Half 1 of A3 (validate-before-relay) alone first — it is nearly free and closes the worst part.
- Non-enforcing steps before enforcing ones, so behavior changes land last and observably.
- Per-family rollout with a smoke between, not a big-bang flip.
- Expect a protocol bump when commands become intents; plan it as its own release.

---

Back to: `README.md` · `TRACKER.md` **A3**, **A4** · `RULES.md` S2, S6.

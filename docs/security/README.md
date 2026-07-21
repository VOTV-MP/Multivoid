# docs/security/ — the security knowledge base

**The concept (folder-per-domain-concept rule):** everything about **what an attacker can do to a
Multivoid player, host, or to our own infrastructure, and what we do about it.**

Not "encryption", not "TLS" — those are mechanisms, and this folder exists precisely because the
project spent a day and a half building a mechanism before anyone wrote down the threat.

**This file is navigation only.** Every fact lives in one of the documents below; nothing is
restated here (RULE 2 — one home per fact).

---

## Read in this order

| # | Document | What it holds | Read it when |
|---|---|---|---|
| 1 | **`THREAT_MODEL.md`** | Who the attacker is, what is worth protecting (ranked), and **what we deliberately do NOT protect** | **First, always.** It is the ranking authority for every severity |
| 2 | **`SUBSTRATE.md`** | The measured engine + control-plane facts everything rests on | Before contradicting any assumption about GNS, certs, or the master |
| 3 | **`TRACKER.md`** | The register: one row per finding, with evidence and status | To find out whether something is already known |
| 4 | **`EXECUTION.md`** | The board: waves, order, dependency graph, session log, definition of done | To find out **what to work on next** |
| 5 | **`RULES.md`** | Six standing rules (S1-S6), each born from a specific failure | Before writing a validation, a comment about a control, or a cap |
| 6 | **`DECISIONS.md`** | What was decided, and the **positions retracted** with the measurement that killed each | Before proposing a security mechanism — your idea may already be in the retracted table |
| 7 | **`MTA_PRECEDENT.md`** | What MTA:SA actually shipped for 15 years against a hostile player base, with citations — and where it has **no** answer for us | Before designing any authority, flood-protection, identity or wire-parsing mechanism (standing rule 2026-05-28) |

## The fix plans

| Plan | Covers | Status |
|---|---|---|
| **`PLAN_01_PEER_AUTH.md`** | **P1**, **A1** — peer certificates from our own CA. The root fix | DESIGN — **gated on a spike** |
| **`PLAN_02_WIRE_HARDENING.md`** | **W1-W10** — apply-side caps and validation | **W1-W6 BUILT**; W7-W10 open |
| **`PLAN_03_AUTHORITY.md`** | **A3**, **A4**, **A5** — the security framing only | **Mechanism moved to `docs/COOP_SYNCER_MODEL.md`** |
| **`PLAN_04_CONTROL_PLANE.md`** | **A2**, **A5**, **A6**, **A7**, **A9**, **S2** — master + moderation | DESIGN — mostly small deletions |
| **`PLAN_05_WEBSITE.md`** | **A8** — render rules written before the site exists | DESIGN — done until the site is written |

---

## Where things stand

> **7 findings BUILT, 14 OPEN** (2026-07-20). Closed: the save-transfer one-packet remote kill
> (W1/W1b/W2), W3's pre-Begin window, and the three parse-layer floods (W4/W5/W6) — plus both false
> security comments. **Nothing is VERIFIED:** no hostile-peer drill has ever run, and W6 is not even
> runtime-exercised.
>
> **The authority-shaped findings (A3/A4/A5) are now downstream of
> `docs/COOP_SYNCER_MODEL.md`** — a user decision to adopt the MTA authority model first. TLS arcs
> 1-2 remain defence-in-depth and close nothing (`DECISIONS.md` §1). Next actions: the **CA spike**
> (`PLAN_01` §2) and the syncer model's own `/qf`.
>
> **Forward-looking RULES (F1-F6)** now live in `TRACKER.md` — decisions about the future dedicated
> server and anti-cheat that are cheap to state now and expensive to retrofit: blob-donation is
> host/admin-only (F1); the resource system ships `bAllowRemoteTrigger` default-deny (F2); validate by
> distance+rate never geometry (F3); anti-dupe is the authority model not a detector (F4); no
> client-side anti-cheat (F5); the validation toggle is a per-check list in one config, never over the
> authority model (F6). Measured against MTA — see `MTA_PRECEDENT.md` §11.

Current detail, including the session log and what is unblocked right now: **`EXECUTION.md`**.

---

## Conventions used in every file here

**Evidence tags.** `[V]` measured personally with a file:line citation · `[A]` reported by a
read-only audit agent, **not yet personally re-verified** · `[?]` unverified.

**Status.** OPEN · DESIGN · BUILT (shipped, not hands-on) · VERIFIED (hands-on or matching live log
— say which) · DISMISSED (with evidence).

**The `[A]` rule.** Fourteen rows are still `[A]`. **Re-read the cited site before fixing one**
(`RULES.md` S5) — on 2026-07-20 doing this to five rows produced three corrections to the fixes.

---

## Related documents outside this folder

This folder links, never restates:

- `docs/COOP_SYNC_MAP.md` — which file owns a given sync lane
- `docs/COOP_DISPATCH_VISIBILITY.md` — whether a hook fires; observe vs drive
- `docs/COOP_EVENT_JOIN.md` — the late-join contract every lane owes (relevant to `PLAN_03`)
- `docs/LESSONS.md` §9 — the security lessons, with links to their full `memory/` files
- `research/findings/network/votv-tls-tier-b-c-DESIGN-2026-07-20.md` — the TLS design of record.
  **Accurate as an as-built record of arcs 1-2; superseded as a plan** — see `DECISIONS.md` §1

# Multivoid documentation

Multivoid adds co-op multiplayer to *Voices of the Void* — a single-player Unreal Engine 4.27 game —
**without modifying a single original game file**. These docs are the engineering record of how.

They are written by the people building it, so they are dense and they argue with themselves in
public: a claim is tagged with how it was established, and a doc that turned out wrong says so
instead of being quietly edited. Pick a lane below rather than reading top-to-bottom.

---

## I just want to play it

| | |
|---|---|
| **[INSTALL.md](INSTALL.md)** | Install, update, uninstall. The single owner of that prose — the README and every release body just link here |
| **[../SECURITY.md](../SECURITY.md)** | What the mod does and does not protect, and how to report a vulnerability. **Read the "what does not hold" part before hosting for strangers** |
| **[../README.md](../README.md)** | The project front page: what works today, field reports from testers, where to get builds |
| **[FIELD_REPORTS.md](FIELD_REPORTS.md)** | The full tester-credit ledger — every report that changed the mod, what it actually turned out to be, and what shipped. The README and the site carry a one-line-per-tester summary of this |

Everything below this line is for people who want to know how it works. You do not need it to play.

---

## I want to understand it, or help

Start here, in this order:

| | |
|---|---|
| **[ARCHITECTURE.md](ARCHITECTURE.md)** | The shape of the mod in one read: the loader, the engine-wrapper layer, the co-op layer, and why they are separate |
| **[COOP_SCOPE.md](COOP_SCOPE.md)** | What is and is not replicated. This one is *law* — anything not listed is deliberately not synced |
| **[DEVS_GAUNTLET.md](DEVS_GAUNTLET.md)** | The VOTV developers' public statement on why multiplayer mods fail, kept verbatim. It is the bar this project builds to |
| **[FEASIBILITY.md](FEASIBILITY.md)** | Whether this is even possible, answered with measurements rather than optimism |
| **[ROADMAP.md](ROADMAP.md)** | Where it is going — the phased plan, with the dated decision entries that changed it |

Then, if you are going to touch code:

| | |
|---|---|
| **[RE_WORKFLOW.md](RE_WORKFLOW.md)** | How this project reverse-engineers the game: reflection first, then IDA, then UE4SS as a probe. None of those ship |
| **[AUTONOMOUS_TESTING.md](AUTONOMOUS_TESTING.md)** | The two-instance LAN harness — how a change gets smoke-tested without a human in the loop |
| **[RELEASE.md](RELEASE.md)** | How a build becomes a release, and the gates it must pass |
| **[THUNDERSTORE.md](THUNDERSTORE.md)** | How a release becomes a Thunderstore package: the manifest, the upload, and the rules that cannot be undone (a version is immutable; an author cannot delete a package) |
| **[VERSION_MIGRATION.md](VERSION_MIGRATION.md)** | What happens when VOTV updates: the measured version surface and the port runbook |
| **[MULTIPLAYER_UI.md](MULTIPLAYER_UI.md)** | The menus, the server browser, the master/signaling servers behind them |

**Before writing any entity-sync, hook or spawn-catch code, read these two first.** They exist
because not knowing them cost a three-iteration rework and two review agents giving opposite answers:

- **[COOP_DISPATCH_VISIBILITY.md](COOP_DISPATCH_VISIBILITY.md)** — will my hook even fire? Visible vs
  invisible Blueprint dispatch, and the trap that `init()` is BP-internal.
- **[COOP_ENTITY_EXPRESSION_MAP.md](COOP_ENTITY_EXPRESSION_MAP.md)** — how each entity gets identity,
  expression and destruction, plus the duplication matrix.

---

## I maintain this

**Models — how authority and state are supposed to work**

[COOP_METHODOLOGY.md](COOP_METHODOLOGY.md) (the architecture doctrine, adapted from MTA:SA) ·
[COOP_SYNCER_MODEL.md](COOP_SYNCER_MODEL.md) (per-element authority: assigned, never asserted) ·
[COOP_SERVER_MODEL.md](COOP_SERVER_MODEL.md) (what "our server" is; embedded == dedicated) ·
[COOP_CLIENT_MODEL.md](COOP_CLIENT_MODEL.md) · [COOP_RNG_AUTHORITY.md](COOP_RNG_AUTHORITY.md)
(who rolls shared-world randomness) · [COOP_EVENT_JOIN.md](COOP_EVENT_JOIN.md) (joining
mid-event — every sync lane owes a late-join answer)

**Maps — where a thing lives, and what state it is in**

[COOP_SYNC_MAP.md](COOP_SYNC_MAP.md) (which file owns which sync) ·
[COOP_SYNC_PROFILES.md](COOP_SYNC_PROFILES.md) (per-system status: what works *inside* X, with the
evidence for each claim) · [MODULARIZATION_PLAN.md](MODULARIZATION_PLAN.md)

**Known-hard problems, each with its own file**

[COOP_STABLE_ID_SIDECAR.md](COOP_STABLE_ID_SIDECAR.md) (entity identity across a join) ·
[COOP_WORLD_PROP_DIVERGENCE.md](COOP_WORLD_PROP_DIVERGENCE.md) (props that mutate themselves over
time) · [COOP_MIRROR_IDENTITY_WINDOW_RACE.md](COOP_MIRROR_IDENTITY_WINDOW_RACE.md) ·
[COOP_INSTANT_WORLD_TWO_LAYER.md](COOP_INSTANT_WORLD_TWO_LAYER.md) ·
[COOP_VM_DISPATCH_PLAN.md](COOP_VM_DISPATCH_PLAN.md) ·
[AUTHORITATIVE_INTERACTABLE_MIGRATION.md](AUTHORITATIVE_INTERACTABLE_MIGRATION.md)

**Per-domain trees** — one folder per game system, each with its own README

[events/](events/) · [items/](items/) · [signals/](signals/) (the signal-processing pipeline) ·
[upgrades/](upgrades/) · [notifications/](notifications/) · [kerfur/](kerfur/) ·
[piles/](piles/) (trash piles — the longest-running sync problem in the project, 53 files)

**Live arcs** — work in flight, each tracking its own work packages

[UE4SS_ARC.md](UE4SS_ARC.md) (becoming a UE4SS mod) ·
[OVERLAY_CAPTURE_COEXIST.md](OVERLAY_CAPTURE_COEXIST.md) (coexisting with RTSS and OBS) ·
the local-only docs-arc note (this documentation audit)

**The ledger**

**[LESSONS.md](LESSONS.md)** — every hard-won lesson, categorized, each with a "look here first next
time" pointer. It is long on purpose: it exists so the same hole is not dug twice. If you are about
to spend a day on something, grep it first.

[OPUS_48_DISCIPLINE.md](OPUS_48_DISCIPLINE.md) — the working agreement for AI-assisted sessions on
this codebase. Multivoid is built by one maintainer with Claude, which the project states openly
rather than hides.

---

## How to read a claim in these docs

Status and evidence are tagged, and the tags are load-bearing:

| Tag | Means |
|---|---|
| `[V]` / **VERIFIED** | measured personally, with a citation — a log line, a disassembly, a file:line |
| `[RD]` | derived from reverse engineering, not directly observed running |
| `[A]` | reported by a read-only audit pass, **not** personally re-verified |
| `[?]` | unverified — a hypothesis wearing a claim's clothes |
| **DESIGN** vs **AS-BUILT** | what was planned vs what actually shipped. These drift, and the docs say when they did |

**Nothing is marked working on the strength of an automated smoke test alone** — that gets called a
smoke pass, and it says so. A doc that says "PROVEN" without naming its evidence is a bug in the doc.

Two conventions worth knowing: files ending in `-RE-<date>` are durable reverse-engineering records
and files ending in `-DESIGN-<date>` are point-in-time plans that are **deliberately never rewritten**
— read them as "what was believed that day", not as current instructions. Anything superseded moves
to an `_archive/` folder rather than being deleted, so an abandoned approach can never be mistaken
for the live one.

Some pointers in these docs lead to `research/` or `docs/security/`. Those resolve in the
maintainer's working tree and not on GitHub — both are kept unpublished on purpose
(the local-only docs-arc note says why). That is not a broken link.

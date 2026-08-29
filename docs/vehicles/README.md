# docs/vehicles/ — the per-vehicle coop knowledge base

*[↑ docs index](../README.md)*

One doc per VOTV **vehicle** — a driveable, occupant-carrying, multi-body physics actor.
Same discipline as `docs/items/` and `docs/events/`: each vehicle's native behaviour
(bytecode ground truth, evidence-tagged), its sync-axis table, its coop design, and its
honest as-built status live in that vehicle's own file.

A vehicle is NOT an item and NOT a device: it has an **occupant** (which unpossesses
`mainPlayer_C`), a **multi-rigid-body rig** (the body plus constraint-driven wheels — so a
single actor transform is NOT the whole pose), and a **parts/upgrade economy**. Those three
properties are why this folder exists rather than a row in `docs/items/`.

| Vehicle | Class | Doc | Status |
|---|---|---|---|
| ATV / quadbike | `AATV_C` (+ `ATV_Child_C`) | [ATV.md](ATV.md) | RE COMPLETE; sync PARTIAL (pose only) |

Cross-cutting contracts stay where they are (link, don't restate):

- `docs/COOP_DISPATCH_VISIBILITY.md` — will my ProcessEvent hook fire on this verb?
- `docs/COOP_ENTITY_EXPRESSION_MAP.md` — identity / expression / destroy per class.
- `docs/COOP_SYNC_MAP.md` — where each shipped wire-sync lives.
- `docs/COOP_SYNCER_MODEL.md` — per-element authority + the act-as-host intent rule (§2b).
- `docs/COOP_EVENT_JOIN.md` — the per-lane late-join answer table (principle 8).
- `docs/upgrades/` — the upgrade subsystem hub. The ATV's **physical module** family is
  documented in full inside [ATV.md](ATV.md) §4; `docs/upgrades/TRACKER.md` carries its status row.

Evidence tags used in this folder (same as `docs/README.md`):
**[V]** measured this session from bytecode / pak / live source (cited) ·
**[RD]** reasoned from measured facts, composition not yet run ·
**[A]** asserted by an earlier doc, not personally re-verified ·
**[?]** not excavated.

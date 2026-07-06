# docs/items/ — the per-item coop knowledge base

One doc per VOTV ITEM whose behavior is its own subsystem (user ask 2026-07-06, born from
the hook). Same discipline as `docs/events/` (THE METHOD, granular passes): each item's
native behavior (bytecode ground truth, tagged), its sync-axis table, its coop design, and
its honest status live in that item's own file. Plain props with no behavior beyond
grab/throw/physics do NOT get a doc — the prop lane covers them; this folder is for items
that are effectively little state machines (deployables, tools with world-side actors,
multi-actor contraptions).

Cross-cutting contracts stay where they are (link, don't restate):

- `docs/COOP_ENTITY_EXPRESSION_MAP.md` — how an entity class gets identity/expression/
  destroy; the dupe matrix. An item doc that ships a sync adds/updates its class row there.
- `docs/COOP_SYNC_MAP.md` — where every wire-sync feature lives. A shipped item sync adds
  its row.
- `docs/COOP_EVENT_JOIN.md` — late-join contract. Deployed item actors that persist in the
  world need a join answer like any lane.

## Doc skeleton (copy for a new item)

```markdown
# <item name> — <one-line what it does>   (STATUS: RE | DESIGN | AS-BUILT | VERIFIED)

## 1. Native behavior (ground truth)
   Classes (parent chain!), the full use/lifecycle state machine, every externally
   visible effect. Tag every claim [bytecode fn@off] / [wiki] / [live log] / [?].
## 2. Sync-axis table
   | axis | owner (who simulates) | peers need | carried by (lane/wire) |
## 3. Coop design
   Ownership per lifecycle phase; what mirrors, what replays, what stays local.
## 4. Caveats / known quirks
   Native guards, save/reload interactions, SP-only assumptions (getMainPlayer!).
## 5. Verification
   What was proven, HOW, what remains.
```

## Tracker

| Item (classes) | Doc | Pass |
|---|---|---|
| hook (`prop_hook_C` : prop_C → spawns `hook_C` : actor_save_C) | [hook.md](hook.md) | RE DONE 2026-07-06 (full static bytecode pass: prop_hook uber + hook uber + all named fns). Sync: DESIGN RATIFIED (phase-split authority, poll-diff transitions, `coop/items/` home — hook.md §3), NOT BUILT — build starts after 2-3 smart-item cases are doc'd (user 2026-07-06: collect cases first, then implement the pattern) |
| rope (`rope_C`) — two-end sibling of the hook (PhysicsConstraint + ConstraintBroken handler, 4 fns) | (todo) | next candidate after hook ships |
| variants census: `hook_Child`, `hook_flesh`, `prop_hook_erie` (assets exist in pak next to hook) | — | fold into hook.md when RE'd (likely `single=true` / cosmetic children) |

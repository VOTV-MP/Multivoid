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
| nailgun (`prop_nailgun_C`/`prop_nail_C` : prop_C → `nailProjectile_C` : Actor → `nail_C` : actor_save_C) | [nailgun.md](nailgun.md) | RE DONE 2026-07-06. The pure commit-to-host case (no player-attached phase); attach fns are SELF-TRACING from transform = native host replay. Sync NOT BUILT |
| wallbuilder (`prop_wallbuilder_C` : prop_C → `customWall_unfinished_C` : actor_save_C → `customWall_C`) + wallfixer | [wallbuilder.md](wallbuilder.md) | RE DONE 2026-07-06 (unfinished-wall uber = census only, read before building). Editing natively local (tick gated on local holder); commit = transform+shape+material. Sync NOT BUILT |
| rope (`prop_rope_C` : prop_C → **`rope_C` : hook_C**) | [rope.md](rope.md) | RE DONE 2026-07-06. rope_C IS a hook subclass (4-fn diff) — ships automatically with hook sync as a class ROW, no own lane |
| grenade + pipebomb (`prop_grenade_C`/`prop_pipebomb_C` : prop_C → transient `explosion_C`) | [grenade.md](grenade.md) | RE DONE 2026-07-06. New axis: ARM = a mode on an existing host-owned prop (intent, not spawn) + one authoritative fuse clock + explosion replay event. explosion_C unread. Sync NOT BUILT |
| fishing rod (`prop_fishingRod_C` : prop_C → transient `fishingRodString_C` : Actor) | [fishingrod.md](fishingrod.md) | RE DONE 2026-07-06. Hook's owner-phase twin (never anchors); new axis: loot RNG → host-rolled catch commit. String uber unread (roll site!). Sync NOT BUILT |
| physgun + gravity gun (`prop_physgun_C` : prop_C, `prop_physgun_soft_C` : physgun, `prop_gravgun_C` : prop_C) | [physgun.md](physgun.md) | RE DONE 2026-07-06. Physgun = grab/carry-at-range (rides the prop-lane rails: intents + host-side drive); freeze couples to hook's unfreeze; input-delegate self-binding must NEVER install on mirrors. gravgun = NO force code — a chrysalis that microwave-charges (250) into physgun_soft. Sync NOT BUILT |
| food (`prop_foodBox_C`/`prop_food_C` : prop_C; family: banana/mushroom/pinecone/snuskLoaf/soap) | [food.md](food.md) | RE DONE 2026-07-08 + DESIGN RATIFIED (/qf R1-R7). MEASURED: eaten ONLY while HELD (client-authoritative) + one-press-one-use; `uses` count invisible + client-local; only the terminal morph (destroy box + spawn `prop_C 'g_foodbox'` via FinishSpawn) crosses. => FOOD = an INSTANCE of the rock-drop held-item→world-prop pattern, NOT a new lane. NOT BUILT (lands with the deferred rock-drop authoring). Host-authoritative "use-intent" design was built then REJECTED by the HELD-ONLY measurement |
| concrete/cement/bucket (`prop_cementBag_C`, `prop_concreteBucket_C` : prop_C; wetConcrete wall/pile/clump) | [concrete.md](concrete.md) | RE DONE 2026-07-08 + DESIGN (/qf R1-R6 + a code-verified probe). MEASURED: cementBag = HELD (client-owned) → rock-drop held→world instance; concreteBucket = WORLD (host-owned) depletable+curing, the first confirmed instance of `COOP_WORLD_PROP_DIVERGENCE.md` (save-loaded client world props run their own brain + diverge). Fix = extend the pile-lane pattern (park brain + host authors progression) + a 2-scalar curated push. NOT BUILT (gated; generic primitive DEFERRED, N=1) |
| variants census: `hook_Child`, `hook_flesh`, `prop_hook_erie`, `prop_physgun_s` (assets exist in pak) | — | fold into parent docs when RE'd |

## The pattern (6 cases in — the shared smart-item shape)

Persistent deployed actors are **`actor_save_C`** children with **replay-friendly native
entry points** (hook: attach_a/b `actorReplace...` params; nail: attach/nailNail
self-trace from own transform; wall: spawn takes transform+2 ints). The split that
repeats:

1. **Owner-local, never synced**: previews, montages, minigames, inventory/ammo
   consumption, anything gated on `getMainPlayer`/local holder (the game does this
   gating itself).
2. **Commit intent → host** (small reliable message): EITHER a spawn of a persistent
   actor (nail, wall, anchored hook/rope — transform + a couple of scalars, host
   replays via the NATIVE fn) OR an eid-addressed ACTION on an existing host-owned
   prop (grenade/pipebomb arm — the fuse clock must be the host's).
3. **Host → clients mirror** of the persistent actor (spawn + tiny state stream;
   display-only — constraints/overlaps/timers live host-side ONLY, mirror tick OFF).
4. **Late-join = host save**, free (actor_save_C + prop getData persist natively;
   transient Actors — projectiles, casts, explosions — are rightly absent).
5. **Player-attached phase** (hook climb, fishing cast): owner-simulated extension of
   the player; ONE shared "held-item aux state" slot in the per-player stream, not
   per-item lanes.
6. **Transient world-mutating bursts** (nail projectile, explosion): host replays the
   burst natively (impulse/damage host-side); clients get a cosmetic replay event.
7. **Loot/RNG payouts** (fishing catch): host rolls (client RNG suppressed — same
   principle as events), client sends the trigger, prop lane delivers the result.
8. **Subclasses are ROWS, not lanes** (rope_C : hook_C; physgun_soft : physgun).
9. **Continuous manipulation of a host-owned body** (physgun drag, hook reel-on-prop,
   ATV↔prop tow): the constraint/kinematic WRITES live host-side; the manipulating
   player streams aim/intents up, the prop lane streams the resulting pose back.
   User facts 2026-07-06: hook reel PULLS PROPS, and an anchored hook can tow a prop
   behind the ATV with the hook in nobody's hand — host adoption must trigger the
   moment EITHER end lands on a dynamic body, not only at release.
10. **The held-vs-world ownership cut is the FIRST measurement** (RULE 2026-07-08, from
   the food + concrete `/qf`s): a "use/deplete an item" case is NOT one lane — MEASURE
   whether the item is used **while HELD** (`mainPlayer.@holding_actor` gate, a discrete
   `IE_Pressed` on the holder) or **while PLACED IN WORLD** (an aimed `takeConcrete`/
   `lookAt`-interact). HELD ⇒ CLIENT-authoritative ⇒ an instance of the rock-drop held→
   world morph authoring (food = [food.md]; cementBag = [concrete.md]). WORLD ⇒ HOST-
   authoritative. Do the disasm (`holding_actor` gate) before scoping any use/deplete sync.
11. **A world prop that mutates its own state over time is a DIVERGENCE case, not an item
   lane** (RULE 2026-07-08 — see `../COOP_WORLD_PROP_DIVERGENCE.md`): a joining client
   save-loads the host world, so keyed world props are REAL brained `Aprop_C` with tick
   LEFT ON, and no per-prop BP-scalar wire channel exists → a local-accumulator mutation
   (drying, curing, rotting, growing, heating) self-simulates on both peers and diverges.
   Fix = extend the pile-lane pattern (park the brain + host authors progression) + a
   curated on-change push for any mid-life scalar. First instance: concreteBucket ([concrete.md]).

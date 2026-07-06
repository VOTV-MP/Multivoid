# fishing rod — cast, wait, reel, loot   (STATUS: RE)

The second player-attached-phase item (hook's validation partner): the rod never anchors
to the world — the whole lifecycle hangs off the owning player. Adds one genuinely new
axis to the pattern: **loot RNG resolution**.

- **`prop_fishingRod_C` : prop_C** — the rod ITEM. `string` (component ref), `length`
  (line length, cast sets 2000), `bait` name + `luck`/`eat`/`lure` floats (bait state on
  the rod), c1-c10 curve components + a `test` timeline (rod bend animation),
  scrollUp/scrollDown (reel), bind/unbind, up/down, getData/loadData (bait/lure state
  persists with the rod). LMB cast (uber @1226): `length := 2000`, deferred-spawn
  **`fishingRodString_C`** with `vel` (throw vector), `rod=self` back-ref, and the
  bait stats (`chance=lure, luck, bait, eat`), then `string.setLength(length)`.
  [bytecode]
- **`fishingRodString_C` : Actor** — the TRANSIENT cast (not saved): buoy + hook chain
  (`buoy2hook`/`cEndToBuoy` constraints), `drop` MAP (the loot table), `processDrops`,
  `catched` object, `setLength`/`setHookLoc`, `restoreString`. Owns the bite/catch
  minigame per-cast. [bytecode: property + fn census; uber not read — minigame flow
  inferred, tag [?] where it matters]
- **Catch payout** (rod uber @16/@645/@2359): spawns the caught thing as `prop_C` /
  `prop_food_C` (with `foodData`) / a class from the drop — by `bait` name and the drop
  resolution. A WORLD SPAWN (persistent prop) at the end of a local minigame. [bytecode]

## 2. Sync-axis table

| axis | owner | peers need | carried by |
|---|---|---|---|
| rod in hand, bend anim, reel scroll | owning player (all local: timeline + scroll on own item) | arm pose rides player pose; rod bend = cosmetic [?] worth checking on puppets | player pose lane; likely nothing extra v1 |
| the cast (string/buoy actor, ~minutes) | owning player (transient Actor, minigame local) | SEE the buoy + line (a peer watching a fisherman should see the float in the water) | GAP — display mirror (spawn event + buoy pos at low cadence) |
| bite/minigame state | owning player | nothing (private feedback) | none |
| CATCH RESOLUTION (RNG → what spawns) | must end HOST-side (persistent prop enters the world/economy) | the caught prop appears | GAP — catch commit intent |
| bait/lure state on the rod | owner plays, but the rod ITEM is a host-saved prop | consistency after save | rides prop identity; verify at build |

## 3. Coop design (DESIGN — not built)

Hook's owner-phase half, applied to a rod that NEVER graduates to world furniture:

1. Cast/minigame/reel = owner-local (the game already builds it that way — transient
   Actor, rod-back-ref, local scroll input).
2. Peers get a cosmetic cast mirror: `FishCast{on/off, buoy pos}` at low cadence in the
   per-player stream (same carrier as the hook's player-phase — one "held-item aux
   state" slot, not per-item lanes).
3. Catch = commit intent: owner resolves the bite locally (the minigame IS local skill),
   but the SPAWN goes `CatchCommit{dropRow...}` → host validates against the rod's
   bait/drop table (no client-invented loot — forgery guard) → host spawns the prop
   natively → it mirrors back via the prop lane like any prop.
   RNG policy: consistent with the events principle (client RNG suppressed, host is the
   roller) the DROP ROLL itself should be host-side — owner sends "caught at bite
   quality X", host rolls the drop map. Exact split decided at build after
   fishingRodString's uber is read (where the roll happens natively).
4. Nothing persists mid-cast (string is a plain Actor, absent from saves natively) —
   late-join during someone's cast = just the cosmetic mirror from the live stream;
   no save/EventSnapshot involvement.

## 4. Caveats

- fishingRodString uber unread — the bite timing, drop-map roll site, and
  `processDrops` semantics are census-level; MUST be read before building the catch
  intent (the anti-forgery split depends on where the game rolls).
- The rod's `test` timeline bend runs on the OWNER's item — whether a puppet's held rod
  shows bent on peers is a hold-state visual question (existing hold-state gap family),
  not a new lane.
- Water interaction (buoy floats) is physics per-viewer once mirrored — cosmetic drift
  between peers is acceptable; only position cadence matters.

## 5. Verification

2026-07-06 static RE (rod uber skimmed at spawn/cast/payout sites + fn census; string =
census only). No live probe. Sync NOT BUILT.

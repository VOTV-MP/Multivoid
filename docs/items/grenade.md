# grenade / pipebomb — timed explosives   (STATUS: RE)

The fuse family: the deployed thing is THE PROP ITSELF — no separate world actor. Arming
flips the held prop into a timer mode; the timer ends in a transient `explosion_C` spawn
(force + damage + camera shake) and self-destruction. New pattern axis vs the other item
docs: a MODE on an existing host-authoritative prop, plus a transient world-mutating
burst.

- **`prop_grenade_C` : prop_C** — `timer`/`Force`/`Damage` floats. RMB (uber @481, only
  when `timer == 0`): rename self → `grenade_1` + `init()` (the armed VARIANT identity —
  same prop, different row), spawn a plain `prop_C` named **`grenade_p`** flying off at
  rightVector×200 with 10 s lifespan (the PIN — pure cosmetics), `timer := 3.0`,
  `fuse()` loop + tick sound. Fuse (@15): `timer -= dt`; at ≤0 → deferred-spawn
  **`explosion_C`** at own transform with `force=Force`, `damage=Damage`,
  `shake=shake_explosion_C` → `explode()` → `K2_DestroyActor`. getData/loadData persist
  the timer — an armed grenade survives save/load. [bytecode]
- **`prop_pipebomb_C` : prop_C** — the placeable timed charge: `time` float,
  `active`/`timer` bools, `eff_light` blink + `timerBlip` beeping (0.5 s → 0.1 s under
  1 s left), LMB = `player.throwHoldingProp()` (@917), RMB = `activate()` + sound
  (@954). Tick (@15): `time -= dt`; at ≤0 → `explosion_C{force=2500, radius=1500,
  damage=100, debris=10, shake}` at own location → destroy. getData/loadData. [bytecode]
- **`explosion_C`** — transient burst actor (not RE'd here: applies radial force/damage,
  debris, shake [?]). The world MUTATION lives in it.

## 2. Sync-axis table

| axis | owner | peers need | carried by |
|---|---|---|---|
| the prop itself (held/thrown/world) | host (prop lane, existing) | normal prop mirror | prop lane |
| ARM action (RMB: rename/init + timer start + pin/beep cosmetics) | must run on the HOST instance (it mutates the saved prop identity + starts the authoritative fuse) | armed look + beeping + blink | GAP — arm intent today runs only where RMB happened |
| fuse countdown | host (one clock — else double explosions) | approximate beep cadence (cosmetic; can run from the armed event locally) | armed event carries the deadline |
| explosion (force/damage/debris on world) | host (props/NPCs are host-owned) | the blast visual + shake + LOCAL player damage | explosion replay event (cue-like) |
| persistence (armed timer in save) | host save (prop getData) | joiner sees armed prop from save | free once arm is host-side |

## 3. Coop design (DESIGN — not built)

The commit-intent pattern again, but the intent is an ACTION ON AN EXISTING prop
(eid-addressed), not a spawn:

1. Owner presses RMB → intent `ItemModeArm{eid}` (forgery-guarded like other prop
   intents); local arm suppressed.
2. Host runs the native RMB path on ITS instance — rename, fuse, save-visible state,
   the single authoritative countdown.
3. Host broadcasts the armed transition (clients play beeping/blink cosmetics on the
   mirror; the pin prop rides the normal host prop spawn).
4. At zero the host's native fuse spawns `explosion_C`: physics/damage to host-owned
   things happen natively there. Clients get an explosion replay event at the same
   transform for the visual/shake; per-viewer LOCAL player damage policy = same answer
   as other host-driven damage (player_damage lane owns it — decide there, not here).
5. Late-join: armed grenade arrives via host save (timer value included natively).

## 4. Caveats

- The armed grenade RENAMES itself (`grenade_1`) — identity-sensitive: our stable-ID /
  prop identity must tolerate the rename (verify against the identity map before build).
- Fuse ubers run wherever the arm ran — without the intent handoff, a client arming a
  grenade would run a CLIENT-side fuse on a host-owned prop: divergent explosion, no
  save persistence, double damage. This is the whole reason for the arm intent.
- `explosion_C` not RE'd — its damage application sites (props? player? buildings?)
  need a read before deciding what the replay event must and must not do on clients.
- Pipebomb LMB is `throwHoldingProp` — the throw itself is already the prop lane's
  concern; only `activate` needs the intent.

## 5. Verification

2026-07-06 static RE (both ubers full). explosion_C unread. No live probe. Sync NOT
BUILT.

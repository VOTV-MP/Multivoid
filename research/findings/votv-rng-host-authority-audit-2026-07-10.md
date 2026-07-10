# VOTV RNG / shared-world host-authority audit (2026-07-10)

**Trigger:** user — "пробежаться очень тщательно по всем аспектам игры, УЗНАТЬ что из систем /
из RNG мы ещё не обрабатываем в том плане что RNG должно приходить от ХОСТА ... per rule 1".
**Method:** three parallel read-only audit agents — (A) our current coop coverage
(`src/votv-coop` + docs), (B) game world/environment RNG, (C) game creature/entity/item RNG —
from the UAssetAPI bytecode dumps (`research/bp_reflection/*.json`) + `CXXHeaderDump/*.hpp`.
**Status:** AUDIT (static). The gap rows are STATIC-INFERRED (class not in our suppression set +
the game ships the roller); each needs a **live client-liveness probe** before a fix (does the
client actually roll it independently?). This doc is the roadmap, not a build.

---

## The principle (per rule 1)
**Any RNG that changes SHARED world state must be rolled by the HOST; a peer rolling its own
diverges.** Three correct shapes (all already proven in the codebase):
1. **Host-auth mirror** — host owns the roll+state, broadcasts on change; client SUPPRESSES its own
   roll/sim (mirror-pattern step 3: `time_sync` freezes the clock, `weather_sync` cancels its
   schedulers, `event_fire_sync` zeroes `allEvents.Num`, `npc_sync` cancels allowlisted spawns,
   `serverbox_sync` kills `ticker_serverBreaker`, `console_state_sync` kills the signal spawn timer).
2. **Client→host intent** — a discrete client action carrying RNG/authority is sent as an intent;
   the host performs it authoritatively (`sv.request` archetype; device_occupancy, DoorOpenRequest,
   balance-spend, signal-catch, kerfur-command).
3. **Seeded determinism** — a *seeded* `RandomStream` is reproducible if the seed is shared; replicate
   the seed instead of suppressing (only 3 game systems qualify: `garbagePileSpawner`, `radiotower`,
   `xmaslight`). Everything else is UNSEEDED global `FMath::Rand` — not reproducible.

Cosmetic-local RNG (no shared consequence) is left alone (player view-bob, footsteps, ATV fx,
flicker, fireflies-are-additive).

---

## The game's RNG topology (what actually rolls)
- **Master directors:** `daynightCycle` (236 RNG sites) + `mainGamemode` (397) — roll WHICH
  weather/anomaly/event fires + WHEN (self-rearming randomized timers), day length + timescale,
  daily-task sizes, and the sky/ground SPAWN POSITIONS of meteors/props/creatures. Plus rare
  weighted rolls: `wakeup`/`createDream`, `gen_gear`, `addHallFood`, `trySpawnInsomniac`, and —
  notably — `calculateAreaError` → `RandomBoolWithWeight[0.01] → QuitGame` (a rare RNG that ENDS the
  game).
- **~30 `ticker_*` + event spawners:** each rolls a spawn gate + position (bearing[0,360]+radius, or
  RandomPointInBoundingBox+nav-project) + a randomized re-arm interval. Creatures, wildlife, flora,
  props.
- **Signal layer:** `spaceRenderer.addSignal/gatherSignal` (signal content + whether gathered),
  `dish` calibration drift, `coordRadarDish`/`radiotower` `Array_Shuffle` scrambles.
- **Server layer:** `ticker_serverBreaker` (which server breaks), `serverBox.canBreak` +
  `getRandomServerMinigameType` (the break minigame variant), `launchServerMinigame`.
- **Loot layer:** `actorChipPile` (chipType+count+scatter), `prop_garbageClump` (break contents),
  `trashBitsPile`, `prop_food` (spoilage), `garbagePileSpawner` (SEEDED).

---

## COVERAGE — what is host-authoritative TODAY (agent A, all file:line-cited in the raw audit)
Host-auth mirror (host owns, client suppresses/mirrors): **serverbox** (IsBroken state + breaker-kill),
**scheduled/story events** (`event_fire_sync` allEvents.Num=0 + host passEvents observe), **join-event
registry** + **event cues**, **weather** rain/snow/fog/redsky/lightning (client PRE-cancels its
schedulers), **world time** (TimeScale=0), **sky/moon**, **NPC+kerfur+pyramid AI** (allowlist spawn
suppress + AI-timer neutralize + pose one-way), **wisp aggro**, **drone**, **turbine**, **garbage
spawner bodies** (client cancels its spawner), **sky-signal GENERATION** (`console_state_sync` kills
`spaceRenderer` spawn timer), **balance/wallet**. Host-mediated intent: **alarm**, **signal-catch**,
**device-occupancy**, **comp**, **kerfur-commands**, **balance-spend**. Convergent-by-design
(peer-symmetric but safe): **power breakers**, **saved-signals CRDT**, **window/grime min-wins**,
**email-delete**, **doors**. Host-only email APPEND (this session). 

**The genuine "world RNG, each peer rolls independently" BUG class was measured at N=1 (email append,
now host-gated) — for the systems we ALREADY touch.** The gaps below are systems we **don't touch at
all**.

---

## GAPS — ranked (systems with NO host-authority; STATIC-INFERRED, probe before fixing)

### TIER 1 — gameplay divergence (peers see different world; fix first)
1. **Uncovered creature/threat spawners** — the `npc_sync` allowlist covers **15 classes**
   (zombie, kerfurOmega, krampus, funguy, goreSlither, insomniac, fossilhound, antibreather, orborb,
   arirFollower, ariralShooter, ariralPigBeater, killerwisp, ventCrawler, wisp) + piramid2 + arirShip.
   The game ships **~20 more spawners NOT covered** → each peer's ticker rolls independently:
   `ticker_deerSpawner`, `ticker_mannequinSpawner`, `ticker_hexahiveSpawner`, `ticker_eyers`,
   `ticker_bp7Spawner`, `ticker_roachSummoner`, `grayBoarSpawner`/`boarInvasion`, `ghostcarSpawner`,
   `bunnySpawner`, `bloodSkeletonSpawner`, `arirBusterSpawner` (fakeRadarWalker), `greenFireSpawner`,
   `furfurAltarSpawner`, `hillRollerSpawner`, `ufoDropper` payloads, `blackballSpawner`(orborb→
   covered), `ticker_yellowWispSpawner`→killerwisp (covered class, but the SPAWNER's own roll?).
   → **Root question: allowlist (add each) vs structural (client runs NO world-spawn ticker).**
2. **`mainGamemode` rare weighted rolls** — most dangerous: `calculateAreaError` →
   `RandomBoolWithWeight[0.01] → QuitGame`. If the client's `mainGamemode` ubergraph rolls this
   independently, ONE peer can quit-to-menu while the other continues. Also `wakeup`/`createDream`
   (dream/story state), `gen_gear`, `trySpawnInsomniac`, sky/meteor spawn positions. **Must confirm
   whether the client's `mainGamemode` roll-machinery is live or already frozen.**
3. **Server minigame type** — `getRandomServerMinigameType` / `launchServerMinigame`
   (`RandomBoolWithWeight[0.005]/[0.2]`). `serverbox_sync` mirrors IsBroken STATE but not the minigame
   VARIANT a player faces on fix/interact. If two peers interact with the same server, the minigame
   type can differ. (Intent-shape: the fix interaction → host rolls the variant.)
4. **Loot content RNG** — `actorChipPile.chipType`+count, `prop_garbageClump` break contents,
   `trashBitsPile`. The trash-pile IDENTITY rides our host-auth element engine, but the CONTENT roll
   (what a pile/clump yields) must be host-owned or the two peers see different loot. **Confirm the
   host owns the chipType roll.**

### TIER 2 — world-consistency (shared but lower stakes)
5. **Signal calibration/scramble** — `dish` calibration drift (`RandomFloatInRange` losePrec),
   `coordRadarDish`/`radiotower` periodic `Array_Shuffle` scramble, `ticker_dishUncalib`/`ticker_disher`.
   Sky-signal GENERATION is host-auth (console_state_sync), but the dish/tower drift+scramble aren't →
   two peers' dish calibration + radar order diverge. (This is near the user's **`sv.request`** area —
   the signal request/detect flow: catch is host-mediated (`signal_catch_sync`), generation is
   host-auth, but drift/scramble is unhandled.)
6. **Ambient wildlife/flora spawners** — `ticker_beehiveSpawner`, `ticker_treeSpawner`
   (walkingTree), `ticker_bushSpawning` (growingPlant), `ticker_susHoleSpawner`, `mushroomMaster`,
   `pineconeSpawner`/`birchSpawner`/`autumnLeafSpawner`. Shared world (real actors), but lower gameplay
   stakes — each peer currently grows its own trees/mushrooms. Sync or accept-as-cosmetic is a
   product call.
7. **Seeded-stream determinism opportunity** — `garbagePileSpawner`, `radiotower.generateGizmos`,
   `xmaslight`: replicate the `seed` member host→client and both roll identically WITHOUT suppression.
   The one place shape-3 (seed sync) beats suppress+mirror.

### TIER 3 — cosmetic-local (leave alone)
Player view-bob/footstep/voice jitter (`mainPlayer`), ATV exhaust/backfire fx, `ticker_flickerer`
light flicker, `firefly_sync` (peer-symmetric but additive→union), `xmaslight` pattern (unless
shared-décor matters). No shared consequence.

---

## Root-cause recommendation (the rule-1 decision → needs a /qf design pass)
The `npc_sync` **allowlist inherently lags** (15 of ~40 spawn classes). The proper root is a
**structural rule**: *the client runs NO autonomous world-spawn ticker; the host owns every world
spawn and the client mirrors via the entity-sync engine* — i.e. generalize mirror-step-3 from a
per-class allowlist to the **ticker/spawn-director layer** (suppress the client's `ticker_base`
spawn dispatch wholesale; keep the allowlist only as the MIRROR set). Plus:
- **Seed replication** for the 3 seeded systems (cheap, exact).
- **Intent** for the per-interaction rolls (server minigame type on fix).
- **Confirm-then-freeze** `mainGamemode`/`daynightCycle` client roll-liveness (esp. the QuitGame roll).

This is a **big blast-radius** change (wholesale client spawn suppression could hide a needed local
actor). It MUST go through a `/qf` QUESTION pass first — the gating measurement is a **live probe**:
run a LAN session, and on the CLIENT log every `BeginDeferredActorSpawnFromClass` + every
`mainGamemode`/`daynightCycle` RNG-consuming vf call that is NOT already suppressed. That tells us
which rollers ACTUALLY fire on the client today (the static "not in allowlist" list is the upper
bound, not the confirmed set).

## Direct answers to the user's examples
- **Servers dying** — HANDLED (serverbox mirror: host owns IsBroken, client kills its breaker).
  RESIDUAL: the server **minigame type** (Tier-1 #3) is an uncovered per-interaction roll.
- **`sv.request`** — the signal request/detect flow: the CATCH is host-mediated (`signal_catch_sync`,
  shape 2) and sky-signal GENERATION is host-auth (`console_state_sync`), so the core is covered;
  the RESIDUAL is dish calibration drift + radar/tower scramble (Tier-2 #5), which still roll per-peer.

## Honesty / what is NOT yet measured
Every Tier-1/2 row is STATIC-INFERRED (class absent from our suppression set + the game ships the
roller). Whether the CLIENT actually rolls each one depends on whether the client's ticker system /
`mainGamemode` roll-machinery is live — which is NOT yet probed. The live client-side roll census is
the first step before any build. No fix is justified until that probe confirms the diverging set.

# COOP_RNG_AUTHORITY.md — living RNG host-authority tracker

**Purpose.** The single living home for "which of VOTV's RNG / non-deterministic systems are
host-authoritative in coop, which still diverge, and the progress of closing each gap." This is the
TRACKER (update it every session); the point-in-time audit snapshot is
`research/findings/votv-rng-host-authority-audit-2026-07-10.md` (the evidence + file:line cites).

**How to use.** Each RNG system has a STATUS. Work proceeds **tier by tier**, each tier gets its own
`/qf 15` (QUESTION → DESIGN → IMPL) **before** any build (user directive 2026-07-10). Update the
row STATUS + the per-tier progress block as work lands. Never mark a row VERIFIED without a real
hands-on or a matching live log (docs-piles discipline).

---

## The principle (per rule 1)
**Any RNG that changes SHARED world state must be rolled by the HOST; a peer rolling its own
diverges.** Three correct shapes:
1. **MIRROR** — host owns roll+state, broadcasts on change; client SUPPRESSES its own roll (mirror
   step 3). Precedents: time_sync, weather_sync, event_fire_sync, npc_sync, serverbox_sync,
   console_state_sync.
2. **INTENT** — discrete client action → host performs the RNG-bearing action authoritatively
   (`sv.request` archetype; device_occupancy, DoorOpenRequest, signal-catch, balance-spend).
3. **SEED** — a *seeded* `RandomStream` is reproducible if the seed is shared; replicate the seed
   (only garbagePileSpawner, radiotower, xmaslight qualify). All other game RNG is unseeded → not
   reproducible.
Cosmetic-local RNG (no shared consequence) is LEFT ALONE.

**STATUS legend:** `DONE-MIRROR` / `DONE-INTENT` / `CONVERGENT` (peer-symmetric but safe by design)
· `OPEN-DIVERGES` (needs a shape) · `NEEDS-PROBE` (static-inferred; confirm client-liveness first)
· `COSMETIC-LOCAL` (leave) · `SEED-OPP` (seed-replication opportunity).

---

## PROGRESS DASHBOARD (update each session)

| Tier | scope | rows | DONE | OPEN | /qf QUESTION | /qf DESIGN | /qf IMPL | build |
|---|---|---|---|---|---|---|---|---|
| **T1** | gameplay divergence | 4 groups | 0 | 4 | ⬜ not started | ⬜ | ⬜ | ⬜ |
| **T2** | world consistency | 3 groups | 0 | 3 | ⬜ | ⬜ | ⬜ | ⬜ |
| **T3** | cosmetic-local | — | n/a (leave) | — | — | — | — | — |
| **SEED** | seed replication | 3 | 0 | 3 | ⬜ | ⬜ | ⬜ | ⬜ |
| **prior** | already host-auth | ~30 | ~30 | 0 (email N=1 closed) | — | — | — | ✅ shipped |

**Gating measurement for ALL of T1/T2 (do FIRST, next session):** a LIVE client-side roll probe —
log every un-suppressed `BeginDeferredActorSpawnFromClass` + `mainGamemode`/`daynightCycle`
RNG-consuming vf call that fires on the CLIENT during a LAN session. Converts every NEEDS-PROBE row
below into a measured OPEN or a struck row. The static list is the UPPER BOUND, not the confirmed set.

**Recommended entry point (next session):** Tier 1 → the **structural-suppression vs allowlist**
root decision (it unlocks the whole spawner group at once) is the first `/qf 15` QUESTION pass; the
live probe is that pass's gating measurement. The `calculateAreaError → QuitGame` roll (T1-2) is
small + scary enough to verify on its own in parallel.

---

## TIER 1 — gameplay divergence (peers see a different world)

### T1-1 · Uncovered creature/entity spawners — STATUS: NEEDS-PROBE / OPEN-DIVERGES
`npc_sync` allowlist covers 15 (zombie, kerfurOmega, krampus, funguy, goreSlither, insomniac,
fossilhound, antibreather, orborb, arirFollower, ariralShooter, ariralPigBeater, killerwisp,
ventCrawler, wisp) + piramid2 + arirShip. UNCOVERED spawners (each ticker rolls per-peer):

| spawner | spawns | RNG decides | status |
|---|---|---|---|
| ticker_deerSpawner | deer_C | gate[0.02] + pos + variant | NEEDS-PROBE |
| ticker_mannequinSpawner | wMannequinSpawn_C/prop | Array_Shuffle+Random which+where | NEEDS-PROBE |
| ticker_hexahiveSpawner | hexahive | gate[0.02] + pos | NEEDS-PROBE |
| ticker_eyers | eyer_C | gate[0.05] + pos (night) | NEEDS-PROBE |
| ticker_bp7Spawner | bp7 | branch coin + pos | NEEDS-PROBE |
| ticker_roachSummoner | roach | timing + pos | NEEDS-PROBE |
| grayBoarSpawner / boarInvasion | grayboar_C, ariral_shooter, firetank | count+pos+variant | NEEDS-PROBE |
| ghostcarSpawner | ghostcar_C | pos + yaw jitter | NEEDS-PROBE |
| bunnySpawner | ominousBunny/superAngryBunny | which variant + pos | NEEDS-PROBE |
| bloodSkeletonSpawner | blood skeleton | pos + timing | NEEDS-PROBE |
| arirBusterSpawner | fakeRadarWalker_C | pos + timing | NEEDS-PROBE |
| greenFireSpawner | greenfire_C | pos + timing | NEEDS-PROBE |
| furfurAltarSpawner | paranormalSpot_C | pos + timing | NEEDS-PROBE |
| hillRollerSpawner | prop/propThrown/nail | which prop + pos | NEEDS-PROBE |
| ufoDropper | kerfurOmega/fallingBody/lampPost | Array_Random which drop + where | NEEDS-PROBE |
| ticker_yellowWispSpawner | killerwisp_C (covered class) | spawner's own gate+pos | NEEDS-PROBE |
**Root decision (the /qf):** broaden the allowlist (add each) vs STRUCTURAL — client runs NO
world-spawn ticker; host owns all spawns; allowlist becomes only the MIRROR set. Structural is the
rule-1 root (allowlist inherently lags 15/40).

### T1-2 · mainGamemode rare weighted rolls — STATUS: NEEDS-PROBE / OPEN-DIVERGES
| roll | consequence | status |
|---|---|---|
| calculateAreaError `RandomBoolWithWeight[0.01]` → **QuitGame** | one peer force-quit to menu | NEEDS-PROBE (HIGH — verify standalone) |
| wakeup / createDream `[0.25]/[0.5]` | dream/story state | NEEDS-PROBE |
| gen_gear `[0.1]`, addHallFood, trySpawnInsomniac `[0.001]` | ambient spawns | NEEDS-PROBE |
| sky/meteor/ground spawn positions (RandomFloat[50000,65000] → BeginDeferredSpawn) | where sky objects land | NEEDS-PROBE |
**Question:** is the client's `mainGamemode` roll-machinery live or already frozen (time_sync /
event zeroing may or may not stop these)? The probe answers it.

### T1-3 · Server minigame type — STATUS: OPEN-DIVERGES
serverbox_sync mirrors IsBroken STATE but not `getRandomServerMinigameType` /
`launchServerMinigame[0.005]/[0.2]` — the break-minigame VARIANT a player faces. Two peers on the
same server can get different variants. Shape: INTENT (fix interaction → host rolls variant).

### T1-4 · Loot content RNG — STATUS: NEEDS-PROBE
`actorChipPile` chipType+count+scatter; `prop_garbageClump` break contents; `trashBitsPile`;
`prop_food` spoilage. Trash-pile IDENTITY is host-auth (element engine) but the CONTENT roll may
still be per-peer. Confirm the host owns the chipType roll.

---

## TIER 2 — world consistency (shared, lower stakes)

### T2-5 · Signal calibration / scramble — STATUS: OPEN-DIVERGES  (the `sv.request` residual)
Sky-signal GENERATION host-auth (console_state_sync); CATCH host-mediated (signal_catch_sync). RESIDUAL:
`dish` calibration drift (RandomFloat losePrec), `coordRadarDish`/`radiotower` periodic `Array_Shuffle`
scramble, `ticker_dishUncalib`/`ticker_disher`. Two peers' dish calibration + radar order diverge.

### T2-6 · Ambient wildlife / flora spawners — STATUS: OPEN-DIVERGES (product call: sync vs accept-cosmetic)
ticker_beehiveSpawner, ticker_treeSpawner (walkingTree), ticker_bushSpawning (growingPlant),
ticker_susHoleSpawner, mushroomMaster, pineconeSpawner/birchSpawner/autumnLeafSpawner. Real shared
actors, low gameplay stakes — each peer grows its own.

### T2-7 · Seed-replication opportunity — STATUS: SEED-OPP
`garbagePileSpawner` (garbage layout+types), `radiotower.generateGizmos` (décor), `xmaslight`
(pattern) use a SEEDED stream. Replicate the `seed` member host→client → identical rolls, no
suppression. Cheapest exact fix; shape-3.

---

## TIER 3 — cosmetic-local (LEAVE ALONE)
mainPlayer view-bob/footstep/voice jitter; ATV exhaust/backfire fx; ticker_flickerer; firefly_sync
(peer-symmetric but additive→union); xmaslight pattern (unless shared-décor matters); footstep anim
notifies; weatherFogController / newsky / spaceRenderer visual params; grime/greenfire/campfire fx.
No shared consequence → forcing host-auth would be gratuitous (rule-1: not a bug).

---

## PRIOR — already host-authoritative (reference; DONE)
serverbox (IsBroken + breaker-kill), scheduled/story events (event_fire_sync), join-event registry
+ cues, weather rain/snow/fog/redsky/lightning, world time (TimeScale=0), sky/moon, NPC+kerfur+
pyramid AI (allowlist + AI-neutralize + pose one-way), wisp aggro, drone, turbine, garbage-spawner
bodies, sky-signal generation, balance/wallet, email APPEND (host-gate, 2026-07-09). INTENT: alarm,
signal-catch, device-occupancy, comp, kerfur-commands, balance-spend. CONVERGENT-by-design: power
breakers, saved-signals CRDT, window/grime min-wins, email-delete, doors. The "we touch it but each
peer rolls" BUG class = N=1 (email append) → CLOSED.

---

## CHANGELOG
- **2026-07-10** — doc created. 3-agent audit → tiers seeded. All T1/T2 rows NEEDS-PROBE/OPEN.
  Next session: live client-roll probe → then `/qf 15` per tier (start T1 structural-suppression).

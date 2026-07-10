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
| **T1** | gameplay divergence | 4 groups | 0 | 4 | ✅ converged 2026-07-10 (11 rounds; PRE-REGISTRATION below) | ✅ converged 2026-07-10 eve (6 rounds; ratified DESIGN below) | ✅ **Inc-1 AS-BUILT `e6c1371b`** (insomniac+fossilhound parked, ambient dissolved into coop/world/spawn_authority, tripwire live; VERIFY-gate passed by census; Inc-2 families next) | ✅ **probe v9 BUILT+DEPLOYED** `7109efd1` + exposure run 2026-07-10 pm (host full day; client 18-min slice — idle death); 2 live + 4 armed + 1 DONE; signal still points STRUCTURAL |
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
| ticker_eyers | eyer_C | gate[0.05] + pos (night) | **DONE-OWNER-ENTITY** (2026-07-10 eve, USER RULE: each peer keeps its OWN native eyer -- the stalker loop (isLooking/angrify/dash) reads the LOCAL player -- but every peer SEES it: `coop/creatures/owner_entity_sync` (OwnerEntitySpawn/Pose/Destroy 94-96, (slot,seq) identity, brain-parked collision-off mirrors, 10 s keepalive = late-join delivery). The TICKER is never parked. F1 Content > Entities "Spawn Eyer" = the lane test. **AS-BUILT caveat (audit F-2): the NATIVE ticker roll's PE-visibility is UNPROVEN** -- the F1 button re-enters ProcessEvent by construction, so it cannot discriminate the axis ([[lesson-e2e-assert-must-discriminate-the-axis]]); the row goes VERIFIED only on a live `owner_entity: OWN 'eyer_C' spawned locally` from a NATURAL/night roll. Fallback if the ticker is EX_CallMath: +1 npc_world_enum Func-thunk source row) |
| ticker_bp7Spawner | bp7 | branch coin + pos | NEEDS-PROBE |
| ticker_roachSummoner | roach | timing + pos | **DONE-MIRROR** (2026-07-10 eve, v108 roach lane: roaches = COMPONENTS on the one cockroachMaster (not actors); client sim parked (t1+t3 rows) + host paged RoachState snapshot + RoachConsumed eat/stomp intents — `coop/creatures/roach_sync`) |
| grayBoarSpawner / boarInvasion | grayboar_C, ariral_shooter, firetank | count+pos+variant | NEEDS-PROBE |
| ghostcarSpawner | ghostcar_C | pos + yaw jitter | NEEDS-PROBE |
| bunnySpawner | ominousBunny/superAngryBunny | which variant + pos | NEEDS-PROBE |
| bloodSkeletonSpawner | blood skeleton | pos + timing | NEEDS-PROBE |
| arirBusterSpawner | fakeRadarWalker_C | pos + timing | NEEDS-PROBE |
| greenFireSpawner | greenfire_C | pos + timing | NEEDS-PROBE |
| furfurAltarSpawner | paranormalSpot_C | pos + timing | NEEDS-PROBE |
| hillRollerSpawner | prop/propThrown/nail | which prop + pos | NEEDS-PROBE |
| ufoDropper | kerfurOmega/fallingBody/lampPost | Array_Random which drop + where | NEEDS-PROBE |
| ticker_yellowWispSpawner | killerwisp_C (covered class) | spawner's own gate+pos | **DONE-MIRROR** (2026-07-10 correction: the client ReceiveTick cancel — now a t3 row in coop/world/spawn_authority.cpp, `e6c1371b`; wisp class allowlisted) |
**Root decision (the /qf):** broaden the allowlist (add each) vs STRUCTURAL — client runs NO
world-spawn ticker; host owns all spawns; allowlist becomes only the MIRROR set. Structural is the
rule-1 root (allowlist inherently lags 15/40).

**2026-07-10 fact-base correction (the audit missed a shipped module):**
the ambient module (dissolved 2026-07-10 into `src/coop/world/spawn_authority.cpp` `e6c1371b`) ALREADY client-cancels 4 rollers via per-class
PE-visible fn PRE-interceptors (mushroomMaster.Spawn, mushroomSpawner.Spawn,
pineconeSpawner.ReceiveTick, ticker_yellowWispSpawner.ReceiveTick), and `host_spawn_watcher.cpp`
MIRRORS the pinecone-family outputs (keyless PropSpawn + death-watch) — the full suppress+mirror
shape is SHIPPED for ambient flora. ~~Color wisps (ticker_wispSpawner) are deliberately per-peer
(product decision, comment in that file).~~ **SUPERSEDED 2026-07-10 eve (anchor audit): BOTH calls
flipped.** pineconeSpawner is PLAYER-CAMERA-anchored (dump-measured) → OWNER-EFFECT: the cancel row
was REMOVED and the ambient broadcaster went peer-symmetric (each peer rolls + mirrors its own
drops). ticker_wispSpawner is ABSOLUTE-map-coords-anchored (dump-measured, ±60-70k X/Y Z=80k) →
world-anchored: now a t3 cancel row + host mirror (wisp_C + 8 color variants allowlisted,
ticker_wispSpawner EX-catch source row). Consequences: (i) mechanism precedent =
fn-cancel-at-the-class, the structural option generalizes it at the driver layer; (ii) the module
sits in the WRONG folder (coop/session/ is not spawn-authority) — the move is owed to the T1 design;
(iii) the probe analysis must JOIN against this module's targets + the npc allowlist (from live
source, at analysis time) so cancelled classes read DONE, not false-live.

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

### T2-6 · Ambient wildlife / flora spawners — STATUS: PARTIAL (2026-07-10 eve anchor audit)
**DONE-MIRROR (host-auth):** mushroomMaster, mushroomSpawner (own-transform world spawns; t3
cancels + host mirror), **ticker_wispSpawner sky wisps** (2026-07-10 eve REVERSAL: dump-measured
ABSOLUTE map coords, not player-anchored — t3 cancel row + wisp_C+8 variants allowlisted + EX-catch
source row). **DONE-OWNER-EFFECT (peer-symmetric):** pineconeSpawner forest drops (2026-07-10 eve
REVERSAL: dump-measured PLAYER-CAMERA anchor — cancel row removed; each peer rolls its own,
host_spawn_watcher broadcasts peer-symmetrically with ScopedMirrorSpawn echo guard + mirror
SetLifeSpan(900) orphan backstop). **Still OPEN / NEEDS-PROBE:** ticker_beehiveSpawner,
ticker_treeSpawner (walkingTree), ticker_bushSpawning (growingPlant), ticker_susHoleSpawner,
birchSpawner/autumnLeafSpawner (leaves = the OWNER-EFFECT candidate still lacking an anchor read).
Real shared actors, low gameplay stakes.

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

## T1 PRE-REGISTRATION (2026-07-10 — the /qf 15 QUESTION pass converged; written BEFORE the probe build)

**The probe instrument (v9)** — ini-gated `[dev]`, read-only, both roles, resident:
- **(a)** unfiltered BeginDeferredActorSpawnFromClass pass-through census at the existing npc_sync
  silent-pass point — class + location, role-stamped, JOIN-EPISODE-tagged (see gates below);
  per-class summaries inside the join window.
- **(b)** driver-census: native Func-patch on K2_SetTimerDelegate, K2_SetTimer, Delay,
  RetriggerableDelay, DelayFrames, SetActorTickInterval (engine natives, patched at boot →
  client-side install-before-arm holds: a client's world loads AT JOIN, no pre-armed state).
  First-sight-per-class + relaxed atomic counters (pointer-hash flat set; allocation-free steady
  state; NameOf only on first sight); self-instrumented overhead counters; TLS coop-origin latch
  excludes our own machinery's arms; role-stamped; JOIN-EPISODE-tagged.
- **(c)** PERIODIC (~10 min) one-shot censuses of live ticker_base_C descendants: existence +
  tick-enabled + tick interval.
- **(d)** global QuitGame native log (7 sites measured in mainGamemode: 6 ubergraph +
  1 calculateAreaError body; the native patch is global so any other asset's call is also caught).
  Caveat: static-library Func context = the library CDO, NOT the caller → attribution by timing at
  analysis (autonomous smokes contain no user-initiated quits).
- **(e)** PERIODIC paired world class-histogram censuses as per-actor (class, ptr, InternalIndex)
  SETS, diffed actor-wise, wire-explained actors subtracted per-actor via the mirror registries —
  the seam-agnostic backstop (catches native-path spawns; count cancellation impossible).

**Semantics (pre-registered):**
- Positive assertions come from ARMED-state + driver calls + histogram residue — never from
  fire-absence.
- Records inside the world-load episode (world_load_episode latch) are JOIN-EPISODE and EXCLUDED
  from fork arithmetic (fail-safe: a stuck latch over-tags, which can never manufacture a positive;
  a fully-tagged log is visibly broken and the run repeats).
- Suppressed-set JOIN at ANALYSIS time against the LIVE source (kNpcAllowlist +
  ambient_spawner_suppress targets) — nothing hand-copied into the probe.
- Streaming admissibility: (a)/(b)/(d) stream to the log; a crash leaves the last completed (c)/(e)
  pair + streamed records as a PARTIAL census. POSITIVES always admissible; ABSENCE claims require
  the full exposure.
- **Shared-world definition:** the spawned class is a physical world actor a peer can see/interact
  with (creature, prop, vehicle, structure); excludes UI widgets, fx/sound-only, camera/viewmodel-
  local. All 16 T1-1 table rows above classify Y (shared-world) under this definition. Definition
  changes require a CHANGELOG entry — no silent reclassification.
- **Exposure minimum:** >= 1 full in-game day-night cycle on both peers + a standard join, at
  NATURAL 1x time — clock acceleration FORBIDDEN for census runs (it decouples the real-second
  delay axis from the game-clock gate axis). Shorter runs = PARTIAL census; phase-gated rows stay
  CONDITIONAL.
- Under-exposed / event-gated / dormant-on-both-roles rows are CONDITIONAL and EXCLUDED from the
  fork arithmetic (they never count toward "targeted").

**The fork rule (pre-registered):**
- **>= 3 distinct** client-side positively-measured live un-suppressed shared-world spawner classes
  → **STRUCTURAL** (client runs no autonomous world-spawn ticker; allowlist becomes the mirror set).
- **<= 2**, both event-gated rarities → targeted per-class adds.
- Positively-measured < 3 while many rows remain CONDITIONAL → **DEFER** (extend resident
  accumulation) — no default to targeted.
- **Callable-check:** the fork may be called only when >= 2/3 of the pre-registered shared-world
  rows are adjudicated non-CONDITIONAL; otherwise a longer run is mandated. (Exclusion can only
  push toward targeted/DEFER, so a mostly-CONDITIONAL census must not be allowed to call the fork.)
  **Pinned arithmetic:** denominator = the 16 T1-1 rows as listed at pre-registration; the
  adjudication vocabulary is live / armed / confirmed-starved / **DONE-suppressed** (a class the
  analysis-time join marks already-suppressed — e.g. yellowWisp — counts as adjudicated
  non-CONDITIONAL). No analysis-time re-basing of the denominator.
- Orthogonal lanes regardless of fork: any client-side QuitGame liveness → freeze that mainGamemode
  roll lane; the seeded-3 (garbagePileSpawner, radiotower, xmaslight) → seed replication (T2-7).

**Named accepted blind spot:** native-path AND transient-lifetime actors. Static argument: every
audited VOTV spawner is BP-authored; no native transient spawner is evidenced. If the pre-work
dumps reveal a spawn native or re-arm driver OUTSIDE the BeginDeferred family / the 6 patched
natives, the pre-registered response is **AMEND THE INSTRUMENT BEFORE BUILD** (widen (a)/(b)) —
never silently file a KNOWN row under the blind spot.

**RULE-2 pre-commitment:** if the fork lands STRUCTURAL, ambient_spawner_suppress's 4 per-class
cancels dissolve into the structural suppress set in the SAME change (no parallel suppress paths);
the mirror halves (host_spawn_watcher pinecone lane) stay. The module's folder move (out of
coop/session/ into the spawn-authority home) is owed REGARDLESS of the fork.
**PRODUCT-EXEMPTION list (STRUCTURAL must not silently overrule it):** ~~ticker_wispSpawner color
wisps stay PER-PEER~~ **RETIRED 2026-07-10 eve** — the exemption assumed player-anchoring the
bytecode refutes (absolute map coords); color/sky wisps moved to the cancel+mirror set with the
user's green light. The exemption CONCEPT stays: the current exemption set is the OWNER-EFFECT
tier (fireflies, pinecone drops — dump-measured player anchors), and extending/shrinking it is a
DESIGN-pass decision with the user, never an implementation-time default.

**Instrument scope (pinned):** channels (a)-(e) adjudicate **T1-1 and T1-2** (spawns, drivers,
quits, histograms). **T1-3** (server minigame type) is ALREADY adjudicated statically: the roll is
per-interaction, serverbox_sync does not touch it → OPEN by construction, shape = INTENT (no probe
needed). **T1-4** (loot content: chipType/count, garbageClump contents, spoilage) lands as
property writes on existing actors — INVISIBLE to (a)-(e); its adjudication path is its own static
RE (pre-work gate 5: who rolls chipType, at which seam, on which role) + a targeted follow-up
channel or hands-on diff if the RE leaves it open. The tier's probe does NOT silently cover it.

**Static pre-work gates (execute before the probe build):**
1. Dump + classify cockroachMaster, propSpawner_editor, prop_notebook_busterSpawner — BOTH the
   spawn native AND the re-arm driver (a fifth mechanism widens (b) first).
2. Locate gen_gear (not in mainGamemode exports; likely daynightCycle).
3. Extract per-spawner interval/delay literals from the 28 ticker dumps (sizes expected
   fires-per-cycle per row — feeds the callable-check; non-gating for liveness).
4. wakeup/createDream trampoline-offset segment walk (attribution only; consequence observability
   already covered by (a)/(b)/(d); also decide their PLAYER-LOCAL reclassification in the DESIGN
   pass — dreams are personal experience, plausibly not shared world state).
5. T1-4 loot-content RE: who rolls chipType/count (actorChipPile), garbageClump break contents,
   prop_food spoilage — which function, which seam, which role runs it in coop. Adjudicates T1-4
   (outside the (a)-(e) instrument's scope — see Instrument scope above).

---

## T1 PROBE v9 — AS-BUILT + FIRST PARTIAL CENSUS (2026-07-10)

**AS-BUILT:** `coop/dev/rng_roll_census.{h,cpp}` (ini `[dev] rng_roll_census=1`), wired via
subsystems Install/Tick + npc_sync pass-through notes. SEAM CORRECTION measured by the probe's own
positive control on the first smoke: the driver natives are EX_CallMath-invoked → the PE-detour
interceptor table was BLIND (zero records against 973 PE-visible BeginDeferreds); channels (b)+(d)
were moved to the ufunction_hook FUNC-PATCH seam (capacity raised 16→40), where
sourceObject=FFrame::Object gives free caller attribution. Coop-origin via a TLS depth latch in
reflection::CallFunction. All 5 static pre-work gates CLOSED: the 3 undumped chains are
BeginDeferred + known drivers (no instrument amendment); gen_gear lives on gearer_C (interaction
content roll → gate-5 family, not a spawner row); interval literals extracted (mannequin 1-3 h,
hexahive 40-60 min, eyers 30-45 min, deer 60-300 s; greenFire/furfurAltar/hillRoller/arirBuster/
propSpawner_editor have NO drivers in own bytecode = externally-triggered CONDITIONAL);
wakeup expresses through BeginDeferred (visible to (a)), createDream entry is state-only;
chipType is ALREADY wire-stamped host-auth at spawn/convert — T1-4 residue = per-interaction rolls
(turnIntoScrap ×135, clump crowbarOpen) → INTENT family + food spoilage → the
COOP_WORLD_PROP_DIVERGENCE class.

**FIRST PARTIAL CENSUS (150 s LAN smoke, 2026-07-10 12:48 — PARTIAL per the exposure rule;
positives admissible, fork NOT formally callable yet at ~9/16 adjudicated < 2/3):**
- **LIVE client-side steady-state rolling (episode=0, coop=0):** `mainGamemode_C` 157 Delay arms +
  1 SetTimerDelegate (the master director's machinery — incl. the QuitGame lane — runs on the
  CLIENT at the host's rate 188) · `ticker_dishUncalib_C` 11 TickInterval re-arms (dish-scramble
  drift rolls independently; host 11 — same rate = parallel divergence) ·
  `ticker_insomniacSpawner_C` 26 · `autumnLeafSpawner_C` live · `prop_food_mushroom_C` 4462
  steady TickInterval re-arms (food-spoilage self-sim, the divergence-class live confirmation).
- **ARMED on the client** (one-shot load arms + census (c) tickEnabled=1, awaiting their 30 min-3 h
  cycles): deerSpawner, mannequinSpawner, hexahiveSpawner, bp7Spawner, treeSpawner, susHoleSpawner,
  bushSpawning, eyers, fossilhoundSpawner, roachSummoner.
- **DONE-suppressed corroborated:** serverBreaker tickEnabled=0; yellowWisp/pinecone cancelled at
  the fn body (analysis join marks DONE).
- QuitGame: zero (channel armed, global).
- **Partial verdict: every measured row points STRUCTURAL** (far more than 3 shared-world classes
  armed/live). Formal fork call = the full 1x day-night exposure run (the probe is resident;
  any long organic session with the ini flag on accumulates it).

## T1 SECOND (EXPOSURE) CENSUS — 2026-07-10 afternoon: formal fork call = **NOT CALLABLE → DEFER**

**The run:** 95-min LAN session (host: full 75-min in-game day at 1x + margin; `maxTime=4500 s`
measured from the daynightCycle CDO). **Client exposure came in SLICES, not a full day:** the idle
autonomous client DIED in-world at 14:15 (~18-min life, permadeath → menu; pre-death log shows
`prop_burgerHallucinate_C` spawning — cause unadjudicated), and the relaunched second client's join
ABORTED during the save transfer ("Remote player left" hit it mid-transfer — likely the kill of the
old menu client racing the fresh join on the recycled slot; candidate join-window bug, OPEN). The
first life's raw log was then TRUNCATED by the relaunch (the DLL truncates votv-coop.log at boot —
[[lesson-copy-peer-log-before-relaunch]]); its evidence survives as the 14:55 analyzer aggregates.

**Adjudication (life-1 aggregates via `tools/rng_census_analyze.py` — the pre-registered
suppressed-set JOIN + spawner-vs-product evidence rules live IN the script):**
- **live (client, coop=0, episode=0): deerSpawner** (ticker re-arms outside the join episode),
  **bp7Spawner** (9 TickInterval re-arms) — 2 rows.
- **armed:** mannequinSpawner, hexahiveSpawner, eyers, roachSummoner (tickEnabled=1; their
  30 min–3 h cycles exceed the 18-min client life) — 4 rows.
- **DONE-suppressed:** yellowWispSpawner (suppress-join; its BeginPlay-time arm record is visible
  but the fn body is cancelled — the analyzer flags arm-only evidence on a suppressed class).
- **CONDITIONAL:** the remaining 9 (4 static event-triggered per gate 3; 5 with no records —
  grayBoar/ghostcar/bunny/bloodSkeleton/ufoDropper never appeared in the world census).
- QuitGame: zero records on either peer (channel armed, global).

**Formal arithmetic (pinned):** 7/16 adjudicated non-CONDITIONAL < the 11/16 (≥2/3) callable
threshold → **the fork may NOT be called. Pre-registered consequence: DEFER + extend resident
accumulation.** (Fork inputs would also be only 2 live < 3.) The probe stays resident
(`rng_roll_census=1` in HOST+CLIENT_1 inis); the user's organic sessions accumulate client
exposure; the armed 4 firing within a longer client life settles it. **The T1 DESIGN pass stays
gated on the fork call** — the partial signal still points STRUCTURAL, but the gate exists
precisely so nothing gets built on "points".

**Exposure reality for future runs [SUPERSEDED 2026-07-10 night]:** the ~18-min idle death was
ROOT-FIXED (starvation; `[dev] vitals_keepalive_sec=180`, commit `0211b9c5`) — autonomous peers now
survive indefinitely (65-min continuous run measured, both peers alive at kill). Log-copy-before-
relaunch still applies ([[lesson-copy-peer-log-before-relaunch]]).

## T1 THIRD (KEEPALIVE) CENSUS — 2026-07-10 evening: fork = **STRUCTURAL** (user-called; callable gate waived)

**The run:** 65-min continuous LAN session (17:58–19:03), both peers alive the WHOLE time — the
idle-death blocker was ROOT-FIXED first: the new `[dev] vitals_keepalive_sec=180` ticker
(`0211b9c5`, DLL `7CBC7122`) measured the cause as STARVATION (harness save starts food=24.4;
idle drain ~2.3 food/min) and pins vitals at 100 via the proven restore_vitals path (host refill
+ RestoreVitals broadcast). Orchestrated by scratchpad `census_overnight.py` (log snapshots,
death-watch, hourly analyzer). Final logs: scratchpad `t1_final_{host,client}.log`.

**Adjudication (final analyzer run, 19:03):**
- **live (client, coop=0, episode=0): deerSpawner, bp7Spawner, hexahiveSpawner** (hexahive
  converted armed→live at ~60 min — its 40–60 min cycle fired ON THE CLIENT, coop-unexplained)
  — **3 rows ≥ the pre-registered STRUCTURAL threshold.**
- **armed:** mannequinSpawner (1–3 h), eyers, roachSummoner (tickEnabled=1) — 3 rows.
- **DONE-suppressed:** yellowWispSpawner. **confirmed-starved:** none.
- **CONDITIONAL:** the same 9 (4 static event-triggered per gate 3; 5 with NO spawner instance in
  this world at all). QuitGame: zero records on either peer across the whole run.

**The call (recorded honestly):** fork inputs = 3 client-live un-suppressed shared-world classes
→ the pre-registered fork rule reads **STRUCTURAL**. The callable-check (≥11/16 non-CONDITIONAL)
stands at 7/16 — formally short. The USER terminated the run and called the design pass
(2026-07-10 evening, “Kill it” after the confirmation-not-discovery framing), so this is a
**user-called STRUCTURAL with the callable gate explicitly waived**, not a silent re-basing.
The waiver is sound because the 9 CONDITIONAL rows are structurally NON-adjudicable by idle
exposure (5 have no world instance to testify; 4 are event-triggered by construction) — and the
STRUCTURAL design covers them BY CONSTRUCTION (the client rolls NOTHING shared-world), so their
per-row measurement moves to POST-BUILD VERIFICATION, not the design gate. Every row idle
exposure COULD adjudicate (7/7) confirmed client-side spawn machinery: 3 live + 3 armed + 1
suppressed-by-us. Zero rows contradict STRUCTURAL.

**Consequences now armed:** the RULE-2 pre-commitment (ambient_spawner_suppress's per-class
cancels dissolve into the structural suppress set in the same change), the color-wisp
PRODUCT-EXEMPTION carry-through, placement #2 (ambient-spawn authority home), and the
mirror-step-3 generalization all go to the **T1 `/qf 15` DESIGN pass — the next work item.**

## T1 STRUCTURAL DESIGN — ratified (/qf DESIGN pass, 6 rounds, 2026-07-10 evening; NOT built)

**Status: Inc-1 AS-BUILT `e6c1371b` (2026-07-10 night); the rest DESIGN.** Transcript:
scratchpad qf_thread.md (session 86866d94). Critic hold at R6.
**Inc-1 as-built + VERIFIED-by-census (host-as-control, smoke x2, 0 errors):** module
`coop/world/spawn_authority.{h,cpp}` -- t1 parks ticker_insomniacSpawner_C +
ticker_fossilhoundSpawner_C (client tickEnabled=0, ZERO post-park driver re-arms; refined run
parked at +7s BEFORE the first re-arm; host control 26 insomniac re-arms in-window), t3 = the
four ambient cancels migrated verbatim (ambient_spawner_suppress DELETED, RULE-2), shipping
tripwire in npc_sync's client pass-through, analyzer suppress-join re-pointed. The measured
join-window fact: save-loaded spawner instances appear AFTER install -> the 1 Hz first-instance
hunt bounds the pre-park window to ~1 s (was: the design's 'park inside the join episode' --
as-built this is hunt-until-first-park, measurably equivalent).

**INVARIANT:** a connected CLIENT ticks NO shared-world spawner and rolls NO shared-world spawn
RNG; shared-world content arrives ONLY via the host wire; per-peer exemptions are an explicit
table row, never an omission.

**Mechanism — ONE cancel TABLE (data), THREE tiers (fixed seams):**
- **Table row:** spawner class + tier tag (t1/t2/t3/OWNER-EFFECT) + optional
  (class, latent-UUID) rows for mixed classes. `ticker_wispSpawner_C` = explicit OWNER-EFFECT
  row (user decision 2026-07-10: per-peer AUTHORITY + cross-peer mirror — see USER DECISIONS).
- **t1 tick-park:** engine's own `SetActorTickEnabled` wrapper (ue_wrap/engine.cpp:539 — the
  mirror brain-park path; no raw field writes). Park during the JOIN EPISODE (before client
  gameplay ticks → zero window); 1 Hz drift-only re-assert over a CACHED instance set (one
  class-filtered walk at install, cheap-class-check-before-NameOf; 60s reconcile belt; late
  instances also enter via the BeginDeferred interceptor); restore-on-disconnect. The
  suppression loan is repaid by construction: every session-end path tears down to the main
  menu (measured), so the next world load re-arms everything; the restore is belt for the
  teardown window.
- **t2 latent-starve:** ONE shared PRE Func-patch each on Delay / SetTimerDelegate /
  SetActorTickInterval (3-4 slots total): cancel the arm iff role==Client && running() &&
  caller UClass* ∈ table (flat pointer set resolved at install; allocation/NameOf/lock-free;
  parallel-anim-thread safe). Catches Delay-latent re-arm chains regardless of BP-internal
  dispatch (probe-v9-measured seam, caller attribution via FFrame::Object).
- **t3 entry-fn cancel:** the ~5 externally-triggered spawners (greenFire/furfurAltar/
  hillRoller/arirBuster/propSpawner_editor — NO drivers in own bytecode): targeted body cancel
  of the triggered entry fn (proven ambient shape); their post-trigger chains have
  caller==spawner so t2 catches those too.
- **Directors (mainGamemode/daynightCycle) are NOT parked** — their roll lanes stay T1-2
  per-lane work (QuitGame freeze = its own small patch).
- **The PRODUCT seam stays:** the BeginDeferred interceptor keeps wire-bypass + the allowlist
  keeps its MIRROR-set role (product-class-keyed regardless of caller) + a SHIPPING low-rate
  tripwire WARN when a table class spawns client-side without coop origin (regression alarm;
  silent against the measured 121-spawn census). New-class DISCOVERY stays with the ini-gated
  resident census (dev tooling) — stated, not silently claimed.

**Phasing (no content regression):**
- **Inc-1:** park ONLY spawners whose products are already mirrored (npc allowlist 15 +
  ambient 4). Per-class gates BEFORE parking: (i) bytecode-dump read (all 28 dumps exist) for
  non-spawn responsibilities (mixed class → (class,UUID) grain) AND the product-class literal
  (converts the name-inferred product→spawner mapping to VERIFIED; unmirrored product →
  self-moves to Inc-2); (ii) MEASURED free-hook-slot count before any t2/t3 wiring (estimate:
  ≤10 new vs 14 free of kMaxNativeHooks=40).
- **Inc-2+:** one product FAMILY per increment: extend the host EntitySpawn mirror to the
  product class FIRST, then park its spawner. Proposed order (measured-liveness default,
  user-adjustable): hexahive → bp7 → eyers → roach → mannequin. Slow classes are Inc-2 by
  construction (their gates need long windows; the vitals keepalive 0211b9c5 makes 12h+
  verification runs routine).
- **Per-class VERIFY gate (discriminating, host-as-control, same run + instrument):** client
  BeginPlay arm record present + host ≥2 driver re-arms in the window + client ZERO re-arms;
  window = 2x the class's max interval literal. Ambient-4 mechanism is grandfathered through
  the dissolve but re-gated after the move as relocation proof.

**RULE-2 / placement:** new module `coop/world/spawn_authority.{h,cpp}`;
ambient_spawner_suppress dissolves INTO it in the SAME commit; host_spawn_watcher mirror halves
stay. Residue census rows are classified to their OWN lanes, never stuffed into T1-1:
growingPlant → COOP_WORLD_PROP_DIVERGENCE, weatherFogController → weather_sync, grime_* →
grime_sync/T3, dirthole_item_C (~330/pass, first-sighted in the join window with flat per-pass
count) = census-ARTIFACT candidate (level-streaming re-instantiation) → targeted classify probe
before any lane assignment, NewBlueprint3/41 → RE.

**USER DECISIONS (answered 2026-07-10 night, verbatim rule recorded):**
(1) **OWNER-EFFECT RULE (user, general):** "If some effect spawns only around the local player
and suppressing that on clients would degrade native experience — then yes, client rolls their
own effect (like fireflies), but those effects shouldn't be local-only per peer: let all peers
see each other's effects. This rule applies to all effects of this kind. Own the auth and sync
layers on such effects." → the table tier EXEMPT-PER-PEER is RENAMED **OWNER-EFFECT**: the
rolling peer keeps AUTHORITY (never parked, never host-rolled) and gains a cross-peer MIRROR
lane (owner broadcasts its effect spawns; peers render them) — the pose-stream shape applied to
ambient effects. An RE/classify step in the Inc plan decides each ambient spawner's class
(world-anchored → cancel table; player-proximity effect → OWNER-EFFECT + mirror lane). The
owner-effect mirror is its own increment family (does not block Inc-1). **Shipped precedent:
`coop/world/firefly_sync` (v51, 2026-06-09, [V]) already IS this rule end-to-end** — each peer's
ticker_fireflySpawner keeps rolling natively (camera-relative), PRE+POST-diffs its OWN spawn,
broadcasts FireflySpawn (peer-symmetric, host-relayed), peers render the union.
**MEMBERSHIP (2026-07-10 eve anchor audit — measured, superseding the assumed list):** fireflies
[V-dump] + pinecone forest drops [V-dump, built peer-symmetric same day]. Color/sky wisps were
REVERSED OUT (dump: absolute map coords → world-anchored cancel+mirror). Autumn leaves
UNVERIFIED (autumnLeafSpawner not yet dumped — anchor read before classifying). Roaches assessed
NOT a member (world-anchored infestation on one master; own host-auth lane v108).
(2) **dreams/wakeup (user, 2026-07-10 night): "The dreams need to be a shared experience,
not per peer. Host owns the dreams."** → dream/wakeup rolls are HOST-AUTHORITATIVE shared
state: the host rolls dream selection, clients mirror the outcome (a dream-sync lane; its RE —
wakeup/createDream trampoline segments, gate-4 notes — feeds a dedicated increment). NOT
player-local; the earlier default is superseded.
(3) Inc-2 family order: measured-liveness default (hexahive → bp7 → eyers → roach → mannequin)
stands unless reordered.
(4) **OWNER-ENTITY tier (user, 2026-07-10 eve): "Each peer must have it's own but visible for other
peers. Host auth and sync layer for this is needed, and a test in f1 dev to spawn it."** -> the
owner-effect rule extended to full CREATURES whose behavior loop reads the LOCAL player (stalkers).
Each peer keeps its native roll + owns its entity; peers render brain-parked display mirrors via
`coop/creatures/owner_entity_sync` ((slot,seq) identity -- deliberately NOT the host-eid element
registry). First member: eyer_C. eyers therefore LEAVES the Inc-2 mirror-then-park order (it is
never parked). F1 reorg shipped with it: Content section (Entities + Events), Game dissolved.

## CHANGELOG
- **2026-07-10 (eve, later)** — **OWNER-ENTITY lane BUILT (eyer)** per the user rule (decision (4)):
  new `coop/creatures/owner_entity_sync` (kinds 94-96, relayed; owner BeginDeferred POST detect +
  4 Hz pose/keepalive/death-watch; receivers park ticks + disable collision so the mirror's
  killsphere can never hurt a viewer). eyers row -> DONE-OWNER-ENTITY. F1 menu reorganized by game
  domains: Content (Entities incl. "Spawn Eyer" test + Events), Game dissolved into World/Content.
- **2026-07-10 (eve, post-documentize)** — **ANCHOR AUDIT + three lanes BUILT (v108)** on the
  user's "Lets go, also fix roaches": per-class bytecode ANCHOR reads reclassified the ambient
  set both ways — (a) pineconeSpawner = PLAYER-CAMERA-anchored → OWNER-EFFECT: t3 cancel row
  REMOVED, host_spawn_watcher ambient broadcaster went PEER-SYMMETRIC (ScopedMirrorSpawn echo
  guard + mirror SetLifeSpan(900) orphan backstop); (b) ticker_wispSpawner = ABSOLUTE-map-coords
  (±60-70k, Z=80k) → the OWNER-EFFECT call REVERSED: t3 cancel row + wisp_C + 8 color variants
  allowlisted (15→23) + EX-catch source row; (c) ROACHES = components on the ONE world-anchored
  cockroachMaster (food-seeking infestation, shared-prop mutation in calc()) → NOT owner-effect:
  new `coop/creatures/roach_sync` (RoachState paged snapshot 92 + RoachConsumed intent 93,
  proto v108) + t1 park (master+ticker) + 4 t3 timer-entry cancels. yellowWisp anchor measured
  = navmesh random-walk (suppression stays correct). Fireflies re-confirmed player-anchored
  (the only measured owner-effect members: fireflies + pinecone drops; leaves RE-pending).
- **2026-07-10 (night, later)** — **Inc-1 BUILT + VERIFIED-by-census `e6c1371b`** (see the
  as-built note in the design section). DLL `92C7FC96` deployed all 4; smoke x2 PASS. Audit
  0 findings (npc_sync 978-LOC pre-existing cap flag carried -> extraction on next touch).
- **2026-07-10 (night)** — USER DECISIONS answered: OWNER-EFFECT rule (per-peer authority +
  cross-peer mirror for all player-proximity ambient effects — wisps/fireflies/leaves; own
  both layers), dreams default PLAYER-LOCAL, Inc-2 order default accepted. EXEMPT-PER-PEER
  tier renamed OWNER-EFFECT throughout the design.
- **2026-07-10 (late evening)** — T1 `/qf` DESIGN pass CONVERGED (6 rounds, critic hold at R6;
  transcript scratchpad qf_thread.md): ratified STRUCTURAL DESIGN section added (cancel table +
  3 tiers + product seam retained + Inc phasing + verify gates + RULE-2 dissolve). NOT built.
  NEXT: Inc-1 implementation plan; 3 non-blocking user decisions surfaced.
- **2026-07-10 (evening)** — THIRD (keepalive) census: idle-death root-fixed (starvation;
  vitals_keepalive `0211b9c5`), 65-min continuous 2-peer run, hexahive converted → 3 client-live
  rows → **fork = STRUCTURAL (user-called; 7/16 callable gate waived — rationale in section)**.
  DESIGN pass UNGATED; run `/qf 15` on the structural fix next.
- **2026-07-10 (afternoon)** — SECOND (exposure) census run: host full day, client 18-min slice
  (idle death) + an aborted rejoin. **Formal fork call NOT CALLABLE (7/16 < 11/16) → DEFER**,
  per the pre-registered rule (section above). Analyzer tool `tools/rng_census_analyze.py`
  committed (`a287b3f1`). DESIGN pass stays gated. Also: the audit's ambient-module fact-base
  rows unchanged; placement #2 (ambient-spawn authority merge) still pre-registered to the
  T1 design (the rest of the placement series EXECUTED this day — see the placement finding).
- **2026-07-10 (later)** — probe v9 BUILT + smoked twice (seam correction in between) + first
  partial census recorded (section above). Pre-work gates 1-5 closed. NEXT: full 1x day-night
  exposure run → formal fork call → `/qf 15` DESIGN pass on the structural fix.
- **2026-07-10 (late)** — T1 `/qf 15` QUESTION pass CONVERGED (11 rounds — critic "that holds" at
  R11; 3 self-refutations by measurement; R10-R11 pinned the callable-check arithmetic, the
  instrument scope (T1-3 static/INTENT, T1-4 gate-5 RE), and the color-wisp PRODUCT-EXEMPTION).
  PRE-REGISTRATION section added (probe v9 + fork rule + blind spot + RULE-2 fate).
  Fact-base corrections: ambient_spawner_suppress discovered (audit missed it) → T1-1 yellowWisp
  row flipped DONE-MIRROR, T2-6 split (mushroom/pinecone DONE-MIRROR, color wisps product-per-peer);
  mainGamemode ubergraph native census measured (66 spawn pairs, 62 delays, 20 timer arms,
  7 QuitGame, 24 weighted rolls); ticker re-arm heterogeneity measured; 15/17 spawners
  BeginDeferred-based (2 undumped chains gated as pre-work).
- **2026-07-10** — doc created. 3-agent audit → tiers seeded. All T1/T2 rows NEEDS-PROBE/OPEN.
  Next session: live client-roll probe → then `/qf 15` per tier (start T1 structural-suppression).

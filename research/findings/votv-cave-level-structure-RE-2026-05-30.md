# VOTV "other place" structure RE — one UWorld; entry mechanisms unknown (2026-05-30)

## Question

User (2026-05-30): VOTV has the cave/mine, "dimensions" (a backrooms; a cave-like
dimension of stacked rocks), **sleep dreams** (sleep -> minigame -> succeed=sleep /
fail=wake ragdolled out of bed), and **hidden locations physically under the map**.
The backrooms and dream maps are NOT under the map; the under-map hidden locations
are separate. Make-or-break coop question: **separate `UWorld` (OpenLevel travel =
the hard nightmare) or same persistent world (in-world teleport / streaming
sublevel = no travel machinery)?**

## Method + correction trail (read this before trusting any mechanism claim)

Static RE off the two UE4SS object dumps (`UE4SS_ObjectDump_GAMEPLAY_SAVE.txt`
~83 MB, player on surface; `..._MAIN_MENU.txt` ~68 MB) + string-grep of the cooked
`VotV-WindowsNoEditor.pak` (8.2 GB, asset-path strings only — NO readable BP
bytecode). Final pass was a 7-agent evidence-locked workflow (5 parallel finders ->
adversarial verifier -> synthesis); this doc is its corrected output.

**Hard epistemic limits (obeyed):** an object dump proves only what is LOADED — it
cannot disprove on-demand content (absence = "not loaded in snapshot", never "does
not exist"). The pak has assets but no BP logic — so HOW a place is entered
(teleport vs `LoadStreamLevel` vs trigger) and any generation/seed logic are NOT
knowable statically and are marked UNKNOWN. This finding was revised across the
session as the user corrected earlier over-claims (a blanket "zero work"; conflating
dreams with the backrooms; "below-map -> backrooms"; asserting `LoadStreamLevel` as
fact). Mechanism claims are now status-marked confirmed / inferred / unknown.

---

## 1. Bedrock (CONFIRMED)

**VOTV gameplay runs in exactly ONE persistent UWorld** — the load-bearing fact.
- One gameplay UWorld: `World /Game/maps/untitled_1.Untitled_1`.
- One gameplay ULevel: `untitled_1.Untitled_1:PersistentLevel`, with **50,637
  placed objects** under it (matches the user's number exactly).
- Menu is a separate UWorld (`/Game/menu.menu`) in its own dump — not a second
  *gameplay* world.
- **No non-CDO `LevelStreaming` instance in either snapshot** (only the
  `Default__LevelStreaming` CDO). `OpenLevel`/`LoadStreamLevel`/`ServerTravel`
  appear only as `/Script/Engine` + `VictoryBPLibrary` Function *definitions*
  (present in every UE build) — not evidence of use.
- The only world swap is boot/save-load (`open untitled_1`), already handled by the
  reaper + world-change re-seed (HEAD 63f238d). No second gameplay world anywhere.

(Correction banked: an earlier finder's "~49 placed actors" was a grep artifact —
it matched only the bare class string `Actor`, missing `StaticMeshActor`/`*_C`/
components. Real count 50,637.)

## 2. The systems

### (a) Modeled mine + hidden under-map locations
The under-map area is RICHER than just the mine. CONFIRMED present in
`untitled_1:PersistentLevel` (surface snapshot):
- **Cave**: `caveSegment_straight_*` / `caveSegment_deadend*` (~50 hits, baked);
  `rocks_rock_L27_abcave*`; `caveEntryCheck` is a real `SphereComponent` on
  `antibreatherSPawner_2` (EXISTENCE only — not a proven entry mechanism).
- **Elevator-of-Death** cluster (~56 hits): `elevatorOfDeath_elevatorShaft3`,
  `_elevatorWalls/Doors/Grate/CeilingFloor/Details`.
- **Basement / sub-basement** under `Basev2Final_2`: `base3segment_basement`,
  `_bottomBasement`, `_subBasement`; `misc_box_basement1/2`; `ambLigh_basement`;
  `waterVolume_basementFlooder_5`; `trigger_lightRoot_basement`.
- **Alpha Bunker**: `misc_box_alphaBunker`, `ambientLightCurve_alphaBunker`,
  `bunkerHatch_2`, `doorbunkersw_prot_2`.
- **Secret wall**: `prop_basementSecretWall_2` (PhysicsConstraint1-4 + break audio
  + `eff_break` — a breakable barrier, inferred medium).
- **Tunnels**: `prop_tunnelRock2..6`, `misc_box_tunnel`, `prop_minelight*`.

INFERRED: these are distinct discrete locations (cave/elevator/basement/bunker/
tunnel), not one continuous space (medium-high; naming cohesion, no transforms).
UNKNOWN: **Z-coordinates** (dumps carry no reliable transforms; "under the map"
rests on user ground truth + naming, not the dump); how each is entered; whether
"the cave" a player visits is these baked `caveSegment_*` actors or the `sl_caves`
sublevel (see 2b) — the two are in tension and the snapshot only proves the baked
segments exist.

### (b) Dimension sublevels
CONFIRMED: **nine `sl_*` packages** under pak `/Game/maps/sublevels/`: `sl_caves`,
`sl_goop`, `sl_woid`, `sl_poopDim` (+`sl_poopDim_BuiltData` = lighting-bake data,
NOT a dimension), `sl_alexpdim`, `sl_place`, `sl_SaveTest`, `sl_backroomsObsolete`;
plus `/Game/maps/sl_cavesObsolete` outside `/sublevels/`. **None loaded in either
snapshot.** Theme assets exist (caveores/cave ambience; `mat_goopscape`/goop
footsteps; `mat_void*`/`inst_void*`; `poop_*`).
INFERRED (theming, asset-level): `sl_caves`=rocky cave, `sl_goop`=slime, `sl_woid`
≈void, `sl_poopDim`=poop (all high); `sl_SaveTest`=dev/test artifact; `*Obsolete`
=deprecated earlier impls (high). `sl_alexpdim`/`sl_place` purpose = low/UNKNOWN
("alexp" reads as a dev name, not "alien").
UNKNOWN: **the load mechanism** — "streams on demand" is NOT proven (zero
`LevelStreaming` instances). Confirmed only: "present in pak, not loaded in either
snapshot." Whether they load via `LoadStreamLevel` (-> same UWorld) vs `OpenLevel`
(-> world swap) vs never (dev-only) is bytecode-UNKNOWN. (Per-sublevel "12/13
instances" numbers from an earlier finder were mashed-string artifacts — discarded;
~3 distinct base refs each.)

### (c) Sleep dreams (matches user ground truth)
CONFIRMED classes/members: `mainGamemode_C` `isSleep/dreaming/dreamProbability/
sleepTime/sleepCam/bed/sleepingPawn/playerPreDream` + fns `sleep(bed,dropItem,
ignoreRagdoll)/wakeup/bedSleepProb/createDream/processDream/leaveDream` + delegates
`fellAsleep/sleepTriggered`. `AdreamBase_C : AActor` is **self-contained**
(`PostProcess`, `SkyLight`, `dream` audio, `sky` mesh, `playerSpawn` billboard,
`Duration`, `TArray<Fstruct_save> inventory`; `naturalWakeup()/awoken()`); dream
actors `/Game/objects/dreams/dream_{boulders,burger,climb,dreambase,fill,jump,mann,
room,run,ufo,wend}`. `mainPlayer_C` `isRagdoll/ragdollMode/ragdollActor/
AutoRagdollGetup/wakeup/forceWakeup/sleepComfort/sleepDraining`.
INFERRED (high): dreams run as **contained actors inside the one persistent world**
(per-dream PostProcess+SkyLight+playerSpawn), NOT as loaded map levels — consistent
with ground truth; `processDream` does weighted-random selection (`dreamers_C`
class->float map) + `BeginDeferredActorSpawnFromClass`; inventory carried across via
`dreamBase_C.inventory`. UNKNOWN: spawn coords/placement; sleep trigger; per-minigame
success/fail/timeout; whether `sl_alexpdim`/`sl_place` are ever used by dreams (a
categorical "dreams never use a sublevel" is NOT provable — only "no dream-named map
found by pattern, none loaded in snapshot").

### (d) Backrooms (separate system; NOT under-map per ground truth)
CONFIRMED: `mainGamemode_C` `backroomsEnabled [o:8D9]`, `backroomsPass [o:C48]`,
`cheatEnableBackrooms [o:1240]`; `canBackrooms` fn (structure includes
`GetCurrentLevelName` + `GetActorUpVector` + `Dot` + `EqualEqual_StrStr` nodes — a
level-name+orientation check STRUCTURE, not proven semantics), referenced from the
mainPlayer ubergraph. `mainPlayer_C::teleportWObackrooms` fn uses explicit
`NewTransform/useRotation/trueRotation` + velocity + grabbed-actor params (explicit
transforms, not runtime generation). Pak: only `sl_backroomsObsolete` matches;
textures `backroomsCeiling/_emiss/Floor/Wall` + materials `inst_broom_*`; **no
backrooms mesh and no generator found by name-pattern grep.**
INFERRED (medium): entry is a **teleport within the persistent world**
(`teleportWObackrooms`'s explicit transforms) rather than a procedural generator —
but the call-site is bytecode. The OLD impl was sublevel-based (`*Obsolete` says it
was abandoned); the CURRENT mechanism is UNKNOWN (do NOT assert "procedural" — that's
an absence argument). UNKNOWN: what calls `teleportWObackrooms` + the destination;
the boolean runtime values; whether current backrooms reuse the obsolete sublevel,
teleport-to-a-baked-region, or something else.

## 3. Coop implications

**It is all ONE UWorld** — removes the hardest MP problems: no `OpenLevel` swap, no
`ServerTravel`, no second gameplay world, and no mass-purge level transition for the
in-world "other places" (the under-map locations are always-resident baked
PersistentLevel actors). No forced co-location concept needed for them.

Bounded real work, split by **is the destination baked-in-world or a separate
sublevel?**
1. **Baked under-map locations** (cave/elevator/basement/bunker/tunnels): already
   inside the synced world. Cost = normal pose/prop sync + per-site SP-assumption
   fixes (principle 4) for interactables there (`prop_basementSecretWall_2`, the
   elevator, the bunker hatch). No new place-membership machinery.
2. **Dreams**: actor-based, deferred-spawned in the shared world, inherently a
   single-player minigame (one sleeper, private). Recommended: **per-player /
   host-authoritative dream instancing** — do NOT replicate the dream actor as a
   shared space; replicate only the sleeper's sleep/ragdoll STATE so others see them
   lying in bed / ragdolled. Treating a dream as shared geometry would force
   co-presence into a private minigame (out of scope, against ground truth).
3. **Dimension sublevels + backrooms**: hinges on the entry mechanism. IF a separate
   **sublevel load** -> need host-authoritative **place-membership** (which peers are
   "in" the place) + each client loads the SAME baked sublevel client-side (cheap,
   deterministic geometry — NO procedural replication). IF a **teleport within the
   persistent world** (what `teleportWObackrooms`'s explicit transforms hint at,
   unproven) -> collapses into case 1: just movement in the shared world, no new
   machinery. **This teleport-in-world vs sublevel-load question is the single
   highest-value unknown** — it decides whether ANY place-membership system is
   needed.

Design call to make explicit when picked up: **shared vs per-player** — baked
under-map = shared by default; dreams = per-player; dimensions/backrooms = undecided
pending the entry-mechanism RE. No MTA Dimensions-style instancing (RULE 3 scope) is
needed for the baked locations regardless.

## 4. Residual RE (runtime / bytecode-only — reflection has hit its limit)

Confirm via a C++ dev probe (preferred over UE4SS Lua) / hands-on / IDA on the
specific native call site:
1. **Backrooms entry call-site (highest value):** what calls `teleportWObackrooms`,
   with what destination, and is it teleport-within-untitled_1 or a sublevel load?
   Probe: trace `teleportWObackrooms` + `canBackrooms`; snapshot world+pos
   before/after.
2. **Dimension-sublevel load mechanism:** are `sl_caves/goop/woid/poopDim/alexpdim/
   place` loaded via `LoadStreamLevel`, `OpenLevel`, or never? Trap those calls;
   watch for any `LevelStreaming` instance appearing.
3. **Cave: baked vs streamed** — does entering the cave use the baked `caveSegment_*`
   actors, trigger `sl_caves`, or both? Watch `caveEntryCheck` overlaps + any
   sublevel load.
4. **Dream spawn flow:** confirm `processDream` weighted-random pick from
   `dreamers_C` + deferred-spawn placement relative to the player.
5. **Dream isolation:** confirm a dream is a self-contained actor in untitled_1
   (PostProcess/SkyLight swap), NO sublevel load, NO world change -> per-player
   instancing is safe.
6. **Backrooms booleans:** runtime values + which gates entry.
7. **Z-coords of under-map locations:** read live transforms of `caveSegment_*`,
   `elevatorOfDeath_*`, `base3segment_*`, `alphaBunker` to confirm placement + map
   their layout.
8. **`prop_basementSecretWall_2` / elevator interactables:** semantics (break vs
   open) for per-site coop replication.

## Bottom line

One persistent UWorld with rich baked under-map locations (caves/basement/bunker/
elevator — confirmed present, Z unknown), an actor-based per-player dream system
(confirmed surface), a teleport-or-sublevel backrooms (entry UNKNOWN), and nine
`sl_*` dimension sublevels in the pak (none loaded). The only coop machinery that
*might* be needed is host-authoritative place-membership for the streamed sublevels/
backrooms — and whether even that is required hinges on the one unresolved question:
**teleport-in-world vs sublevel-load.** NOT a separate-world rewrite.

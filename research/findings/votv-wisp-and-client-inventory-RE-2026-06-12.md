# VOTV killer-wisp + per-player client inventory — RE + coop sync design

Date: 2026-06-12. READ-ONLY RE pass (no `src/` edits). Two independent topics:
(1) the late-game **killer wisp** hostile (mirror it host-authoritatively; target all
peers equally; one victim at a time), and (2) **per-player client inventory** (replace
the v56 whole-host-save inheritance with a Minecraft-style per-player inventory persisted
on the host).

Ground rules followed: standalone reflection + reflected UFunction calls over custom UDP,
host-authoritative, no game-file edits (CLAUDE.md). MTA precedents cited from
`reference/mtasa-blue/`. SDK = `Game_0.9.0n/.../CXXHeaderDump/*.hpp` (authoritative
offsets); BP bytecode = `research/bp_reflection/*.json` via `_fn.py`/`_scan.py`/`_cfg.py`.

New dumps produced this pass (kismet-analyzer e8982e9, UE4.27 — assets were extracted but
not previously dumped):

```
KA=research/pak_re/tools/ka/kismet-analyzer-e8982e9-win-x64/kismet-analyzer.exe
$KA to-json research/pak_re/extracted/VotV/Content/objects/killerwisp.uasset            > research/bp_reflection/killerwisp.json
$KA to-json research/pak_re/extracted/VotV/Content/objects/killerWispSpawner.uasset      > research/bp_reflection/killerWispSpawner.json
$KA to-json research/pak_re/extracted/VotV/Content/objects/triggers/trigger_wispSwarm.uasset > research/bp_reflection/trigger_wispSwarm.json
# disasm: python research/bp_reflection/_fn.py killerwisp scanForActors|canReach|gather|capture ...
# uber:   python research/bp_reflection/_cfg.py killerwisp ExecuteUbergraph_killerwisp [entry]
```

`ticker_wispSpawner` + `ticker_yellowWispSpawner` were already dumped; the SDK header
`CXXHeaderDump/killerwisp.hpp` already existed (my initial `**/[Ww]isp*.hpp` glob missed it
because the file is `killerwisp.hpp`, all-lowercase).

---

# TOPIC 1 — THE KILLER WISP

## 1.1 Asset census (pak `objects/` + `meshes/killerwisp/`)

| Asset | Path | Role | Dumped? |
|---|---|---|---|
| `killerwisp_C` | `VotV/Content/objects/killerwisp.uasset` | **the hostile** (ACharacter) | YES (this pass) |
| `killerWispSpawner_C` | `VotV/Content/objects/killerWispSpawner.uasset` | **the spawner** (map-placed) | YES (this pass) |
| `trigger_wispSwarm_C` | `objects/triggers/trigger_wispSwarm.uasset` | scripted swarm trigger | YES (this pass) |
| `ticker_wispSpawner_C` | `objects/tickers/ticker_wispSpawner.uasset` | **ambient** (harmless) wisp spawner | prior |
| `ticker_yellowWispSpawner_C` | `objects/tickers/ticker_yellowWispSpawner.uasset` | ambient yellow wisp spawner | prior |
| `wisp_C`, `wisp_b/_bl/_blu/_g/_o/_p/_red/_w` | `objects/wisp*.uasset` | harmless ambient wisps (decor) | no (not needed) |
| `prop_bloodGib_arm_C`, `prop_bloodGib_leg_C` | (imports) | the torn-off limb gibs | n/a (referenced) |

The dangerous entity is **`killerwisp_C`**, spawned by **`killerWispSpawner_C`**. The
plain `wisp_*` family is harmless ambient decor (spawned by `ticker_wispSpawner`). Do not
conflate them.

## 1.2 `killerwisp_C` class anatomy (SDK `killerwisp.hpp`, size `0x65A`)

It is an `ACharacter` (so it already has `Mesh` / `CharacterMovement` / `CapsuleComponent`
— it fits the existing **NPC pose-mirror pipeline** which targets `ACharacter`s). Relevant
state fields (the ones a mirror / a routing fix must reason about):

| Field | Offset | Type | Meaning |
|---|---|---|---|
| `Target` | `0x0610` | `APawn*` | **current victim** (set by `scanForActors`) |
| `tryGrab` | `0x05D8` | bool | reached the victim, grab pending |
| `grab` | `0x0600` | bool | grab/fatality montage in progress |
| `killed` | `0x0618` | bool | victim killed (latched after `drop`) |
| `gathered` | `0x0619` | bool | consumed by a piramid event (alt end) |
| `rendered` | `0x0634` | bool | "encounter is active near the player" (music/event) |
| `playerDamaged` | `0x0635` | bool | first limb removed (achievement gate) |
| `harmless` | `0x0658` | bool | non-lethal variant flag |
| `avoidPlayer` | `0x0659` | bool | flee-from-player mode (camera-relative) |
| `g1..g4` | `0x05E0..0x05F8` | `Aprop_bloodGib_C*` | the 4 spawned limb gibs |
| `obj` | `0x0638` | `TArray<EObjectTypeQuery>` | overlap object-type filter (pawns) |
| `GameMode` | `0x0650` | `AmainGamemode_C*` | cached gamemode |
| `1323123122131321322` | `0x0628` | `FVector` | gather/consume anchor location |
| `leg_L/arm_L/LEG_R/arm_R` | `0x0550..0x0568` | `UStaticMeshComponent*` | limb meshes (gib attach points) |
| `Sphere` | `0x0500` | `USphereComponent*` | the 5000-u sense origin |
| `AIPerception` | `0x0548` | `UAIPerceptionComponent*` | sight sense (kick-starts scans) |

Key UFunctions: `scanForActors` (targeting), `canReach` (LOS/reachability), `Capture`
(the fatality), `gather`, `releasePlayer`, `Stretch`/`Walk`/`lookAt`/`moveToTarg`/
`updateSense`. The real bodies of `Capture`/`releasePlayer`/`moveToTarg`/`updateSense`/
`Walk`/`Stretch` are **all `ExecuteUbergraph_killerwisp(<entry>)` trampolines** — the logic
lives in the 541-statement ubergraph, dispatched by anim-montage notifies.

## 1.3 Targeting — `scanForActors` is a PROXIMITY search, NOT a player-0 hardcode

`scanForActors` (80 stmts) is the acquisition. It does **not** read `gamemode.player` or
`GetPlayerPawn(0)`. It does a sphere overlap and picks the nearest LOS-valid pawn:

```
@221  SphereOverlapActors(self, Sphere.location, 5000.0, obj_pawn, <Pawn>, out)   # 5000-u radius
      # per overlapped actor:
@626  GetClassHierarchy(actor.class) ; Array_Contains(filter, class)               # class gate
@1350 LineTraceSingleForObjects(Sphere.loc -> RandomPointInBoundingBox(actor))     # LOS check
@1825 target.GetDistanceTo(self) vs actor.GetDistanceTo(self) ; keep the NEARER     # nearest wins
@2227 target := cast<Pawn>(senseObj) ; moveToTarg()                                 # commit + chase
```

The `filter` class allow-set (extracted from the `EX_SetArray` literal) is:

```
filter = [ mainPlayer_C , kerfurOmega_C , fossilhound_C ]
```

**This is the single most important coop fact for Topic 1.** Our remote-player puppets ARE
unpossessed `mainPlayer_C` orphans (`remote_player.cpp:53` "puppets are also mainPlayer_C
instances"; CLAUDE.md puppet design). Therefore `scanForActors` **already enumerates every
peer's puppet as an eligible target and picks the nearest by `GetDistanceTo`** — the
acquisition treats all peers equally for free, exactly MTA's `CPedSync::FindSyncer ->
FindPlayerCloseToPed` nearest-player model (`reference/mtasa-blue/Server/.../CPedSync.cpp:141`).
No change is needed to make the wisp *acquire* clients. (Caveat: the puppet must be present
as a live pawn within 5000 u on the host — it is; the host runs every puppet.)

## 1.4 The grab → lift → dismember → kill chain (montage-notify FSM)

`Capture()` jumps to ubergraph `@13229 -> @12184` (guard `tryGrab|grab|killed|gathered`)
`-> @12313 cast<mainPlayer_C>(target) -> @12392` plays the **`fatality`** montage on `Mesh`.
The montage's **anim notifies** then drive a `SwitchName(Temp_name_Variable_1)` dispatcher.
Each notify is one phase:

| Notify | Uber block | Action |
|---|---|---|
| `grab` | `@5532` | gate: `SphereOverlapActors(550u)` must contain **`GetPlayerPawn(0)`** `@5787` AND `canReach()` → `grab:=true`, `stretch(true)`, `lookAt()`, dismount the player's ATV |
| (lift) | `@2538` | `GetPlayerCharacter(0).K2_AttachToComponent(Mesh,'playerGrab')` `@2941`; `getMainPlayer().held:=true` `@3150`, `.sittingOn:=self` `@3409`, `.Mesh.SetHiddenInGame(true)`, `.bUseControllerRotationYaw:=false` |
| `d1` | `@5188` | spawn `prop_bloodGib_arm_C` → `g1`, attach `arm_L`; `playerDamaged:=true`; → `@4870` |
| `d2` | `@4537` | spawn `prop_bloodGib_leg_C` → `g2`, attach `LEG_R`; → `@4870` |
| `d3` | `@6749` | spawn `prop_bloodGib_leg_C` → `g3`, attach `leg_L`; → `@4870` |
| `d4` | `@7082` | spawn `prop_bloodGib_arm_C` → `g4`, attach `arm_R`; → `@4870` |
| (dmg) | `@4870` | `getMainPlayer().Add Player Damage(24.0,…)` `@4916`; flesh/break sounds; `getMainPlayer().forceDrop()` `@5151` — runs after EACH of d1..d4 (≈4×24 dmg) |
| `drop` | `@7415` | `getMainPlayer().fallVeloc:=…` `@7461`; **`getMainPlayer().ragdollMode(true,false,true)`** `@7560` → DEATH; `killed:=true`; `walk()` |
| `dr1..dr4` | `@7690..@8085` | detach `g1..g4`, `static:=false`, `init()` → drop the limb gibs as free physics props |
| (hold) | tick `@8480` | while `tryGrab&&grab`: `GetPlayerCharacter(0)` is `VInterpTo`'d to `beamend_3_D` (the victim is lerped up to the wisp) |
| (release) | `@13289`→`@13787` | `getMainPlayer().held:=false` |

### 1.4.1 THE SP-HARDCODE: the kill chain operates on the LOCAL player, not `target`

Acquisition is target-based (1.3), but **every grab/lift/damage/kill step operates on
`getMainPlayer()` / `GetPlayerPawn(0)` / `GetPlayerCharacter(0)` — the LOCAL player
singleton — NOT on `Target`.** In SP that is invisible (target == the only player == player
0). On the host in coop, `GetPlayerPawn(0)` / `getMainPlayer()` is ALWAYS the host player,
so a wisp that *acquires* a client puppet would still *grab and kill the host player*. These
are the exact per-site routing seams (CLAUDE.md principle 6 — keep the P1 path, add the P2
path; here: replace "local player" with "the wisp's own `Target`"):

| Uber `@` | Current call | Route to |
|---|---|---|
| `@2566/2592/2853/2941` | `GetPlayerCharacter(0)` (lift attach + movement-mode) | `Target` |
| `@3002–3409` | `getMainPlayer().held / .sittingOn / .bUseControllerRotationYaw / .Mesh.Hidden` | `Target` |
| `@4916` | `getMainPlayer().Add Player Damage(24)` | `Target` (→ route to victim's slot, §1.6) |
| `@5151` | `getMainPlayer().forceDrop()` | `Target` |
| `@5787` | grab-gate overlap contains `GetPlayerPawn(0)` | contains `Target` |
| `@7461/7560` | `getMainPlayer().fallVeloc / .ragdollMode(true,false,true)` | `Target` (→ victim death, §1.6) |
| `@8480–8787` | `GetPlayerCharacter(0)` lerp-to-wisp hold | `Target` |
| `@13787/13833` | `getMainPlayer().held:=false` | `Target` |
| `canReach @79/163/772/798` | `getMainPlayer().isRagdoll` + `GetPlayerPawn(0).loc` | `Target.isRagdoll` + `Target.loc` |
| `@2148/2174` | `GetPlayerCharacter(0).GetDistanceTo` → `rendered`/`setEvent` (music) | per-victim / any-peer |
| `@11309/11540` | `GetPlayerCameraManager(0)` (avoidPlayer flee) | nearest peer |

We **cannot** edit the BP (anti-pattern A6). The routing happens our way: the host drives
the wisp's victim interaction through **our own reflected UFunction calls against `Target`**
rather than letting the BP's `getMainPlayer()` calls land on the host player — i.e. we
intercept/neutralize the BP's local-player effect and re-emit it onto `Target`. See §1.6.

## 1.5 Spawn — `killerWispSpawner_C` (map-placed, night-gated, capped at 4)

`ReceiveBeginPlay -> ExecuteUbergraph(2996)` starts a self-retriggering loop:

```
@15   IF Array_Length(gamemode.killerWisps) >= 4 : abort           # HARD CAP = 4 concurrent
@145  night := timeZ.X >= 18 OR timeZ.X <= 6   (OR gamemode == 7)  # 18:00–06:00 window
@686  p := K2_ProjectPointToNavigation(random ring 15000..50000u, z 28000)
@1243 LineTraceSingleForObjects(...) ; require PhysMat in `mats`   # valid ground
@1729 BeginDeferredActorSpawnFromClass(self, killerwisp_C, MakeTransform(p)) ; FinishSpawningActor
@1918 spawn `deer` decoys near p (SetLifeSpan 3600)                # ambience
@2746 Delay(180s) -> loop                                          # retry every 3 min
```

The wisp **self-registers** in a gamemode list at its own BeginPlay and deregisters on
destroy — a ready-made enumeration handle:

```
killerwisp BeginPlay  @9820 : Array_Add(gamemode.killerWisps, self)
killerwisp OnDestroy   @11122: Array_RemoveItem(gamemode.killerWisps, self) ; releasePlayer()
```

Persistence: the wisp is a **transient spawned actor** (no SetLifeSpan on the wisp itself;
it ends via `killed`/`gather`/destroy). The spawner is the **persistent map actor**.
Day-gating ("day 30–40"): the gate I can prove from bytecode is the **night window + the
`gamemode==7` check + the 4-cap**; whether the spawner is additionally only *present/enabled*
in late game (a day-gated ticker enabling it, or a story event) is **not resolved from the
spawner BP alone** (UNKNOWN §1.8).

## 1.6 Coop design recommendation — Topic 1

**Architecture: host runs the ONE authoritative wisp; clients get a parked pose-mirror; the
victim interaction is routed to `Target` and replicated to the victim's machine.**

1. **Suppress the client-side spawner (add to `ambient_spawner_suppress` scope).** Today
   that module covers only `mushroomMaster_C.Spawn` / `mushroomSpawner_C.Spawn` /
   `pineconeSpawner_C.ReceiveTick`; `killerWispSpawner_C` is NOT in scope. Add a suppressor so
   only the host's `killerWispSpawner_C` actually produces wisps. The spawner's spawn happens
   via `BeginDeferredActorSpawnFromClass` **inside** `ExecuteUbergraph` (BP-internal — per the
   session-13 "DISPATCH GROUND TRUTH" these never hit our PE detour), so the intercept seam is
   the **latent `Delay(180s)` resume** (PE-visible) or the same `BeginDeferredActorSpawnFromClass`
   class-gate the NPC suppressor already uses (`npc_sync.cpp` intercepts BeginDeferred and
   gates by class — reuse that path, adding `killerwisp_C` to a suppress-on-client set). The
   ambient `ticker_wispSpawner`/`ticker_yellowWispSpawner` may optionally be suppressed too for
   visual parity, but they are harmless and lower priority.

2. **Mirror the wisp to clients through the existing NPC pose pipeline.** `killerwisp_C` is an
   `ACharacter`; add it to the `npc_sync` allowlist (the 12-base subclass-aware allowlist in
   `npc_sync.cpp`). The host broadcasts `EntitySpawn`(`killerwisp_C`) / `EntityDestroy` and
   streams `EntityPoseSnapshot` (already the path for kerfur/fossilhound). Clients materialize a
   **parked** mirror (`DisableCharacterTicks` — no local AI, no CMC) driven by the pose stream
   (`npc_mirror.cpp`). Enumerate the host's wisps cheaply via `gamemode.killerWisps` (no
   per-frame GUObjectArray walk).

3. **Targeting already treats peers equally (§1.3) — keep it.** No change to `scanForActors`;
   it picks the nearest `mainPlayer_C` (host OR any client puppet). This is MTA's
   `CPedSync::FindSyncer` nearest-player shape, already satisfied because our puppets are
   `mainPlayer_C`.

4. **Route the grab/lift/dismember/kill from "local player" to `Target` (§1.4.1).** On the
   host, prevent the BP's `getMainPlayer()`/`GetPlayerPawn(0)` effects from landing on the host
   player when `Target != localMainPlayer`, and re-emit them onto `Target`:
   - **Damage** (`@4916`, 4×24): when `Target` is a client puppet, do NOT apply locally; call
     `player_damage::SendPlayerDamage(victimSlot, 24)` — the established host→victim vitals relay
     (`player_damage.cpp`; client `OnWireDamage` applies `InvokeAddPlayerDamage` to its own
     player). When `Target` is the host player, the SP path is correct as-is.
   - **Lift / held / sittingOn / hide-mesh / attach-to-`playerGrab`** (`@2538`,`@3002–3409`,
     `@8480`): these are pose/visual on the victim. For a **client victim**, replicate a new
     reliable "wisp-grab" event (victim slot + wisp eid) so the victim client plays its OWN
     grab — attach its local player to the **mirrored** wisp's `Mesh:playerGrab`, set
     `held`/`sittingOn` locally, run the lift lerp locally — and the existing puppet pose stream
     carries the lifted pose back to host + other peers. (`Target` routing on the host covers
     the host-victim case directly.)
   - **Kill** (`@7560` `ragdollMode(true,false,true)`): **DO NOT call `ragdollMode` on a
     puppet.** `ue_wrap/engine_playerragdoll.cpp` documents that VOTV's `ragdollMode` is
     GLOBALLY scoped and **kills the host regardless of params** (2026-06-01 RE). Instead, send a
     reliable "wisp-kill" event to the victim slot; the **victim client** runs its own local
     death/ragdoll. The protocol already streams non-death ragdoll (`RagdollPose=16` +
     `kStateBitRagdoll`) but **explicitly excludes death** ("death = native SP menu flow, ends
     the session", protocol.h v20 notes) — so **per-peer death/spectate is a genuinely new coop
     concept** and the headline design decision (see §1.8 / validation): a client's wisp-death
     must NOT tear down the session; it should trigger that client's local death/game-over (or a
     spectate state) while the host world keeps running.
   - **Gibs** (`g1..g4`, `prop_bloodGib_*`): spawned on the host attached to the wisp's limbs,
     then dropped (`dr1..dr4`). Route them through the **prop pipeline** (remote_prop / EntitySpawn
     for actors), or — cheaper — let the **client mirror** spawn its own cosmetic gibs on the
     wisp-kill event (they are pure visuals; exact transforms are non-authoritative). Prefer the
     latter for bandwidth.

5. **One victim at a time = a host-side claim (device_occupancy precedent).** The user's
   "kill ONE peer at a time" maps cleanly to the `device_occupancy` claim table (`DeviceClaim=51`;
   `OccupantOf(key) -> slot | 0xFF`, host-arbitrated, per-slot release on disconnect). Model each
   in-progress grab as a claim: key = the wisp's element id, occupant = the victim slot. The host
   sets the claim at the `grab` transition (`@5978 grab:=true`) and clears it at `releasePlayer`/
   destroy. Two guarantees fall out: (a) a single wisp only ever has one `Target` (inherent — it
   is one `APawn*`), and (b) a **victim** can be claimed by only one wisp at a time (claim is also
   keyed so a second wisp targeting an already-grabbed victim must pick another or wait). This
   also gives the disconnect-safety: a victim leaving mid-grab releases the claim.

MTA precedent: host-authoritative AI with nearest-player ownership is exactly
`CPedSync`/`CPedSyncPacket` (server owns the ped, delegates to the nearest player, re-checks
every 500 ms). We diverge by keeping the AI fully host-side (no per-client syncer handoff —
simpler, and VOTV wisps are few + central) but inherit the nearest-player target selection.

## 1.7 Validation list — Topic 1
- [ ] Two clients + host at night: exactly one `killerwisp_C` exists per host (client
      `killerWispSpawner_C` suppressed; `len(gamemode.killerWisps)` identical on host, mirrored
      count on clients).
- [ ] Wisp acquires the **nearest** of {host, client A, client B} — verify it chases a client
      when that client is closest (proves `mainPlayer_C` puppet eligibility).
- [ ] Wisp grabbing client A deals damage to **client A's** saveSlot.health (not the host's),
      via `SendPlayerDamage(slotA,…)`; host health unchanged.
- [ ] Wisp kill of client A triggers client A's local death/game-over WITHOUT ending the
      session or affecting host/client B (the death-exclusion gap is handled).
- [ ] Lift pose + limb gibs are visible on all three machines (host mirror + client mirror).
- [ ] Two wisps cannot grab the same victim (claim table); a victim disconnecting mid-grab
      releases the claim and the host player is never collaterally killed.
- [ ] No per-frame GUObjectArray walk added (enumerate via `gamemode.killerWisps`).

## 1.8 UNKNOWNs / honest gaps — Topic 1
- **Day-gating ("day 30–40")** is NOT fully proven. The spawner BP gates on night window +
  `gamemode==7` + the 4-cap; any *additional* late-game enablement (a day-gated ticker that
  spawns/enables `killerWispSpawner_C`, or a story flag) is unresolved. Dump where
  `killerWispSpawner_C` enters the world: grep `paklist.txt`/the map exports
  (`research/pak_re/_map_untitled_*.json`) for `killerWispSpawner`, or check a `ticker_*` that
  references it. Also re-read `mainGamemode` for a `killerWisps`-enabling day condition.
- **`gamemode==7`** byte value — I read it as a game-mode discriminator (story/sandbox/etc.); the
  exact enum name is not confirmed (compare against `mainGameInstance.gamemode` enum).
- **Per-peer death/game-over policy** is a DESIGN decision, not an RE fact: VOTV death is the
  native SP menu flow. What a client "dying" should do in coop (local game-over screen? spectate?
  respawn?) is unspecified by the engine and must be chosen. This is the single biggest open
  question and should drive a follow-up design doc.
- **`fatality` vs alt `@12821` montage branch** (when `cast<mainPlayer_C>(target)` FAILS, i.e.
  `target` is a `kerfurOmega_C`/`fossilhound_C`): not traced — only the player-victim `fatality`
  path was disassembled. Relevant only if we later mirror wisp-vs-NPC kills.
- **`Stretch`/beam visuals** (`beam1_R/beam2_L/beam3_B`, `beamsEnds`): the wisp's tractor-beam
  cosmetics are pose-adjacent; whether the pose stream alone reproduces them or they need a phase
  bit is untested.

---

# TOPIC 2 — PER-PLAYER CLIENT INVENTORY

## 2.1 How VOTV persists inventory in SP — `UsaveSlot_C` (SDK `saveSlot.hpp`, size `0xE41`)

VOTV serializes the **entire game** (player + world) into ONE monolithic `UsaveSlot_C :
USaveGame`. There is no per-player partition — SP has one player. The fields split cleanly
into **player-scoped** vs **world-scoped**:

**Player-scoped (what a Minecraft-style per-player file must own):**

| Field | Offset | Type | Note |
|---|---|---|---|
| `inventoryData` | `0x02E0` | `TArray<Fstruct_save>` | **THE inventory** (stored items, full per-item state) |
| `equipment` | `0x0440` | `TArray<Fstruct_equipment>` | equipped items |
| `hold` | `0x0450` | `TArray<Fstruct_equipment>` | **held items (hands)** |
| `equipment_def` / `hold_def` | `0x04C0` / `0x04B0` | `TArray<Fstruct_equipment>` | defaults snapshot |
| `beginEquipment` / `_def` | `0x08C8` / `0x08D8` | `TArray<FName>` | starting kit |
| `droneItembox` | `0x06A0` | `Fstruct_save` | drone item box (per-player drone) |
| `Points` | `0x0090` | `int32` | money (**already shared** via balance_sync — see 2.4) |
| `health`/`p_health`/`maxHealth` | `0x0428`/`0x0658`/`0x08B4` | float | vitals (already vitals-synced) |
| `food`/`sleep`/`battery` | `0x00E4`/`0x00E8`/`0x0100` | float | needs |
| `playerTransform`/`def` | `0x0210`/`0x0240` | `FTransform` | spawn position |
| `Strength`/`agility` | `0x0888`/`0x088C` | float | player stats |
| `crafts` | `0x08B8` | `TArray<FName>` | known recipes |
| `food_consumed`/`food_tolerance`/`photos`/`drawings` | … | arrays | personal misc |

**World-scoped (must stay the host's — keep transferring these as today):** `objectsData`
`0x0300`, `dishData` `0x02F0`, `grimeData` `0x0808`, `trashPilesData` `0x0818`,
`primitivesData` `0x0E30`, `subLevelData_*` `0x0CF8/0x0D48/0x0DE0`, `emails` `0x0118`, `Day`
`0x065C`, `savedtime`/`savedSignals*`/`catchedSignals`, `Task`/`taskNew`, `advancements`,
`objectsCount`, `loan`, `arirReputation`/`arirPranks*`, `mainMap`/`subArea`/`Level`,
`localGameRules`, `moonPhase`.

The **runtime** counterpart on the live player (`mainPlayer.hpp`) is `equipment`
(`TArray<FName>` `@0x0B58`), `equipmentTags` (`@0x0CC8`), the `equipped_*` bools
(`equipped_emf/geiger/metalDetec/krampushat`), the hotbar 1–10 input slots, and the
held-item actors. The persisted truth, however, is the saveSlot arrays above — apply there.

**Resolving the "inventory" ambiguity the task flagged:**
- (a) **held / inventory items** → `saveSlot.inventoryData` + `hold` + `equipment` (player-scoped;
  serialized as `Fstruct_save`/`Fstruct_equipment`). **This is what Topic 2 must make per-player.**
- (b) **money / points** → `saveSlot.Points`; **today a single SHARED host-authoritative pool**
  (balance_sync, §2.4) — a separate policy decision from inventory.
- (c) **world items on the ground** → props (already prop-synced); only become "inventory" once
  collected, at which point they move into (a) and stop being replicated.
  `inventory_pickup_sync.cpp` confirms the seam: it replicates only the **collect BLIP sound**,
  not the item-into-inventory state — so inventory state is genuinely un-synced today and lives
  solely in each peer's own saveSlot.

`Fstruct_save` (SDK `struct_save.hpp`) is VOTV's generic actor-serialization record: `class` +
`transform` + `key` (FName) + typed key-value arrays (`bools/floats/ints/strings/signals/
classes/vectors/rotators/transforms/bytes`). So `inventoryData` is a list of these — fully
structured, and serializable into our own per-player blob without inventing a format.

## 2.2 `save_transfer.cpp` end-to-end — where the host inventory leaks in

The v56 join flow copies the host's **whole `.sav`** to the client (`save_transfer.cpp`):

- **HOST** (`OnRequest`→`TickHost`→`TryCaptureBlob_`): on a client's `SaveTransferRequest`, the
  host reads its entire world `.sav` (`SaveGamesDir()/<g_hostSlot>.sav`, ~17 MB) behind a
  **torn-read guard** (size+mtime stable across 3 probes ≥300 ms apart, then a double-read
  CRC match — VOTV `saveToSlot` writes in place), then streams it in **56 KB chunks**
  (`SaveTransferBegin` + `SaveTransferChunk` on `Lane::Bulk`, `kChunksPerTick=4`).
- **CLIENT** (`BulkSink_`→`MaybeFinishLocked_`): reassembles in order, CRC-verifies the whole
  blob, writes it to a per-pid slot `zcoop_<pid>.sav` (tmp+rename), sets
  `ClientState::ReadySlotWritten`. The client's OWN save is never consulted — it is **bypassed
  entirely**.
- **LOAD** (`harness.cpp:318–331`, `DriveMenuModeJoinWorldBoot`): on `ReadySlotWritten` it calls
  `BootStorySaveBlocking(forceFresh=false, slot=CoopSlotName(), mode=ReceivedGameMode())` — the
  client boots into a clone of the host's save.

**Where the inventory rides in:** there is no field-level separation — the host's
`inventoryData`/`equipment`/`hold`/`Points` are part of the monolithic blob, so the client
inherits the **host's** inventory wholesale. That is the defect to fix. The host save transfer
of the **world** is correct and must stay; only the **player-scoped** fields (§2.1) must be
overridden afterwards with the client's own.

## 2.3 Identity key — what we have

There is **no Steam ID / persistent stable identity** in the codebase today. Identity is:
- **slot-based**: `coop::players::Registry` slot 0 = host, 1..`kMaxPeers-1` = clients
  (ephemeral, reused across joins);
- **nickname**: `player_handshake` `SanitizeNickname` (ASCII, `kMaxNickLen=20`, regex-style
  cleanup borrowed from VoidTogether `SimplifyName`), surfaced as `roster::Row.nick[24]`.

So nothing durable exists today -- the key must be BUILT. **User decision (final,
2026-06-12 after the nickname-vs-GUID comparison: "Ok go with guid"): identity = a
client-generated GUID.** `<playerId>` below = that GUID. Design:
- Generated ONCE on first launch (random 128-bit, hex string), persisted in the client's
  own `votv-coop.ini` (e.g. `player_guid=`); regenerating only if absent.
- Sent in the JOIN handshake (the Join packet gains the field -- protocol bump at
  implementation time; v71+, the v70 numbering is already taken).
- The host keys the per-save file by GUID; the NICKNAME stays display-only -- and is ALSO
  written INSIDE the blob (plus a lastSeen stamp) so the on-disk files stay human-
  identifiable despite the opaque filename (mitigates the readability drawback).
- Documented tradeoffs accepted with the decision: identity is per-INSTALL (a reinstall or
  another PC = fresh inventory unless the ini's guid line is copied over -- mention the ini
  key in the user-facing docs); renames are free; same-nick peers never collide.

## 2.4 Money is already shared (note, don't double-handle)

`balance_sync.cpp`: the host polls `saveSlot.Points` and broadcasts `BalanceSync` to all;
clients mirror it (retry until `WritePoints` lands). So **money is a single shared
host-authoritative pool today**. If per-player money is desired (Minecraft-style), that is a
change to `balance_sync` (per-slot balances), **separate** from the inventory blob — call it
out explicitly in the design rather than silently entangling it.

## 2.5 Coop design recommendation — Topic 2 (Minecraft per-player model)

**Store per-player inventory blobs on the HOST, keyed by a stable player id, and on join push
the CLIENT'S OWN inventory (not the host's) over the top of the world save.**

1. **On-disk layout (host machine, host's save folder) — PER SAVE, not global**
   (user directive 2026-06-12: "Not just coop_players/id but save_name/coop_players/id" --
   different saves/worlds carry different inventories, exactly like Minecraft's per-world
   playerdata/):
   `SaveGamesDir()/<save_name>/coop_players/<guid>.json` (or `.sav`), where `<save_name>`
   is the hosted slot's name (e.g. `s_1234`; VOTV slots are flat `.sav` FILES, so this is a
   sibling folder derived from the slot name) and `<guid>` is the handshake GUID (2.3) —
   direct analog of Minecraft `world/playerdata/<uuid>.dat` and MTA's `userdata` rows
   (`CAccountManager`: SQLite `userdata(userid,key,value,type)`,
   `reference/mtasa-blue/Server/.../CAccountManager.cpp:44`). One file per player PER SAVE;
   the host owns all of them; each blob carries the owner's nickname + lastSeen for
   human identification. First join (no file) = empty/default inventory (run the BP's own
   `reset_player_inventory`/`reset_player_hand`/`reset_player_equipment`, which exist on
   `saveSlot`).

2. **Blob contents:** the player-scoped fields from §2.1 — minimally `inventoryData` + `hold` +
   `equipment` (+ `beginEquipment`, `droneItembox`); optionally stats/needs/position if we want
   full per-player persistence. Serialize from the structured `Fstruct_save`/`Fstruct_equipment`
   (don't reinvent: reuse the existing blob transport — `SaveTransferChunk` u32-idx+bytes on
   `Lane::Bulk`, or the `blob_chunks.cpp` / `signal_wire.cpp` chunked-blob precedents already in
   the tree).

3. **Runtime ownership:** the **client owns its live inventory** (it picks up / equips on its own
   machine after the override at join). The client cannot read its inventory off the host (the
   host only has a pose-puppet for that client, no real inventory state). So the **client streams
   its inventory to the host** for persistence — periodic autosave + on a local inventory-change
   edge. This mirrors MTA, where the account data lives server-side but is written through from
   the player's session.

4. **Join injection point:** `harness.cpp:328`, immediately AFTER
   `BootStorySaveBlocking(...)` returns (the client is now in-gameplay as a clone of the host
   world+player). At that point: (a) the host has already sent this client's persisted blob
   (a new reliable, sent alongside `SaveTransferBegin`), and (b) the client applies it onto its
   live `saveSlot`/`mainPlayer` — overwrite `inventoryData`/`hold`/`equipment` with the client's
   own (replacing the host's), then re-equip via the BP (`mainPlayer.addEquip(Fstruct_save)` /
   `putObjectInventory2` / `updateEquipment` exist as reflected entry points). First join → apply
   empty/default.

5. **Host-side write timing (MTA `CAccountManager` cadence — `Save` only when changed, every
   15 s, + on shutdown):**
   - **Periodic**: persist each connected peer's last-received inventory blob to its file on the
     host's autosave tick (dedup on change, like balance_sync's send-on-change).
   - **On disconnect** (`CancelForSlot`/the per-slot disconnect path): flush that slot's blob to
     disk (the player's authoritative last state).
   - **On host quit/shutdown**: flush all connected peers' blobs.
   - **The host's OWN inventory** stays in the host's normal `.sav` (it is the SP player); only
     CLIENT inventories get the `coop_players/` side files. (Optionally also write the host's
     player into a `coop_players/<hostId>.json` for symmetry, but it is already in the main save.)

6. **What to STOP doing:** do not let the client inherit the host's player-scoped fields. Two
   options — (a) leave `save_transfer` as-is (whole-save) and OVERRIDE at the injection point
   (§2.5.4) — simplest, lowest-risk, recommended; or (b) have the host **scrub** the player-scoped
   fields out of the transferred blob before sending (clean but touches the 17 MB capture path).
   Prefer (a): override-after-load is a small, well-contained change at one seam.

MTA precedent fit: `CAccountManager`/`CAccount` is the per-player-persisted-data model
(server-owned KV keyed by `userid`, periodic + shutdown saves) — our `coop_players/<id>.json`
is the file-per-player simplification of it (RULE 3: no SQLite dependency needed).

## 2.6 Validation list — Topic 2
- [ ] Client A first-join → empty/default inventory (NOT the host's items).
- [ ] Client A picks up items, disconnects → `coop_players/<A>.json` written on host with those
      items; host's own inventory unchanged.
- [ ] Client A rejoins (same world) → its own items restored; client B joining gets B's file
      (or empty), never A's or the host's.
- [ ] World state (props/day/emails/signals) is still the host's — only player-scoped fields
      differ per peer.
- [ ] Periodic autosave persists mid-session inventory (kill the host process → last ≤15 s of
      each client's inventory survives).
- [ ] Money behaves per the chosen policy (today: shared pool — confirm no regression).

## 2.7 UNKNOWNs / honest gaps — Topic 2
- **Identity = client-generated GUID in the ini + Join handshake** (user decision FINAL
  2026-06-12 "Ok go with guid", after a nickname interlude; see 2.3). Per-install identity
  is the accepted tradeoff; nickname is display-only, embedded in the blob.
- **Apply path on the live player**: I confirmed the saveSlot fields + that `mainPlayer.addEquip`/
  `putObjectInventory2`/`updateEquipment`/`saveSlot.reset_player_*` exist as reflected entries,
  but the exact safe call sequence to *re-materialize* `inventoryData` onto a live player
  (vs writing the saveSlot then forcing a reload) is **untraced** — needs an SP probe of the
  load-time inventory rebuild (`saveSlot.save`/`gatherData`→player chain).
- **Drone / ATV item boxes** (`droneItembox`, ATV inventory container) — per-player vs shared is a
  policy call (the drone may be a shared world object). Not resolved.
- **Blob size / churn**: `inventoryData` is unbounded (a hoarder's base). The per-change stream +
  periodic write must be throttled; size budget unmeasured.
- **Concurrent same-machine peers**: solved by the GUID key as long as each game-folder copy
  carries its OWN votv-coop.ini guid (our 4-copy convention does -- each copy has its own
  ini); the dev launchers need no special handling.

---

## TOOLING APPENDIX (exact commands used)
```
research/pak_re/tools/ka/.../kismet-analyzer.exe to-json <uasset> > research/bp_reflection/<name>.json
python research/bp_reflection/_fn.py  killerwisp {scanForActors,canReach,gather,capture,releasePlayer,moveToTarg,updateSense,walk,stretch,ReceiveTick,ReceiveBeginPlay,BndEvt__...}
python research/bp_reflection/_cfg.py killerwisp ExecuteUbergraph_killerwisp [entry]
python research/bp_reflection/_scan.py killerwisp {getMainPlayer,GetPlayerPawn,Gib,SpawnActor,isRagdoll}
python research/bp_reflection/_cfg.py {ticker_wispSpawner,killerWispSpawner} ExecuteUbergraph_<asset>
# SDK headers: Game_0.9.0n/.../CXXHeaderDump/{killerwisp,saveSlot,mainPlayer,struct_save}.hpp
# src read: coop/{ambient_spawner_suppress,npc_mirror,player_damage,save_transfer,inventory_pickup_sync,device_occupancy,balance_sync,roster,players_registry}; ue_wrap/engine_playerragdoll.cpp; harness/harness.cpp:278-336
# MTA: reference/mtasa-blue/Server/mods/deathmatch/logic/{CPedSync.cpp,CAccountManager.cpp}
```

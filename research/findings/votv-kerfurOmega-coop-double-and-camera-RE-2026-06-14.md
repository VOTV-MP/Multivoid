# KerfurOmega coop bug — deep RE + root-cause map + fix design (2026-06-14)

User report (verbatim): *"KerfurOmega npc if left active and saved - then load save
and host game it follows host, then if client connects he also has his own kerfur
which follows him and he doesn't see host's kerfur and host doesn't see client's
kerfur - weird buggy shit. And if turned off and grabbed - it appears for client and
host. And when host turns off active kerfurs - from client's perspective the active
kerfurs turn into a floating camera object attached to air."*

Directive: **no guessing, no iterative fixing, full deep RE.** This doc is the RE
ground truth (3 parallel agents, all anchored to cooked kismet + CXXHeaderDump +
UE4SS object dump) and the root-cause-clean fix design.

---

## !!! CORRECTION (2026-06-14, post-v74-test) — v74's SAVE-KEY ADOPTION WAS BUILT ON A FALSE PREMISE

v74 shipped, the user tested, **nothing changed** (the double persisted). Log forensics +
a second deep-RE pass (4 agents) + direct bytecode extraction settled the real cause:

**The kerfur's int_save `Key` is NON-DETERMINISTIC across peers, so key-equality adoption can
NEVER match.** Proof (raw bytecode, `kerfurOmega.json`):
- `kerfurOmega::loadData` (Exports[224]) **OVERRIDES** the int_save base (`SuperStruct=0`, no
  parent call) and restores `state`/`type`/`hasFloppy`/`loadedHoldObjectKey`/`drip`/`floppy*`
  — but writes the `Key` field **ZERO times** (verified: 0 occurrences of a `key` write target;
  the only key-ish write is `loadedHoldObjectKey`@0x09B0, the HELD-object key, NOT the save
  Key@0x09B8).
- The base `actor_save::loadData` DOES `this.key = data.key` (stmt[0] EX_Let InstanceVariable
  `key`←StructMember `key_64_...` of the `data` record) — but the kerfur's override REPLACES it,
  so that restore never runs for a kerfur.
- Therefore every load, the kerfur's UCS `getKey → lib::assignKey → generateRandomKey`
  (GenerateRandomBytes(16)→Base64Url, a fresh 22-char FName) mints a **NEW random key** because
  the field is `None`. Host key K_host ≠ client key K_client, always.
- **Why props work but kerfurs don't:** props use the BASE `loadData` (deterministic key restore)
  → same key on both peers → the prop channel's `ResolveLiveActorByKey` adoption works. The
  kerfur is the one class whose override silently drops the key restore. The v74 design copied
  the prop *shape* (match-by-key) onto a class where the precondition (deterministic key) is
  false.

**Second, independent failure:** the one-shot reconcile sweep (`DestroyUntrackedClientNpcs`,
the intended safety net) **never produced a sweep line all session** — net_pump clears
`g_clientNpcReconcilePending` on any transient `!isConnected` (which the save-transfer world
swap causes), and it only logs on `found>0`, so it is both unreliable and unobservable.

**Corrected fix (supersedes §7/§10 below):** key-adoption is abandoned. The client's local
save-kerfur spawns via un-hookable `EX_CallMath` (§2) at an unpredictable time, so the robust
identity is **class + untracked-local**, resolved by a DEFERRED POLL that adopts the real local
kerfur as the host mirror (camera-safe: a fully-initialized actor parked AFTER init, unlike a
parked-at-spawn fresh mirror), with a reliable ghost sweep for genuine orphans. See §11 (AS
BUILT, corrected) for the implementation. §7 and §10 below are the SUPERSEDED v74 design, kept
for the post-mortem record.

---

Anchors: byte offsets are into `research/bp_reflection/_kerfuromega_uber.txt`
(== `kerfur_actor_uber_blocks.txt`, the offset-aware `ExecuteUbergraph_kerfurOmega`
disassembly); member offsets from `CXXHeaderDump/kerfurOmega.hpp`; L# from
`UE4SS_ObjectDump_GAMEPLAY_SAVE.txt`.

---

## 0. The four observations, mapped to mechanisms

| # | Observation | Root mechanism (proven) |
|---|---|---|
| 1 | Saved-active kerfur: host follows host; joined client gets its OWN kerfur following the client; mutual invisibility | Client double-materializes a save-persisted NPC; suppressor is structurally blind; one-shot reconcile sweep is the only cleanup and is fragile across the save-transfer world swap; NPCs have NO key-adoption path (§2, §3) |
| 2 | Turned-off + grabbed → appears for both (WORKS) | The off form is a plain `Aprop_C`-derived prop on the established prop snapshot/lifecycle channel (§5) — this is the contrast that proves the prop path is right and the NPC path is the gap |
| 3 | Host turns off active → client sees a "floating camera attached to air" | The kerfur owns a visible camera child (`cam` UChildActorComponent → `prop_camera_good_C`); on the client's non-standard parked MIRROR, the child survives the mirror's `K2_DestroyActor` (§4) |
| — | (deeper) a parked mirror still runs 3 timers + has no synced State/face | `DisableCharacterTicks` stops actor/CMC tick but NOT FTimerManager timers; `timer_face`/`timer_kerf`/`checkDoor` survive and run local AI on the mirror (§6) |

---

## 1. The actor (CXXHeaderDump/kerfurOmega.hpp, `AkerfurOmega_C : ACharacter`, 0x9F0)

Child actors / camera-relevant members:
- `UChildActorComponent* cam;` @0x05A0 — a **true engine UChildActorComponent**;
  `ChildActorClass = prop_camera_good_C` (kerfurOmega.json L128856 `ChildActorClass`
  → `prop_camera_good_C`; template `cam_GEN_VARIABLE_prop_camera_good_C_CAT`).
  Parented under `meshLag` (USpringArmComponent @0x0598).
- `Aprop_camera_good_C* Camera;` @0x0628 — **NOT separately spawned**: a cached
  pointer to `cam.ChildActor` (proven `@24497 cast<prop_camera_good_C>(cam.ChildActor)`
  → `@24594 Camera := <cast>`). `prop_camera_good_C : prop_camera_bad_C` is a VISIBLE
  camera prop (`UStaticMeshComponent head`, `UCameraComponent Preview`, spotlight —
  prop_camera_bad.hpp). This is the object the user sees floating.
- `AkerfusFace_C* face;` @0x07C8 — a **separately-SpawnActor'd standalone actor**
  (`makeFace()`: BeginDeferred+FinishSpawning `kerfusFace_C`, NO attach, NO Owner).
- `AobjectViewer_C* Viewer;` @0x09D8 — on-demand upgrade-UI render actor (not in the
  spawn path; irrelevant to turn_off of an idle pet).
- `comp_radarPoint` (Ucomp_radarPoint_C @0x04D8) — passive radar marker.

State/AI-relevant: `State` (enum_kerfurCommand @0x05C8), `TargetActor` (AActor* @0x0648),
`targetLoc` @0x0638, `remoteControl` @0x07A8, `kill` @0x05E0, `sentient` @0x07D8,
`Key` (FName @0x09B8) + `GetKey`/`getOnlyKey`/`setKey`/`processKeys`, `dropProp`
(TSubclassOf<prop_kerfurOmega_C> @0x0850), int_save funcs `getData`/`loadData`/`ignoreSave`.

---

## 2. Symptom 1 — save serialize + the DECISIVE respawn opcode (native, invisible)

**Save:** an active kerfur is the NPC itself. It implements `int_objects`+`int_save`
(kerfurOmega.json import L24/L26); `saveObjects` gathers `int_objects_C`, casts to
`int_save_C`, gates on `ignoreSave()`, calls `getData()` → an `Fstruct_save` record
(class_3 = `kerfurOmega_C`, transform, key, State/kill/floppy/holdObject) into
`saveSlot.objectsData@0x300` / `GObjStack@0x198` (votv-save-path-RE §2).
`kerfurOmega::ignoreSave` is hardcoded `false` (kerfurOmega.json L117797-117825:
`LetBool(ignoreSave, False); Return`) → **an active kerfur is ALWAYS saved.**

**Respawn (the crux):** the per-record actor spawn inside `mainGamemode::loadObjects`
(FunctionExport L320864-336832) is, verbatim:
```
@L329468  LetObj BeginDeferredActorSpawnFromClass_RV =
            EX_CallMath StackNode=-1064 [ Self, ObjectConst(class_3), ComposeTransforms, Byte(1), NoObject ]
@~L329585 EX_CallMath StackNode=-1068  // FinishSpawningActor
@~L332447 EX_VirtualFunction "loadData"  // restore state in place
```
`BeginDeferredActorSpawnFromClass` is invoked as **`EX_CallMath` — a direct native
call, NOT dispatched through `UObject::ProcessEvent`.** Our client suppressor
(`npc_sync.cpp NpcSuppress_Interceptor`) is a UFunction interceptor that fires ONLY on
ProcessEvent-dispatched `BeginDeferredActorSpawnFromClass`. **It is structurally blind
to the save-load spawn** (the same EX_CallMath bypass documented in
votv-kerfur-convert-RE §3; log census: zero `npc-suppress[client]: skipping` lines have
ever been emitted by a game-initiated spawn). `getObjectFromKey` (mainGamemode.json
L223425) is only a post-spawn key→actor lookup, not the spawner.

**NPC-vs-prop:** an ACTIVE kerfur round-trips as the NPC (class_3=kerfurOmega_C); a
TURNED-OFF kerfur round-trips as the inert `prop_kerfurOmega_C` prop (which has NO
int_save and only runs `spawnKerfuro` from the radial menu, never from a load callback).
Two independent records/paths — "saved active" does NOT pass through the prop form.

---

## 3. Symptom 1 — why suppress + sweep both miss it (the double)

The client ends with **two kerfur actors**:
1. **Its own save-spawned kerfur** — `loadObjects` native EX_CallMath spawn → invisible
   to the PE-only suppressor → AI-active, follows the client (§6 follow target). The
   host never sees it (clients never broadcast NPCs).
2. **A host mirror** — host `RegisterExistingWorldNpcs` (subsystems.cpp:168) registers
   its own save-loaded kerfur + broadcasts `EntitySpawn`; client `OnEntitySpawn`
   (npc_mirror.cpp:64) spawns a **brand-new** actor (BeginDeferred+Finish), parks it.
   **There is NO NPC fuzzy/key adoption** (confirmed: npc_mirror/npc_sync have no
   key-bind path; EntitySpawnPayload carries no key) → the client never adopts #1 as the
   mirror; it makes a duplicate #2.

`DestroyUntrackedClientNpcs()` (npc_mirror.cpp:434, net_pump.cpp:555) is the SOLE
mechanism meant to delete #1 (untracked) while keeping #2 (tracked). Its universe +
timing are statically correct (it would match a kerfur skin subclass via the
subclass-aware allowlist walk). But it is a **one-shot latch** armed by `ClientWorldReady`
and cleared after one run. The save-transfer boot is a **real UWorld swap** even when the
map name is unchanged (votv-snapshot-adoption RC1/Fork-A: "VOTV's boot/save-load re-issues
`open untitled_1`"). The client re-announces `ClientWorldReady` on every re-seed; the
single sweep can fire against an earlier/stale world generation (latch consumed before
`loadObjects` placed the kerfur in the FINAL post-swap world), so ghost #1 is never
re-swept. **Result: #1 survives, #2 exists separately → the double + mutual invisibility.**
(The exact ordering is the one runtime item to confirm with a client-log line — but the
mechanism class is established; the fix below removes the dependence on the one-shot
sweep entirely.)

---

## 4. Symptom 3 — the floating camera

`cam` is a `UChildActorComponent` whose child is `prop_camera_good_C` (a visible camera
prop). `ReceiveDestroyed()` (ubergraph @31608) explicitly `K2_DestroyActor`s **`face`
only** (@31883, unconditional) and removes self from `gamemode.allKerfuros` (@31608);
it does NOT touch `cam`/`Camera` — those rely on the engine UChildActorComponent cascade.
`Dest(DestroyedActor)` (@33185) is the held-object OnDestroyed delegate, unrelated.

- **Host-own kerfur turn_off**: `dropKerfurProp` → `K2_DestroyActor(self)` → engine
  `Destroy` cascades to the `cam` component → its child `prop_camera_good` is destroyed.
  Host is correct (consistent with the bug being client-side).
- **Client mirror**: spawned non-standard (BeginDeferred+Finish then parked,
  npc_mirror.cpp:186-310). `FinishSpawningActor` runs in an already-begun world →
  the mirror's `cam` registers and **creates a real `prop_camera_good` child on the
  client**. On host turn_off the converge broadcasts EntityDestroy → client
  `OnEntityDestroy` → `K2_DestroyActor(mirror)`. **The visible orphan is that mirror's
  `cam` child (`prop_camera_good`) surviving the mirror destroy.** The stick mechanism
  is ruled out (votv-camera-stick-RE §2: `canStick` requires `!IsChildActor`; the eye
  camera IS a child actor, so it can never re-parent/stick).

Static kismet establishes the orphan's IDENTITY and rules out the stick path, but cannot
prove from bytecode WHY the engine cascade fails specifically on the deferred-spawned+
parked mirror (default UE4.27 semantics say it should cascade). **This is the one
genuinely engine-level open item** — see §8 probe. Crucially, the fix below removes the
non-standard mirror entirely (adopt the client's real save-kerfur instead), which makes
the client's teardown identical to the host's working cascade.

---

## 5. Symptom 2 (works) — the contrast

Turned-off + grabbed = a `prop_kerfurOmega_C` (plain Aprop_C lineage) riding the prop
snapshot/lifecycle channel with exact-key adoption — exactly the model NPCs lack. This is
the positive control: the prop channel's keyed adoption is why "off+grabbed" syncs both
ways, and its absence on the NPC channel is why "active" does not.

---

## 6. Follow target + parked-mirror AI leak (agent 3, byte-exact)

**Follow is per-peer-local, no networked target.** `move()` State machine: `State==0`
(follow) → `@16987 GetPlayerPawn(self,0)` → `@17013 CreateMoveToProxyObject(...,
GetPlayerPawn_RV, accept=100cm)` → `@16936 TargetActor := GetPlayerPawn(0)`.
`targetLocation()` returns `TargetActor.GetActorLocation()`. `GetPlayerPawn(world,0)` =
each machine's own local player-0 → **host kerfur chases host, client kerfur chases
client.** Body-facing (idle): `@30165 GetPlayerPawn(0)` → `@30241 FindLookAtRotation` →
`@28564 Mesh.K2_SetWorldRotation` — also faces the LOCAL player. Both run ONLY from
`ReceiveTick` (entry @26738) → STOP when actor tick is off.

**Parked mirror (`DisableCharacterTicks` = CMC tick off + actor tick off only):**

| Behavior | Driver | Parked mirror |
|---|---|---|
| Body-facing / follow / walk-speed | ReceiveTick @26738 | STOPS (good) |
| meshVel (AnimBP locomotion input) | ReceiveTick | STOPS — our pose drive writes CMC.Velocity instead |
| `timer_face` 0.15s loop → `setFace()` (reads local camera) | SetTimerDelegate @8184 | **SURVIVES — wrong local face** |
| `timer_kerf` 200s loop → spooky-kill roller (sets isSpooky, State=1, lookAt=local cam, customLookAt=true) | SetTimerDelegate @8714 | **SURVIVES — can flip the mirror into a local kill state + fight v39 lookAt** |
| `checkDoor` 0.5s loop | SetTimerDelegate @24444 | **SURVIVES — stray local AI** |
| Head-look lookAt | AnimBP BUA (mesh tick) | SURVIVES — already overridden by v39 customLookAt drive |

BeginPlay also `@24324 Array_Add(gamemode.allKerfuros, self)` → a mirror adds itself to
the client's authoritative kerfur list (and a second `comp_radarPoint`). So a correct
mirror needs MORE than `DisableCharacterTicks`: **disarm timer_face/timer_kerf/checkDoor**
and stream **State + face/isSpooky + hold/equip + on-car (occupyCar)** so the AnimBP +
visible state match the host (pose/bodyYaw/lookAt are already streamed).

---

## 7. Fix design — host-authoritative kerfur via SAVE-KEY ADOPTION (RULE 1, MTA shape)

Root cause across §2–§6: **KerfurOmega is a save-persisted, AI-driven NPC that BOTH peers
load from the same save, but our NPC sync was built for host-only-spawned enemies** (host
spawns / client suppresses+mirrors). Under save-transfer join that model produces a
double, leans on a fragile one-shot sweep, and creates a non-standard parked mirror whose
camera child orphans on destroy. The prop channel already solved the same problem for
save-persisted props with **exact-key adoption** (snapshot RC4/Fork-B). The root fix is to
give NPCs the same thing.

**Design (mirrors prop snapshot adoption; MTA = reconcile the client entity set to the
server's by identity):**

1. **EntitySpawn carries the save key.** Add a `WireKey key` to `EntitySpawnPayload`
   (96B → 128B, fits the 228B datagram). Host reads each registered world-NPC's `Key`
   (`kerfurOmega_C::GetKey`, FName @0x09B8) in `RegisterExistingWorldNpcs` and sends it.
   Keyless NPCs (event/host-spawned enemies) send key.len=0 → today's spawn-new path.

2. **Client ADOPTS its own save-spawned NPC by key instead of spawning a duplicate.**
   `OnEntitySpawn`: if `key.len>0`, walk the client's live allowlisted NPCs (or a
   key→actor index built at world-up) for a matching `GetKey`; on match, **bind that
   actor as the mirror** (SetActor + MirrorManager Install under the host eid) and PARK
   it. No new actor; no double. No match (fresh-boot client) → spawn-new mirror (today).
   This makes #1 and #2 the SAME actor.

3. **Retire the one-shot reconcile as the primary mechanism.** With adoption, the
   client's save-kerfur becomes the host mirror by construction; it is no longer
   "untracked." Keep `DestroyUntrackedClientNpcs` only for genuinely host-only NPC
   classes (keyless), and re-arm it across the save-transfer world swap (tie to the
   SeedGeneration stamp Fork-A already uses) so a keyless ghost can't slip the one shot.

4. **Park an adopted kerfur THOROUGHLY (not just tick-off).** Extend the mirror park to
   **disarm `timer_face`/`timer_kerf`/`checkDoor`** (ClearTimer by function name, or a
   kerfur-specific neutralize) so the mirror runs zero local AI. (Generalizes
   DisableCharacterTicks for timer-armed NPCs.)

5. **Stream host-authoritative kerfur state for a correct mirror.** Add a small kerfur
   state lane (State/enum_kerfurCommand + isSpooky + face/expression + holdObject +
   occupyCar) so the AnimBP + visible state match the host. Pose/bodyYaw/lookAt already
   stream.

6. **Symptom 3 falls out of (2).** The adopted mirror is the client's own real,
   fully-constructed save-kerfur → on host turn_off → EntityDestroy → `K2_DestroyActor`
   → the SAME engine cascade that works on the host → the `cam`/`prop_camera_good` child
   is destroyed, no orphan. (If §8's probe shows the cam child still leaks even on a
   real actor, add an explicit `cam.ChildActor` + `face` teardown on mirror destroy as a
   parity-with-host step — but adoption removes the non-standard-spawn root cause first.)

   The turn_off itself is already host-authoritative (kerfur_convert v67): client cancels
   the local verb + requests; host executes + converges (EntityDestroy + the prop). With
   adoption, the converge's EntityDestroy now lands on the adopted real actor.

**Staging:** (Inc-1) wire key + client key-adoption + thorough park (kills the double +
the floating camera — the two user-visible bugs). (Inc-2) the kerfur state lane
(State/face/hold/car) for full mirror fidelity. (Inc-1 is the minimum that resolves all
three reported observations; Inc-2 is correctness polish.)

---

## 8. The single live-probe open item (everything else is statically proven)

Confirm on ONE instrumented LAN join (host save has an active kerfur), reading the CLIENT
log: (a) a `loadObjects` kerfur spawn with NO `npc-suppress[client]: skipping` line
(native-spawn escape — expected); (b) whether `npc-mirror[reconcile]: swept N` fires and
against which world generation (was the one-shot consumed pre-final-loadObjects?); (c) the
host `OnEntitySpawn` for kerfurOmega_C (two live kerfur actors). And for §4: snapshot the
mirror's `cam.ChildActor` / the `prop_camera_good` actor IsValid+Owner immediately before
vs after the mirror `K2_DestroyActor` to prove the cascade outcome. Per CLAUDE.md "verify
by diffing observable state." This confirms ordering; it does not change the §7 design
(adoption removes the dependence on the sweep and the non-standard mirror).

Agent IDs (resumable): child-actor/camera `ad37f871f487ac947`; save/respawn
`a049fd70d9a48ad64`; AI/follow+park `a2453bb67071ed272`.

---

## 9. "Base kerfur" vs KerfurOmega — class enumeration (user note 2026-06-14)

User: there is a "pre-upgraded, less intelligent" normal kerfur that players buy and
later upgrade into a KerfurOmega. VERIFIED against the dumps (RULE: enumerate the real
classes, don't assume):
- No standalone `kerfur_C` / `kerfuro_C` PET PAWN class exists in 0.9.0n. Searched the
  CXX dump, the live UE4SS object dump (full GUObjectArray), and the extracted pak. The
  `Kerfur_C`/`kerfur_C` grep hits are substrings of `murderKerfur_C` / `prop_tv2_kerfur_C`.
- The pet kerfur NPC IS `kerfurOmega_C` (+ ~20 cosmetic skin subclasses; all `: public
  AkerfurOmega_C`). It carries a full UPGRADE SYSTEM: `sentient` bool @0x07D8 +
  `makeSentient()`, `Type` int32 @0x07A4, `getUpgradesList`, `upgradeTake(FName)`,
  `upgradeUI`, `intComs_stuffUpgraded`. The "less-intelligent base → upgraded" progression
  is this same-class flag flip, NOT a separate pawn.
- Corroboration: the convert RE already found a SENTIENT kerfur REFUSES turn_off
  (dropKerfurProp early-returns on `sentient`). So the floating-camera-on-turn_off is the
  NON-sentient/base case; the upgraded one can't be turned off.
- Non-pet kerfur actors (NOT pets, out of scope for this fix): `murderKerfur_C` /
  `murderKerfuroDig_C` (hostile), `skerfuroWalk_C : AActor` (a walking toy),
  `npc_zombie_skerfuro_C : npc_zombie_C` (zombie variant), `grayboarPawn_kerfurOnly_C`.

## 10. AS BUILT (v74, 2026-06-14) -- Inc-1 + Inc-2 + Inc-3, build-clean, 2-audit-verified

Protocol bumped to **v74**. All three increments shipped in one build (DLL 16:29:34, deployed x4
MATCH). Two adversarial audits (perf/thread-safety = 0 CRITICAL; correctness = 4 CONFIRMED, ALL
FIXED below).

**Inc-1 -- no-duplicate adoption + thorough park (the double + camera bugs):**
- `EntitySpawnPayload` +`WireKey key` (96->128B). Host sends each world-NPC's int_save Key
  (`ue_wrap::kerfur::ReadNpcSaveKey`) in `npc_sync::RegisterExistingWorldNpcs` +
  `npc_pose_host::QueueConnectBroadcastForSlot`.
- Client `npc_mirror::OnEntitySpawn` -> `AdoptLocalNpcByKey`: walks GUObjectArray for a live/
  allowlisted/untracked/non-CDO NPC whose Key matches -> BINDS it as the host mirror (no duplicate
  spawn) + thorough-parks. No key / no match -> fresh-spawn mirror (today's path) + thorough park.
- Thorough park = `puppet::DisableCharacterTicks` + `ue_wrap::kerfur::NeutralizeAiTimers` (clears
  timer_face/timer_kerf/checkDoor via K2_ClearTimer -- a parked mirror that still ran those timers
  would self-roll the spooky-kill state + fight the lookAt drive).
- The "floating camera" fix is STRUCTURAL: an adopted mirror IS the real save-kerfur, so its destroy
  runs the real ReceiveDestroyed + engine cascade exactly as the host (no orphan). The explicit
  child-teardown was REMOVED (audit: K2_DestroyActor-ing the cam ChildActor while the cam component
  lives risks a cascade double-destroy/UAF; adoption removes the non-standard mirror = the root cause).

**Inc-2 -- host-authoritative State/spooky mirror:** `EntityPoseSnapshot` reused its pad bytes for
`kerfState`+`kerfFace` + bits `kEntityPoseBitHasKerfurState`/`...KerfurSpooky` (stays 44B). Host
`TickPoseStream` reads `ue_wrap::kerfur::ReadKerfurState`; client `npc_pose_drive::ApplyToEngine`
applies `DriveKerfurState` (writes State + isSpooky). Confirms the spooky/haunt is host-decided +
mirrored (clients never self-roll -- the 200s timer_kerf is neutralized at park). Face material +
hold-object deferred (face needs a setFace call; documented, not shipped).

**Inc-3 -- host-authoritative menu relay + ownership-aware Follow:** NEW `coop/kerfur_command.{h,cpp}`
+ `ReliableKind::KerfurCommand=70` + `KerfurCommandPayload{elementId,command}`. The actionName
PRE-interceptor (kerfur_convert) routes the State-changing verbs (follow/idle/patrol/fix_servers/
get_reports/fix_transformers) to `TryRecordMenuCommand` (cancels local + records). `Tick`: client ->
resolve wire eid + SendReliable; host -> `ExecuteHostCommand(requester=host)`. `OnCommandRequest`
(host) -> `ExecuteHostCommand(requester=senderSlot)`. Non-follow verbs: the host re-runs the REAL
`actionName` via ProcessEvent (`RunActionName`, guarded by thread_local `t_inHostExec` so the replay
doesn't re-intercept itself); State streams via Inc-2. FOLLOW: the host sets State=idle (silences the
BP's player-0 mover) + `g_followOwner[eid]=ownerSlot` + a 500ms `RunFollowLoop` that issues
`UAIBlueprintHelperLibrary::CreateMoveToProxyObject(Pawn=kerfur, TargetActor=owner body, Destination=
owner loc)` toward THE REQUESTER's body (host `Local()` or remote `Puppet(slot)->GetActor()`). The
follow mechanism is the stock UE "MoveTo" async proxy (the kerfur already follows player-0 with it
natively -> validated, not guessed). turn_off stays in kerfur_convert.

**Audit fixes (correctness, all CONFIRMED -> fixed):**
1. Adoption race / skip-keyed risk: the reconcile sweep no longer skips keyed NPCs; instead it is
   DELAYED ~2s past world-ready (net_pump `kReconcileDelayMs`) so adoption binds the save-kerfur
   (-> tracked -> kept) FIRST. Any still-untracked NPC at +2s is a true ghost (keyless escapee OR a
   kerfur whose adoption failed) -> swept. Prevents the un-adoptable-double the immediate skip-keyed
   sweep risked.
2. Camera teardown UAF: removed (see Inc-1 -- adoption is the structural fix).
3. Follow binding leak on client-disconnect: `kerfur_command::OnPeerDisconnect(slot)` (wired in
   `subsystems::DisconnectSlot`) erases the leaver's owned-follow entries + restores State=follow
   so the host's BP resumes (follows the host).

**Deferred (documented, NOT crutches):** face-material live sync (needs setFace RE); hold-object +
on-car mirror; the murder/haunt ATTACK effect against a remote player (a host-auth damage relay like
the Killer-Wisp -- Inc-2 makes the mirror LOOK haunted, not yet attack); ownership-follow visual
polish (the kerfur paths in idle-State -> verify the walk anim shows via the velocity blend);
take_object (needs the requester's held object). `npc_sync.cpp` at 938 LOC (pre-existing >800; my +7
lines are the key read -- extraction proposal: the host PRE/POST/interceptor block).

**Hands-on items the RE could not settle statically (verify in play):** the ownership-follow walk
animation (idle-State + velocity blend); that a non-pawn puppet works as the MoveTo TargetActor (the
Destination fallback covers it either way); the camera no longer floats on turn_off (adoption path).

## 11. (original sec 7 below)

IMPLICATION FOR THE FIX: `kerfurOmega_C` + the subclass-aware allowlist walk already covers
every tier and skin; the save-key adoption is class-agnostic (any allowlisted NPC with a
"Key" property adopts) — a future distinct save-persisted NPC class is a one-line allowlist
add. NEW follow-up surfaced (Inc-3, not these increments): OWNERSHIP-AWARE FOLLOW — in coop
each player buys a kerfur, but host-authoritative AI makes them all follow the host
(`GetPlayerPawn(0)` host-side); a client-owned kerfur should follow that client's puppet
(host drives its AI toward the client puppet). Bigger feature; tracked here.

---

## 11. AS BUILT — v75 (corrected; supersedes §7/§10). CLASS-MATCH adoption via deferred poll

**Root cause (re-confirmed, bytecode):** see the top "CORRECTION" banner. The kerfur's int_save
`Key` is minted RANDOM per peer (`kerfurOmega::loadData` overrides the int_save base, restores
state/type/floppy/loadedHoldObjectKey but NOT `Key`@0x09B8; UCS `getKey→lib::assignKey→
generateRandomKey` mints fresh each load). Key-equality adoption is structurally impossible →
v74 "changed nothing." Props work because they use the BASE `loadData` (restores `this.key=
data.key`, deterministic) — the asymmetry v74 missed.

**The fix (protocol v75, built + 2-pass-audited + deployed x4 MATCH `0eeab3be…`, DLL 19:58):**
- `EntitySpawnPayload`: drop `WireKey key`, add `uint8_t savePersisted` (+`_pad2[3]`) → 96B.
  Host sets `savePersisted = ue_wrap::kerfur::HasSaveKey(actor)` (non-None int_save "Key" = a save
  object the joining client ALSO booted) at the two world-enum/connect-snapshot sends; `=0` at the
  runtime interceptor (host-spawned transients have no local twin). `ReadNpcSaveKey`→`HasSaveKey`
  (bool). `kProtocolVersion` 74→75.
- NEW `coop/npc_adoption.{h,cpp}` (218 LOC): the local kerfur spawns via un-hookable `EX_CallMath`
  (loadObjects), so `OnEntitySpawn(savePersisted=1)` → `ArmAdoption{eid,classW,actorClass,pose}`
  (does NOT spawn). `Tick()` (from `TickClientNpcs`, GT): (1) throttled (5 Hz, only-while-pending)
  GUObjectArray scan → bind each pending entry's NEAREST same-EXACT-class (both peers loaded the
  same save → same skin class) untracked live non-CDO local actor as the host mirror (`BindAsMirror`
  = Install + DisableCharacterTicks + NeutralizeAiTimers — the REAL fully-initialized actor, so its
  destroy cascades the cam child = the floating-camera fix is now STRUCTURAL), else `SpawnFresh
  NpcMirror` after an 8 s timeout (never lose a host NPC); (2) ONE-SHOT ghost sweep
  (`DestroyUntrackedClientNpcs`) once `g_snapshotDelivered && g_pending.empty() && IsInstalled()`
  — removes genuine orphans (e.g. a kerfur turned OFF after the save: host holds only the prop, no
  EntitySpawn → no adoption → swept). `OnSnapshotComplete` (event_feed) sets the gate;
  `OnClientWorldReady` (net_pump) resets per-world; `OnSessionEnd` (DisconnectAll) wipes.
- `npc_mirror.cpp`: `AdoptLocalNpcByKey` DELETED; `OnEntitySpawn` branches on `savePersisted`;
  the spawn body extracted to public `SpawnFreshNpcMirror` (shared by the `=0` path + the adoption
  timeout fallback). net_pump's one-shot reconcile latch (`g_clientNpcReconcilePending`/
  `kReconcileDelayMs=2000`) DELETED — the v74 fixed-delay crutch is gone; the sweep is now gated on
  a REAL completion signal (`SnapshotComplete`), not a sleep.

**Why the ghost sweep is race-safe (verified):** `EntitySpawn` and `SnapshotComplete` are BOTH
`Lane::Bulk` (FIFO); the kerfur EntitySpawn is enqueued early in `ConnectReplayForSlot`,
`SnapshotComplete` after the multi-tick prop drain. `OnEntitySpawn→ArmAdoption` is
`game_thread::Post`'d; at `SnapshotComplete` (inline) `g_pending` is briefly EMPTY but the queued
ArmAdoption tasks drain before the NEXT `Tick` observes the flag — so the sweep waits out the
just-armed adoptions and NEVER destroys a not-yet-adopted local kerfur. The sweep MUST stay in
`Tick`, never inline in `OnSnapshotComplete` (documented at both sites).

**Audit (5+4 angles, adversarially verified): 0 CRITICAL, 1 HIGH (fixed), rest LOW.** HIGH =
the **world-swap stale-mirror gap**: a save-transfer join does TWO level loads; a mirror bound in
world-1 survived the swap as a dangling Element (no EntityDestroy on a client world-swap), and
blocked world-2's re-adopt of the SAME eid (`OnEntitySpawn`+`ArmAdoption` early-return on
`Get(eid)!=nullptr`) — this also helped v74 "change nothing." FIX: `npc_mirror::Prune
DeadClientMirrors()` (release-only drop of dead-actor mirrors, guarded by `IsLiveByIndex`) called
from `OnClientWorldReady` BEFORE clearing pending; + `IsLiveByIndex` hardening in both tracked-set
builds. LOWs fixed: deleted dead `HasPending` (RULE 2), corrected the stale-v74 + premature-sweep
comments, `NameStartsWith` (alloc-free CDO check), GT-assert tripwires on the no-mutex entry points.
DEFERRED (flagged, separate commit): `npc_sync.cpp` 942 LOC > 800 soft cap (pre-existing, 872 at
HEAD) → extract `coop/npc_world_enum.{h,cpp}` (RegisterExistingWorldNpcs + the world-enum helpers).

**Hands-on to verify (user does):** host loads a save WITH an active KerfurOmega; client joins
(save-transfer). In `Game_0.9.0n*/…Win64/votv-coop.log` look for: `npc-mirror[world-swap]: pruned N
stale dead-actor NPC mirror(s)` (world-2 unblocked) then `npc-adopt: bound LOCAL save NPC … as host
mirror eid=N (class-match, NO duplicate spawn)` — ONE kerfur both peers see, host-driven, follows
host. Then: turn it off → NO floating camera; each player picks Follow → the kerfur follows THAT
player; idle/patrol/etc. mirror. If the double STILL appears, grep the client log for
`npc-adopt:` lines — absence of the `bound LOCAL` line + presence of `materialized mirror` (fresh
spawn) means the local twin wasn't found at poll time (report the surrounding lines).

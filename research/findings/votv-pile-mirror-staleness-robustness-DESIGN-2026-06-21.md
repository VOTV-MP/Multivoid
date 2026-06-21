# The pile client-mirror staleness + the host-authoritative proxy redesign (DESIGN + AS-BUILT phase 1) — 2026-06-21

**Tag: DESIGN of record (§2/§6/§7) + AS-BUILT phase 1 (see the dated section at the end).** The design was
authored at HEAD `fea04c26` (deployed `BA79E705`, proto v82). **Phase 1 of the proxy then SHIPPED at HEAD
`1011e512`** (commits `06685a9c` + `1011e512`) — **AS-BUILT, builds clean, NOT smoked** (the proxy is not yet
deployed; the user is mid-A+B hands-on). This is the track AFTER the deterministic re-pile thunk (commit
`d19ae4d4`) + the triple-grab-cue fix (`fea04c26`). Those fixed the HOST-side re-pile determinism + the
sound; they do **NOT** fix the dup the user hit — that is a CLIENT-side mirror-staleness problem with a
different root, documented here, and now addressed by the phase-1 proxy.

> **§2 (the proxy design), §6 (the four locked requirements), §7 (the phase split + C1/C2/C3 + Q1/Q2) remain
> the DESIGN OF RECORD.** §4 (the 3-verdict discriminator / health-poll / serial-check) is the FALLBACK that
> the proxy makes **moot — DROPPED, not built** (it would only have returned if the proxy were rejected; it
> was not). The dated **AS-BUILT (2026-06-21)** section at the bottom records what actually shipped.

---

## 1. The symptom + the root (from the 2026-06-21 client log)

**User report:** host grabs piles, the **original piles don't disappear** on the client (a visible dup);
and after carrying a clump away from the client + back, the **sync mirror broke entirely**. Plus the
"perfect sync" bar: a player should be able to **carry a pile a few km and it stays mirrored**.

**Root (client log, eid=4424 lifecycle):**
- `17:12:26` the host expressed pile 4424 -> the client `OnSpawn`'d a **real `actorChipPile_C` mirror**
  (`…3F92E700`) + `RegisterPropMirror eid=4424 bound to …3F92E700`.
- `17:12:36` (10 s later, standing still) the first grab convert: `ResolveLiveActorByEid(4424)` returned
  **null** -> `[PILE] CLIENT recv convert GRAB … mirror NOT-FOUND … spawned fresh; was desynced
  pre-convert`. The mirror had gone **not-live within 10 s on its own** (no `OnDestroy` -- the mirror
  chipPile's own `K2_DestroyActor` is `EX_VirtualFunction`, invisible to us). The fresh spawn + the
  lingering original (untracked) = the dup.
- After that one glitch, eid=4424 ran **30+ flawless re-skins** + a clean `OnDestroy`. So the registry +
  the convert mechanism are SOUND; the **only** failure is the mirror dying on its own.

**Why it dies:** the client mirror is a **real game BP actor** (`actorChipPile_C` / `prop_garbageClump_C`)
that runs its OWN ubergraph -> it self-morphs / self-destructs / is GC-eligible (unrooted, per
`remote_prop.cpp:313`) on its own schedule, independent of the host. A self-living mirror can never survive
a km carry.

**The dup is an ORPHAN symptom, not a registry/spawn bug** (the registry `REBOUND` eid=4424 correctly every
convert). Killing the staleness kills the dup.

---

## 2. The radical fork — host-authoritative PROXY mirror (RECOMMENDED), discriminator dropped

The earlier plan was a 3-verdict discriminator (self-morph / GC / stale-index) -> a targeted fix
(BP-suppress / `AddToRoot` / registry). The better RULE-1 move (user-driven 2026-06-21): change the
mirror's **rules of existence** instead of patching each death mode.

**Principle:** a point-hook (the thunk) wins when you must INTERCEPT existing behaviour + let the original
finish -- that is the HOST re-pile catch, and it is correct. But the CLIENT needs behaviour the stock actor
does NOT have: a **host-authoritative entity that does not self-morph, does not self-destruct, runs no BP,
survives a km-walk, and changes state ONLY on the host's command.** Forwarding into the stock actor's BP is
pointless there -- you want an actor the game does not ship. So: give the client its OWN minimal mirror.

### Code evidence the proxy is viable (SDK `CXXHeaderDump`, 2026-06-21)
- **Visual + collision are COMPONENTS, not BP-generated geometry:**
  `AactorChipPile_C { UStaticMeshComponent* StaticMesh @0x0228; UStaticMeshComponent* Collision @0x0230; }`
  `Aprop_garbageClump_C { UStaticMeshComponent* StaticMesh @0x0230; }`. The BP's `Init`/`UserConstructionScript`
  only ASSIGNS the mesh asset (per `chipType`); the component holds it -> separable from the graph.
- **The grab-trace is collision-based, not class-gated:** `mainPlayer.lookAtActor @0x0AA0` is the camera
  trace's hit; the grabbable check (`canPickup`/`playerTryToGrab`) is the chipPile's own BP, AFTER the
  trace. Increment 2 intercepts at the `InpActEvt` PRE (reads `lookAtActor`) BEFORE that -> a proxy with
  collision on the trace channel is seen; it never has to BE a chipPile.
- **The client needs nothing locally beyond render + grab-collision** -- host-authoritative owns physics,
  settling, interactions, state.

### The proxy design
- The client mirror of a trash entity becomes an **`AStaticMeshActor`** (engine class, NO BP) we own:
  - `StaticMeshComponent.SetStaticMesh(meshAsset)` -- the mesh resolved cross-peer from the `chipType` byte
    (already on the PropConvert wire): the client computes the mesh exactly as the game does, via a
    reflection-call to `lib_getFunc_C::getChipPileType(chipType)` (faithful, zero hardcoded paths). The
    convert (pile<->clump) just **re-skins** the proxy in place, no spawn-fresh -- but ASYMMETRICALLY (see
    Section 6 R1): ToPile sets the per-chipType pile mesh; ToClump sets the FIXED `dirtball` mesh + borrows
    `getChipPileType(chipType).GetMaterial(0)` (the clump is a MATERIAL swap, NOT a mesh swap -- verified in
    `prop_garbageClump_C::setTex`). Resolve via `StaticLoadObject` (not `FindObject`) so a not-yet-streamed
    asset still loads; fall back to `/Engine/BasicShapes/Cube.Cube` so the proxy is never invisible.
  - collision on the camera-trace channel (replicate the chipPile `Collision` profile) so Increment-2
    grab-trace hits it.
  - `AddToRoot` (or a held ref) -> no GC race. We own its eid->actor registry -> no stale-index. No BP ->
    no self-morph. **Staleness / GC / stale-index eliminated by construction -> the discriminator, the
    periodic health poll, and the serial-number check are all UNNECESSARY.**
  - driven kinematically by the existing pose stream; state changes only on host convert/pose/release.
- **Scope:** trash-only (chipPile/clump -- the self-morphing world litter). Aprop_C + kerfur mirrors are
  unchanged (they are not self-morphing in the same way).
- **Bounded edges to own at build:** the mesh-name cross-peer resolution; the exact camera-trace collision
  channel; the re-skin path in `OnConvert` (set mesh instead of spawn-fresh + echo-destroy).

---

## 3. Carry hardening (symptom 3: walk-away broke the mirror)

The 500 ms stream-stop timeout (`remote_prop.cpp:506`) is a heuristic that cannot tell a network gap from
a real host release -> on a km-walk with jitter it false-releases (physics on, the mirror falls/desyncs).

**Fix:** the host emits a **reliable** release on EVERY carry-end edge (throw / drop / swap / disconnect),
the receiver treats **no-stream = FREEZE** (hold the last pose, physics-off), and drops physics ONLY on the
explicit release. The 500 ms timeout degrades to a long safety-net (~30 s, for a crashed/disconnected peer),
not a per-frame cliff. (Must verify the host emits the release on ALL carry-ends, else freeze-forever
replaces fall-forever.)

---

## 4. The discriminator (DROPPED — the fallback the proxy made moot; NOT built)

> **STATUS 2026-06-21: DROPPED.** The proxy (Section 2) shipped as phase 1 (`06685a9c` + `1011e512`), so the
> 3-verdict discriminator / periodic health-poll / GUObjectArray-serial-check below were **never built** and
> are **not on any roadmap**. They were the fallback for "the proxy is rejected"; it was not. Kept here only
> as the analysis of the death modes the proxy eliminates by construction. Do NOT resurrect without a written
> reason the proxy failed.

If the proxy is NOT adopted (e.g., the visual turns out to need the BP for something beyond a static mesh),
diagnose the death mode FIRST (read-only, no fix, like the read-only thunk pass) before any BP-suppress/root:
- At `OnConvert` NOT-FOUND + a flagged (ini-gated, ~1 Hz, diagnostic-only) mirror-health poll: read --
  WITHOUT dereferencing the maybe-freed pointer -- the **GUObjectArray slot** at the cached internalIdx +
  the cached **SerialNumber** (`FUObjectItem + 0x10`; cache it at bind, compare BOTH idx AND serial -- idx
  reuse with a serial bump means the slot is a DIFFERENT object, NOT evidence about ours) + the
  PendingKill/Unreachable flags.
- For self-morph specifically, do NOT use a "fresh pile within radius R" heuristic (a stray ambient pile
  false-positives, and on a km-walk last-known position lags). Instead detect it **directly**: the mirror
  chipPile's own re-pile fires `BeginDeferred` on the CLIENT -> our process-wide thunk catches it with
  `worldCtx` = our tracked mirror eid = unambiguous self-morph evidence (the exact source + product). No
  proximity, no false positive. GC = no thunk fire + the slot freed/serial-bumped; stale-index = the slot
  still holds our object (idx+serial match) but `IsLiveByIndex` said dead.
- Strictly read-only: it roots nothing, suppresses nothing -- it must not alter the phenomenon it
  diagnoses. The fix follows the verdict in a separate build.

**But the proxy makes all of this moot** -- it is on the table only as the fallback.

---

## 5. Status + next

- Re-pile thunk + sound fix: AS-BUILT, deployed `BA79E705`, hands-on PENDING (the user confirms single cue
  + no vanish-return via `research/handson_runbook_2026-06-21_repile_thunk.md` take-22).
- Mirror-staleness dup + the km-walk carry: **phase 1 of the proxy AS-BUILT** (`06685a9c` + `1011e512`,
  HEAD `1011e512`) — builds clean, **NOT smoked, NOT deployed**. The host-authoritative `AStaticMeshActor`
  proxy (Section 2) shipped for **visual + position + re-skin, EXPLICIT NoCollision** (phase 1 per §7/C2).
  The discriminator (Section 4) is **DROPPED** (moot). See the dated AS-BUILT section at the bottom.
- STILL PENDING before the phase-1 smoke (audit `a249b005`): HIGH-1 (ToClump-beats-spawn renders as a pile),
  HIGH-2 (the clump's per-chipType MATERIAL swap), MEDIUM-1 (`StaticLoadObject` + Cube fallback), and the R4
  km-walk lerp + the explicit reliable carry-end release (Section 3). Collision + the look-trace probe + the
  client-grab direction are **PHASE 2** (Increment 2).

---

## 6. The four locked requirements (VERIFIED 2026-06-21 vs SDK + bytecode + our code; user green-lit the build)

Before the build the user fixed four edges as REQUIREMENTS. Each is now evidence-backed (three parallel RE
agents + a firsthand SDK read). Two of them REFINE the user's framing -- noted inline. HEAD `fea04c26`.

### R1 -- cross-peer mesh resolution + fallback  [static VERIFIED; clump-is-material REFINEMENT]
- **All trash meshes are 100% STATIC cooked `UStaticMesh` assets -- nothing procedural.** The pile mesh is
  chosen in `actorChipPile_C::init()` -> `StaticMesh.SetStaticMesh(lib_getFunc_C::getChipPileType(chipType))`,
  a pure `EX_SwitchValue` over 14 `chipType` cases returning hard `UStaticMesh` object refs
  (`research/bp_reflection/lib_getFunc.json`). All 14 pile meshes + `dirtball` confirmed present in the pak
  (`research/pak_re/paklist.txt`). The variant subclasses (`_erie`/`_wetConcrete`/`_leaves`) touch NO
  geometry (scale/material/state only). ⇒ **R1(a) YES** -- every variant restores cross-peer by name; no
  vertex data ever crosses the wire.
- **REFINEMENT -- the CLUMP is MATERIAL, not mesh.** `prop_garbageClump_C::setTex()` never calls
  `SetStaticMesh`; the clump keeps a FIXED `dirtball` mesh (`/Game/meshes/shovel/dirtball/dirtball`) and only
  `StaticMesh.SetMaterial(0, getChipPileType(chipType).GetMaterial(0))`. So the proxy convert is ASYMMETRIC:
  **ToPile** = `SetStaticMesh(pileMesh[chipType])`; **ToClump** = `SetStaticMesh(dirtball)` +
  `SetMaterial(0, pileMesh[chipType].GetMaterial(0))`.
- **Wire:** send the `chipType` byte (PropConvert already carries it) -- the client computes the mesh the SAME
  way the game does via `getChipPileType(chipType)` (reflection-call). The full chipType->mesh table is in the
  RE agent's report; resolve via `StaticLoadObject` (idempotent if resident), NOT `FindObject`.
- **R1(b) FALLBACK:** on resolve-null, `StaticLoadObject("/Engine/BasicShapes/Cube.Cube")` (confirmed in the
  pak) -> the proxy is never invisible/uncollidable.
- **Scale note:** `init` gives the pile a random per-instance scale (Z 1.0-1.1, XY 1.2-1.3) + Z-yaw. The proxy
  must take the HOST's ACTUAL transform (capture + send scale3D at spawn/convert), NOT re-roll the RNG.

### R2 -- collision channel + one-vs-two components  [trace channel VERIFIED; collision-geometry PROBE-GATED]
- **THE TRACE:** `mainPlayer.LookAtFunction` sets `lookAtActor @0x0AA0` from a `SphereTraceSingle` (radius 2)
  on **`TraceChannel = TraceTypeQuery1`** (param[4]==0; `research/bp_reflection/_mainplayer_uber_full.txt`),
  with object-type fallbacks, and sanity-checks `hit.GetCollisionObjectType() == 0` (ECC_WorldStatic). ⇒ the
  proxy must BLOCK that trace channel and be `ObjectType = WorldStatic`.
- **OFFSETS reconciled (firsthand SDK read):** `AactorChipPile_C` = `StaticMesh @0x0228` (visual,
  `NoCollision`), `Collision @0x0230` (the separate `garbageCollider` hull -- the component that ANSWERS the
  trace), `chipType @0x0238`. **The user's "Collision @0x0238" was the `chipType` field** -- `Collision` is
  `0x0230`. `Aprop_garbageClump_C` = `physicsImpact @0x0228`, single `StaticMesh @0x0230` (visual AND
  collider), `chipType @0x0238`.
- **VERDICT -- ONE component is sufficient for the CHANNEL.** The proxy is an `AStaticMeshActor` (one root
  `UStaticMeshComponent`). Set: `CollisionEnabled = QueryOnly` (host-driven kinematic), `ObjectType =
  WorldStatic`, profile **`BlockAll`** (blocks Visibility+Camera+all custom channels -> answers
  TraceTypeQuery1 regardless of the project's channel remap). The clump form's `dirtball` is known-collidable
  (it is the stock clump's own collider).
- **PROBE-GATE (the geometry subtlety the user raised):** the stock PILE splits visual/collider because the
  visible pile mesh is `NoCollision` and uses the `garbageCollider` hull; whether the visible pile mesh has
  SIMPLE collision a default (`bTraceComplex=false`) trace can hit is the ONE thing static RE cannot settle.
  So the FIRST proxy smoke is the look-trace probe: spawn a proxy in pile form, look at it, read `lookAtActor`
  (`engine::ReadMainPlayerLookAtActor`). If the worn pile mesh is not hit, the bounded fallback is to add the
  stock `garbageCollider` hull as a collision-only 2nd component (the exact stock setup) -- two components,
  faithful. **Lead single-component; escalate to two ONLY if the probe fails.** (This is also why collision is
  only strictly needed for the future client-grab Increment 2; the immediate dup/km-walk fix needs render +
  host-driven pose, so the proxy is built WITH collision but the probe gates it without blocking the dup fix.)

### R3 -- convert = atomic re-skin in place, registry untouched, no dup  [VERIFIED viable]
- Today `OnConvert` (`remote_prop.cpp:952`) SPAWN-FRESH-AND-DESTROYS; the dup is the `cur==null -> spawn
  fresh, old never destroyed` path (`:967 -> :996/:1024`). For the proxy: `OnConvert` becomes
  `SetStaticMesh(+material for clump)` on the SAME proxy actor; the eid->actor map (`MirrorManager<Prop>`) is
  UNTOUCHED (pointer unchanged; the in-place `existing->SetActor` rebind already exists).
- **Atomicity CONFIRMED:** `OnConvert` runs under `UE_ASSERT_GAME_THREAD`; UFunction calls are synchronous
  `ProcessEvent` -> back-to-back `SetStaticMesh` then the collision/material set in the one body = no tick
  boundary, no window where visual=clump but collision=pile (`SetStaticMesh` recomputes the component bound
  from the new mesh, so the collision bound follows the visual in the same call).
- **No dup BY CONSTRUCTION:** an `AddToRoot`'d proxy never goes stale -> `ResolveLiveActorByEid` never returns
  null -> the fresh-spawn dup path is dead.

### R4 -- client visual interpolation between host poses, no physics  [VERIFIED feasible]
- Today props TELEPORT (`remote_prop.cpp:482-483`, raw `SetActorLocation/Rotation`); NO prop interp buffer.
  The `LerpWindow` (`include/coop/lerp_window.h`) used by `RemotePlayer`/`Npc` is the ready-made shape.
- **Plan:** on a new pose, cache target+error and `LerpWindow::Open`; add a per-tick advance branch (the
  every-tick `remote_prop::Tick` call already exists, `net_pump.cpp:951`) that writes `cur += error*dAlpha`
  until the window closes. Physics stays off (already true for driven props). ⇒ the proxy interpolates
  visually; the km-walk is smooth on the client. Pairs with the carry-end reliable release (Section 3):
  no-stream = FREEZE at the last pose; drop only on the explicit release.

**Net:** all four LOCKED; two refinements vs the user's framing (clump = material not mesh; pile-mesh
collision-geometry is probe-gated). The proxy is the root-cause fix; the discriminator/poll/serial-check
(Section 4) stay DROPPED as moot.

---

## 7. Phase split + the 3 lifecycle/collision clarifications (LOCKED 2026-06-21)

The user split the build to keep collision uncertainty OUT of the dup/km-walk fix. Three clarifications:

### C1 -- does the proxy-clump answer the look-trace? (deferred to phase 2)
The clump is the carried/flying form; you grab a PILE, never a clump. In phase 2 (client-grab) the proxy
answers the trace in BOTH forms via the WORN mesh's bound (dirtball for clump, pileMesh for pile) -- NO
per-form collision toggle. A client targeting a clump and trying to grab is rejected by the HOST's own grab
validation (the clump is held/flying, not a grabbable pile) -- host-authority arbitrates, so a special-case
"disable collision on ToClump" would be a crutch (RULE 1). The re-skin stays a pure mesh/material swap; the
collision bound follows the mesh automatically + atomically (`SetStaticMesh` recomputes it).

### C2 -- collision is PHASE 2; phase 1 carries ZERO collision uncertainty
The look-trace probe validates trace-answering, but collision is only needed for client-grab (Increment 2):
- **PHASE 1 (now): visual + position + re-skin, NoCollision.** Fixes the dup + km-walk. The proxy is a
  kinematic visual follower; the client player walks THROUGH mirrored trash (a temporary, accepted phase-1
  limitation). No collision config, no probe -> no collision uncertainty in the dup fix.
- **PHASE 2 (with Increment 2): collision geometry.** Set BlockAll/WorldStatic/QueryOnly, run the look-trace
  probe, escalate to the `garbageCollider` hull (a 2nd collision-only component) if the worn pile mesh
  isn't hit.

### C3 -- proxy lifecycle (AddToRoot MUST pair with RemoveFromRoot on every teardown)
- **Who tears down:** the client's `remote_prop::OnDestroy` handler (on a PropDestroy for the eid) ->
  RemoveFromRoot + K2_DestroyActor + unregister; AND the disconnect drain (`ForceRelease` /
  `OnDisconnectForSlot` -> `DrainWirePropMirrors`, `remote_prop.cpp:1078/1085`) sweeps the whole mirror
  registry -> RemoveFromRoot each proxy. ONE teardown helper does BOTH (un-root THEN destroy), called from
  both paths -- else a destroyed-but-rooted proxy leaks (exactly why `RemoveFromRoot` must be built; the
  agent flagged it missing).
- **host-destroy IS reliable.** `PropDestroy` is a `ReliableKind` (`event_feed.cpp:156`,
  `event_dispatch_entity.cpp:244`) on the ARQ channel (retransmit-until-acked) -> it CANNOT be lost like an
  unreliable pose packet. So "leak-forever if destroy lost" cannot happen via packet loss.
- **NO far-distance culling.** The proxy mirrors host membership 1:1; retirement is host-driven (the host
  emits PropDestroy when the entity leaves its expressed set / dies), never local distance culling -- local
  culling would re-introduce the stale-mirror class the proxy exists to kill.
- **Backstop:** even with reliable destroy, the existing membership-bounded reconcile (host re-seed/retire,
  `da233ecb`) is a secondary safety for a host-side silent retire; the proxy is a Prop mirror in the same
  `MirrorManager<Prop>` the reconcile sweeps, so it participates.

### Phase-1 scope (building now)
ue_wrap foundation (`SetStaticMesh`, host-side mesh/material reader, `RemoveFromRoot`, exported root/mesh
getter) -> trash proxy module (spawn `AStaticMeshActor` + `AddToRoot` + chipType mesh/material + Cube
fallback + host scale3D, **NoCollision**) -> wire into `OnSpawn` (trash join-spawn) + `OnConvert` (atomic
re-skin in place, registry untouched -> kills the dup) -> R4 lerp + reliable carry-end release -> lifecycle
teardown (RemoveFromRoot on OnDestroy + disconnect-sweep). Collision + probe + the client-grab direction are
PHASE 2.

### A+B hands-on (BA79E705, 2026-06-21 18:06 logs) -- GREEN
Every convert `[SYNC-MIRROR OK]` + `mirror FOUND` both directions across ~10 cycles (eids 4424/4425/5261);
`HOST RE-PILE(thunk) ... convert IN PLACE` on every re-pile (no vanish-return); `CLIENT HOLD carry pose`
shows the ctx-gate holding the ahead-of-convert pose (the triple-cue fix); zero `mirror NOT-FOUND`, zero
errors. The audible single-cue is INFERRED from the HOLD lines (user observational confirm pending). The dup
did NOT recur, but the run didn't clearly exercise walk-away -> the robustness track (the proxy) stands.

### Q1 -- phase-1 NoCollision IS a temporary regression (verified)
Stock client trash DOES block the player. Verified from the SCS collision template
(`docs/piles/re-artifacts/chipPile.json`): the `garbageCollider` hull is `ObjectType = ECC_WorldStatic`,
`CollisionEnabled = QueryAndPhysics` (archetype default; not serialized), and its serialized
`ResponseArray` deltas are WorldStatic/Destructible/sound/water/trigger -> `ECR_Ignore`, WorldDynamic ->
`ECR_Overlap`. **`Pawn` is NOT in the delta list -> it keeps the default `ECR_Block`** (same for
Visibility/Camera). So the player capsule (ECC_Pawn) blocks against the hull -> currently you cannot walk
through a client-mirrored pile. Phase-1 NoCollision removes that -> the client walks THROUGH mirrored trash.
Magnitude is small (the hull is short ground litter the capsule steps over via movement step-up; the
perceptible case is larger / flying clumps) and strictly temporary. **Phase 2 restores it via the
`garbageCollider` hull, which is DOUBLE-DUTY: the Pawn-block (player body) AND the Visibility/Camera
grab-trace.** So ALL collision lives in phase 2 via the one hull -- a coherent split, not two separate jobs.
Phase 1 sets the proxy EXPLICITLY NoCollision (predictable; a kinematic host-driven proxy needs no local
collision -- the host owns physics).

### Q2 -- single un-root chokepoint (verified); the reconcile does NOT even touch mirrors
The membership-reconcile sweep (`RunDivergenceSweep_`, `remote_prop_spawn.cpp:1024`) **EXCLUDES mirrors**:
`if (pr.mirror) continue;` -- it dooms only the client's OWN local (non-mirror, save-loaded) Prop Elements,
never a host-driven mirror (MTA CElementGroup membership: it adjudicates only what THIS client loaded). The
proxy is a mirror (`RegisterPropMirror`, `m_mirror=true`) -> it is **never swept**. So the "backstop path
leaks" case cannot arise -- the reconcile isn't a proxy retire path at all. The ONLY proxy retire paths:
(a) host `PropDestroy` -> `remote_prop::OnDestroy` (reliable ARQ), (b) the disconnect drain
(`DrainWirePropMirrors` / `OnDisconnectForSlot`). BOTH remove the Prop from `MirrorManager<Prop>` ->
`unique_ptr` dtor -> `~Prop` -> `~Element` -> `Registry::UnregisterMirror` (`element.cpp:53`). **The single
chokepoint is the Prop Element dtor.** Design (belt-and-suspenders):
- a `RetireProxy(eid)` helper does `RemoveFromRoot -> K2_DestroyActor -> unbind` at BOTH call sites; AND
- `RemoveFromRoot` is ALSO anchored in the owned-proxy Element teardown -- a cheap, dtor-SAFE flag-clear on
  the GUObjectArray slot (unlike `K2_DestroyActor`, which stays at the call site for game-thread / mutex
  safety). So ANY future path that removes the Element from `MirrorManager<Prop>` un-roots automatically ->
  a destroyed-but-rooted leak is **structurally impossible**, even if a new retire path forgets `RetireProxy`.

### Mesh resolution simplification (from R1) -- no host-side mesh reader needed
Because the client computes the mesh from the `chipType` byte via `getChipPileType(chipType)` (R1), the host
does NOT read+send a live mesh name. The phase-1 ue_wrap foundation is therefore: `SetStaticMesh`,
`SetComponentMaterial` (clump material), `GetMeshMaterial` (read slot-0 off the resolved pile mesh),
`getChipPileType` caller, `RemoveFromRoot`, and an exported static-mesh-component/root getter. Plus
`SpawnActor`(engine class) + `AddToRoot`, which already exist.

---

## AS-BUILT — phase 1 of the proxy (2026-06-22, code HEAD `245148c6` + instrumentation `b7e76db3`/`70f1f04b`) — DUP-FIX + VISIBILITY hands-on VERIFIED; CARRY mechanism SMOKE-PROVEN on a settled join (visual hands-on pending)

> **The DUP FIX works (hands-on): no doubled piles, resting + landed piles mirror correctly + VISIBLY.**
> The host-authoritative `AStaticMeshActor` proxy (§2/§7) replaces the self-morphing BP mirror; the staleness
> dup is structurally gone (`0` `mirror NOT-FOUND` in the smoke + the user saw it works). **The LIVE CLUMP
> CARRY MIRRORS on a settled join** — proven 2026-06-22 by two clean instrumented smokes: the ToClump convert
> adopts (`known` 0->1), the clump mesh resolves (`dirtball`), GRAB-IN fires, the drive advances `#1..#540
> [proxy]` tracking the host's path, and the LAND re-skins back to a pile (see the dated CARRY-MIRROR section
> below). The earlier "stays a PILE / 2 fps / 0 GRAB-IN" was the **JOIN RACE** (the autotest grabbed before
> the client expressed its proxy snapshot), NOT a sync bug — killed by the autotest's new puppet-live settle
> gate. STILL PENDING: a hands-on to confirm the on-screen VISUAL (the smoke is render-blind) + to characterize
> the user's earlier "2 fps" (most likely the same race). §2/§6/§7 above stay the design of record.
> Commits: `06685a9c` (core) + `1011e512` (leak) + `3d371349` (HIGH-1/2 + MEDIUM-1) + `095dbf44`
> (lerp/freeze/teardown) + `8a17faeb` (HOT-1) + `245148c6` (the VISIBILITY/Movable fix); harness `4a1f42a6`
> + `f1177589` + `cfdd7745`.

### What shipped (commits `06685a9c` core + `1011e512` hotfix)

- **NEW `coop/trash_proxy.{cpp,h}`** — owns the eid→proxy registry (`g_proxies`) + the rooting. Exports
  `SpawnProxy` / `ReskinProxy` / `RetireProxy` / `IsTrashProxyClass` / `IsClumpClass` / `IsProxy` /
  `OnDisconnect` / `OnDisconnectForSlot`. The proxy is an `AStaticMeshActor` (NO blueprint), `AddToRoot`'d,
  EXPLICIT NoCollision (`SetActorRootCollisionEnabled(actor, 0)` — `trash_proxy.cpp:120`); `SpawnProxy` is
  idempotent (a same-eid re-spawn re-skins the existing actor instead of leaking a second — `:107-112`).
  `RetireProxy` = `ClearAnyDriveFor → DestroyActor → RemoveFromRoot → unbind the Prop mirror` in that order
  (the GC-window order from §C3/Q2 — `:140-163`).
- **ue_wrap foundation (additive):** `reflection::RemoveFromRoot` (pairs `AddToRoot`), `engine::SetStaticMesh`
  + `engine::GetStaticMeshComponent` (on `UStaticMeshComponent`), `prop::ResolvePileMesh` (the game's OWN
  `getChipPileType(chipType)` resolver per R1, last-good-cached so a proxy is never invisible).
- **Wiring (the dup fix):**
  - `remote_prop_spawn.cpp` `OnSpawn` — a trash class (`IsTrashProxyClass`) → `SpawnProxy` +
    `RegisterPropMirror`, branched BEFORE the BP dedup/converge/physics machinery (`:343-364`).
  - `remote_prop.cpp` `OnConvert` — `IsProxy(E)` → `ReskinProxy` IN PLACE (`SetStaticMesh` on the SAME actor,
    binding untouched) → return. **THIS is the dup fix** (no spawn-fresh, no orphan; a rooted proxy never goes
    stale → the "mirror NOT-FOUND → spawn fresh" path that caused the dup is structurally unreachable).
  - `remote_prop.cpp` `OnDestroy` — `IsProxy(E)` → `RetireProxy` → return (un-roots the rooted actor; else it
    leaks its GUObjectArray slot forever).
  - `subsystems.cpp` `DisconnectSlot` — `trash_proxy::OnDisconnectForSlot(slot)` BEFORE the generic per-slot
    mirror drain; `DisconnectAll` — `trash_proxy::OnDisconnect()` BEFORE `ForceRelease`.

### Audit `a249b005` (post-ship) — CRITICAL fixed; HIGH/MEDIUM-1 pending

- **CRITICAL-1 — FIXED (`1011e512`):** a PER-SLOT disconnect drained a proxy's Prop Element via
  `DrainMirrorsForSlot` WITHOUT `RetireProxy` → the rooted `AStaticMeshActor` leaked. Fixed by making
  `g_proxies` the authoritative per-slot retire driver: `ProxyEntry.ownerSlot` (stamped from `senderSlot` at
  spawn) + `OnDisconnectForSlot(slot)` retiring every proxy owned by that slot, called before the generic
  drain. (The design's Q2 claimed this was "structurally impossible" — it was the gap.)
- **MEDIUM-2 — FIXED:** cache the proxy's `UStaticMeshComponent` in `ProxyEntry` (resolved once at spawn) →
  `ReskinProxy` no longer walks ~237k GUObjectArray entries per convert.
- **MEDIUM-3 — FIXED:** fold `IsTrashProxyClass` + `IsClumpClass` into one cached `ClassKind` lookup (one
  `FindClass` per distinct class, not two).
- **The pre-smoke fixes — ALL BUILT (`3d371349`):**
  - **HIGH-1 — FIXED.** Two root fixes: (a) `OnConvert` now spawns the proxy directly in the unambiguous
    `wantClump` form when a trash convert beats its `OnSpawn` (no proxy for E yet), instead of routing through
    `OnSpawn` whose form is class-derived; (b) `SpawnProxy`'s re-spawn convergence branch **no longer re-skins**
    — the form (pile↔clump) is owned exclusively by the ctx-ordered convert channel, so a trailing/stale
    `PropSpawn(chipPile, E)` can never flip a correctly-converted CLUMP back to a PILE. `ClassNameToWString`
    promoted to `remote_prop_spawn` public API (de-duplicated, RULE 2).
  - **HIGH-2 — FIXED + bytecode-VERIFIED.** Pulled the actual `prop_garbageClump_C::setTex` kismet
    (`docs/piles/re-artifacts/bp_reflection/prop_garbageClump.json` export 64): it is
    `this.StaticMesh.SetMaterial(0, getChipPileType(chipType).GetMaterial(0))` on the FIXED dirtball mesh —
    a **MATERIAL swap, NOT a mesh swap** (the prop.cpp comment claiming "picks the clump mesh" was wrong; corrected).
    New `engine::SetComponentMaterial` (PrimitiveComponent::SetMaterial, null reverts to default) +
    `engine::GetStaticMeshMaterial` (UStaticMesh::GetMaterial, param `MaterialIndex` — SDK-verified
    `Engine.hpp:19127`). `SkinProxy` clump = dirtball + `SetMaterial(0, pileMesh.GetMaterial(0))`; pile = pile
    mesh + `SetMaterial(0, null)` (clear any stale clump override on the shared component).
  - **MEDIUM-1 — FIXED.** Cube last-ditch fallback (dirtball → pile mesh → `/Engine/BasicShapes/Cube`). A full
    native `StaticLoadObject` was deemed unwarranted (a loaded trash class pins its meshes resident +
    `ResolvePileMesh` last-good-caches, so the chain never bottoms out in practice) — documented, not a crutch.
- **The R4 km-walk robustness — ALL BUILT (`095dbf44` + audit HOT-1 `8a17faeb`):**
  - **MTA-style interpolation (proxy-scoped):** `ActiveDrive` lerps position + rotation from the current
    rendered transform to each new pose over the measured interval, advanced EVERY tick (`BeginLerpToPose` +
    `AdvanceLerp`); first pose / far jump (>5 m) / non-proxy snap; shortest-arc angle lerp. A non-proxy Aprop_C
    held item keeps byte-identical teleport behavior (interpolation must not regress proven sync).
  - **Freeze-not-timeout:** the 500 ms stream-stop physics-release is gated to `!isProxy` — a host-authoritative
    proxy FREEZES on a network gap (the km-walk fix) and releases ONLY on an explicit reliable edge
    (`OnRelease` throw / `OnConvert` ToPile / disconnect). Proxy throw = freeze (no local physics) + the ToPile
    convert repositions to the landed pile. Invariant hardened: a proxy is destroyed ONLY via `RetireProxy`
    (ForceRelease + OnDisconnectForSlot are proxy-aware — never `ConsumeLocalActor` = no rooted-slot leak).
  - **HOT-1 dirty-gate (`8a17faeb`):** `AdvanceLerp` skips the SetActorLocation/Rotation write when the per-tick
    interp step is sub-epsilon (0.25 cm / 0.1°) — a held-but-still clump costs ZERO engine writes; active
    motion writes every tick (mirrors + exceeds `atv_sync::ApplyMirror`).
- **Hot-path audit `aa8e7d9a` (post-build) — GO, no CRITICAL/HIGH.** Independently traced the
  destroy-only-via-RetireProxy invariant (holds by construction — the sweep + reaper both `if (pr.mirror)
  continue`), the re-entrancy (fixed-size `g_drives` array — safe), the lerp math, the non-proxy exemption,
  and all four new UFunction signatures (SDK-exact). Only finding = HOT-1 (MEDIUM, folded above). Endorsed the
  post-smoke extraction of the drive subsystem → `remote_prop_drive.{cpp,h}` (`remote_prop.cpp` is 1325 LOC).
- **SMOKE — PARTIAL; "functionally green" was MISLEADING (the smoke is RENDER-BLIND + the autotest grabbed
  DURING the client join). SHA `f2344bab`, 2026-06-21.** What the smoke DID prove (matching real log): the
  dup is GONE — **`0` `mirror NOT-FOUND` / `spawn fresh`**, 875 proxies spawn (`AStaticMeshActor`, rooted),
  `0` proxy/drive errors, no crash/SEH/OOM, 300 s stable. The lan-test exit-1 was a harness PASS-gate bug
  (net-stats `puppet=` is host-centric — `net_pump::Puppet(1)` slot 1, but a client's host-puppet is slot 0),
  fixed `f1177589`+`cfdd7745`. **What the smoke did NOT prove + the hands-on then DISPROVED:**
  - **The proxies were INVISIBLE in the smoke.** `f2344bab` spawned them STATIC-mobility, on which
    `SetStaticMesh` no-ops at runtime; the smoke can't see render (log markers + black screenshots) so it
    passed anyway. FIXED: `SetComponentMobility(Movable)` (`245148c6`, build `69405445`); user hands-on then
    confirmed the resting + landed piles mirror VISIBLY. [[lesson-runtime-staticmeshactor-must-be-movable]]
  - **The live clump CARRY never mirrored.** Client log: **`0` `GRAB-IN`** (the pose drive NEVER established),
    carry poses ctx-HELD, and (verdict run) the ToClump convert never applied. The autotest grabs at host+40 s
    but the client's 875-proxy join finished LATER → the grab RACED the join, so the smoke never cleanly
    tested a post-join carry. The `[SYNC-MIRROR OK]` line was the LAND (spawn-on-convert PILE), not a driven
    carry. So **"the grab→carry→throw→re-pile cycle worked end-to-end" is WITHDRAWN** — only the dup-gone +
    the final landing are evidenced.
  **HANDS-ON 2026-06-21 (`69405445`):** resting + landed piles mirror correctly + VISIBLY [V]. The user also
  reported the host grabbing + carrying a clump did NOT mirror ("old pile not removed" + "2 fps animation").
  **RESOLVED 2026-06-22:** that was the JOIN RACE, not a sync bug — a clean instrumented smoke (settled join
  via the new puppet-live gate) proves the carry MIRRORS end-to-end (convert adopts, mesh resolves, GRAB-IN,
  drive `#1..#540 [proxy]` follows, LAND re-skins; see the dated CARRY-MIRROR section). The user's earlier
  symptom is most likely the same race (grab before the proxy snapshot expressed); a hands-on on a FULLY
  settled join is still needed for the on-screen VISUAL (the smoke is render-blind). Runbook take-24.

### Scope held

Phase 1 = **trash only** (chipPile/clump + variants), **visual + position + re-skin, NoCollision** (the
client temporarily passes through mirrored trash — the accepted phase-1 regression, Q1). Collision (the
`garbageCollider` double-duty hull), the look-trace probe, and the client-grab direction are **PHASE 2 /
Increment 2**. `Aprop_C` + kerfur mirrors are unchanged.

---

## CARRY-MIRROR — RESOLVED at the mechanism level (2026-06-22): the carry MIRRORS on a settled join; the earlier "does not mirror" was the JOIN RACE (build `b7e76db3`)

The phase-1 north star -- "a host grabs a pile, walks, and the client sees a sync mirror of the carried
clump" -- **WORKS on a settled join**, proven by a clean instrumented smoke. The earlier "stays a pile /
2 fps" was NOT a bug in the carry sync path; it was the **autotest grabbing before the client had expressed
its join snapshot** (its proxy mirrors). Once the grab lands on a settled world, every link fires in order.

### What the clean smoke proves (runs `b97z33gyh`/`b7e76db3` then the fuller `b7oxr23uy`/`70f1f04b`, CLIENT log, settled join via the new puppet-live gate)
Two independent clean smokes. The fuller one (`b7oxr23uy`, a 1.5 m/s 8 s carry, grab held 74/80 steps) drove
`drive #1,#2,#3,#60,#120,#180,#240,#300,#360,#420,#480,#540 [proxy]` -- ~540 poses across the WHOLE carry,
targets marching `(-2222,391)->(-1426,-380)` in lock-step with the host. The eid=6265 timeline, in order:
- `SPAWN eid=6265 pile` at join -- settled, well before the grab (the puppet-live gate + 35 s margin).
- `CLIENT HOLD carry pose ctx=1 known=0` -- the FIRST carry pose beat the reliable convert (the unreliable
  lane wins by a frame); held exactly as designed (a single transient HOLD).
- `recv convert GRAB(pile->clump) eid=6264 ctx=1 known=0 isProxy=1 -- RECEPTION` -- the ToClump convert
  ARRIVED and addressed a proxy.
- `SkinProxy CLUMP mesh-src=dirtball mesh=0x... mat=0x...` -- the clump mesh RESOLVED (NOT the PILE-FALLBACK:
  the `dirtball` StaticMesh exists + resolves on the client; the "clump looks like a pile" worry is refuted).
- `recv convert GRAB ... PROXY re-skinned IN PLACE to CLUMP` -- the proxy became a clump.
- `remote_prop: slot 0 GRAB-IN eid=6264` -- the carry pose-drive established.
- `drive #1,#2,#3,#60,#120,#180,#240,#300 -> target(...) [proxy]` -- the drive ADVANCED through the whole
  carry; its targets match the host's `PropPose emit ... ctx=1` 1:1 (the client followed the host's path).
- `recv convert LAND(clump->pile) ctx=3 known=1 -> re-skinned to PILE` -- the land re-skin.

So the convert adopts (`known` 0->1), the clump mesh resolves, GRAB-IN fires, the drive follows, the land
re-skins. **The carry MIRRORS.** (This is MECHANISM evidence from the log -- the drive computes targets that
track the host. The on-screen VISUAL is still render-blind in the smoke; see "hands-on PENDING" below.)

### Why the earlier smoke showed "0 GRAB-IN / 0 ToClump applied" -- the JOIN RACE (NOT a sync bug)
The autotest grabbed at a fixed host+40 s, but the 875-proxy join snapshot expressed ~13 s LATER (smoke
`f2344bab`: proxies SPAWN at 22:16:15 vs the host grab at 22:16:02). The client joined AFTER the entire
grab->carry->throw->land was over -- it correctly showed the final PILE; there was simply no live carry to
mirror at the moment it had a world. "0 GRAB-IN" meant "nothing live to drive," not "the drive is broken."
Killed by the autotest's new **puppet-live settle gate** (poll the client puppet live + a 35 s snapshot
margin) so the grab lands on a settled world.

### The user's "2 fps / stays a pile" (hands-on, `69405445`) -- most likely the same race; hands-on PENDING
The clean smoke could NOT reproduce the user's symptom on a settled join. The most likely cause is the user
grabbed before the client's join fully settled (the proxy-expression window) -- the same race. A fresh
hands-on on a FULLY settled join (loading cleared + the world populated, then grab + walk) is needed to
(a) confirm the on-screen VISUAL (the smoke is render-blind -- it proves the drive computes the right
targets, not that the pixels follow) and (b) characterize the earlier "2 fps" if it persists. Runbook take-24.

### Carried forward / still true
- The lerp/freeze/HOT-1 (`095dbf44`/`8a17faeb`) now ARE exercised (the drive establishes -> AdvanceLerp runs
  + the freeze-not-timeout proxy exemption applies). Smoke-exercised; the VISUAL smoothness is hands-on-pending.
- The ctx-gate mechanism (trash_channel.cpp:126-158) is CORRECT, not the bug: `known` 0->1 on the adopted
  ToClump, ctx=1 carry poses then apply. The single transient HOLD (1st pose beat the convert) is by design.
- Known phase-1 robustness EDGE (NOT the common "settle then interact" case): a grab DURING a client's
  active join (proxies mid-expression) is not guaranteed to mirror -- the convert may arrive before that
  pile's proxy exists. Phase-1 expectation is to let the join settle; mid-join robustness is a follow-up.

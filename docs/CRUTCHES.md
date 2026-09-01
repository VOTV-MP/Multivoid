# The CRUTCH REGISTER — what we built the wrong way, and what the right way is

*[↑ docs index](README.md)*

**Created 2026-08-29 on USER DIRECTIVE:** *"THE CURRENT ATV IMPLEMENTATION IS MADE OF CRUTCHES -
WHICH IS ANTI-RULE 1. Needs proper design and fix. Also make a doc of current crutches - put atv
there and also put piles there - because interaction with piles is as crutchy as atv today -
creating a pile object attachmed etc - full of crutches - that's on the list after atv."*

This is the standing register of **subsystems we shipped in a crutch shape**. It exists because
RULE 1 is easy to honour on a new feature and easy to lose across a year of increments: each
individual commit looked like a targeted fix, and the accumulation is a crutch. A register makes
the accumulation visible, which no single diff can.

**Evidence tags.** **[V]** measured from the current tree / bytecode (cited) · **[RD]** derived from
measured facts, not observed at runtime · **[A]** asserted by an earlier doc, not re-verified ·
**[?]** unmeasured.

---

## 0. What counts as a crutch here

From `CLAUDE.md` RULE 1: *"When you find yourself adding a workaround (filter, skip-if, suppress-X,
catch-and-ignore), STOP. That is a crutch. Identify the root cause and fix it at the point where the
actual problem is."*

This register uses a slightly wider test, because the entries below are not `skip-if` lines —
they are **architectures** that were shaped by an obstacle instead of by the problem:

1. **It neutralises or replaces the engine entity instead of driving it.** Principle 3 says our class
   owns network state and the **engine** class keeps rendering / animation / physics, joined by a
   pointer. A design that freezes the engine entity into a corpse, or swaps it for a fake, has
   abandoned that split.
2. **It spawned a second mechanism to compensate for the first.** A crutch that needs a crutch is the
   diagnostic signature — the compensating mechanism is load-bearing evidence, not a detail.
3. **Its stated reason has since been measured false, and the code stayed.** RULE 2 says the old
   thing goes when its reason retires.
4. **Two implementations of one concept compile together.** Also RULE 2.

A thing is **not** a crutch merely because it is incomplete, deferred, or unverified. Missing work is
missing work; this register is for work that was *built in the wrong shape*.

---

## 1. Register

| # | Subsystem | Shape of the crutch | Status | Order |
|---|---|---|---|---|
| C1 | **ATV** (`coop/interactables/atv_sync`, `ue_wrap/devices/atv`) | The mirror is a frozen corpse; the rig's whole purpose is deleted to make it hold still | **ARC 1 COMMIT 1 SHIPPED 2026-08-29** (`070c7d29` + `a2a45fc7`, proto 146): the freeze/teleport lane is DELETED — the mirror simulates and is corrected, and `AtvRelease`'s launch velocity went with it. The crutch is RETIRED at its root; what remains of C1's gap list is the VITALS and CONFIG arcs, not the corpse. **UPDATED 2026-08-30 (`8cd0ac25`): the 25-40 cm sag was a SECOND crutch in the same lane, and it was the collision guard, not the pose lane.** `coop::atv_hit_guard` cancelled ALL SEVEN `ComponentHit` delegates on a non-owner to stop a mirror authoring damage; the five WHEEL delegates also keep the rig's SHAPE, so the mirror's body sat under its own wheel plane. A four-cell single-variable experiment ACQUITTED the pose corrector outright (with the guard off it is the best of the four cells). Shipped `8cd0ac25`: only the two BODY delegates are cancelled. **THEN `28a958e8` RETIRED THE SUPPRESSION SHAPE ITSELF -- the crutch's last piece is gone.** Nothing is cancelled: all seven delegates are NEUTERED (a zero `FVector` over `NormalImpulse` pre-dispatch; the handler dispatches whole and only the magnitude dies), so the mirror keeps every side effect it needs -- above all `wheelsOnSurface`, whose loss WAS the sag -- and authors no damage at all. `g_cancelMask` and the `atv_hit_guard_mask` row are deleted (RULE 2). `[V]` verified on the same arm: mirror wear 98.22/100/100/98.48 -> **100/100/100/100**, `UNRESOLVED=0`. `[V]` two driven verification runs, A1 x0.95/x1.00/x1.23 and x0.90/x0.59/x2.11 (was x2.3-x2.7), A2 9.6 and 3.8 cm PASS, A6 both PASS, rig-shape spread 0.44-2.43 cm; a third undriven run reads A2 1.83 / spread 0.00. **A post-ship audit corrected the framing: "A2 never passed driven" was FALSE** (`20260830-092139`, driven, all seven cancelled, is the only ACCEPTANCE: PASS on disk at A2 7.0 cm), and the claimed 2x2 is honestly a 1x2 pair on one binary (`-1059` vs `-1057`, A2 30.4 -> 5.3, shape 19.05 -> 0.12) -- which is what carries the conclusion. **The §16 causes above are SUPERSEDED -- read `docs/vehicles/ATV.md` §17.** Residual: a mirror now runs `processTire()` and can eject a tire its author still has -- the fix is tire durability on the wire, not re-suppression. Still open: A5 and A4 (1 s ownership overlap). A5's 209-324 cm peak is NOT noise: `[V]` the author's last 12 driven samples sit still at 0.7-5 cm/s with `driven=1` still true -- it is wedged against a fence -- so A5's window mixes real driving with a stationary author, and 200 cm measured there is a STATIC error the corrector never closed, which A2 cannot see because it reads only the settled tail | **IN PROGRESS** |
| C2 | **Trash piles / clumps** (`coop/props/trash_proxy`, `native_pile_mirror`, + ~9 sibling modules) | The mirror is a FAKE actor, which broke aim, which grew a parallel aim system; two mirror implementations now coexist | **OPEN** | **SECOND** (user: *"that's on the list after atv"*) |
| C3 | **KO respawn** (`coop/player/ko_respawn`, the `Holder::KoRespawn` hold in `coop/player/ragdoll_gate`) | The game's own death mechanism was NEUTRALISED -- `canRagdoll` held shut for the whole session so `ragdollMode` early-outs -- instead of the death being allowed to run and then answered | **CLOSED BY RETIREMENT, `33008d87` 2026-08-31.** The lane is deleted whole (RULE 2) rather than fixed, because the arc that replaces it inverts its mechanism; H1/H2/H3 died with it and H3's underlying write was converted to `CachedObjRef` on the way out. The replacement IS BUILT AND GREEN as of 2026-08-31 (`aaf23a4d`): `ue_wrap/engine/level_travel` detours `UGameplayStatics::OpenLevel` and `coop/player/death_revive` writes the revive in its place, so the whole native death now runs and the world is KEPT. `mp.py death --session` 12/12; `mp.py death` 6/6 with `installed=1 travelsRefused=0`, i.e. single player untouched by NEGATIVE CONTROL rather than by assumption | **CLOSED** |

---

## C1 — the ATV

**RE of record:** `docs/vehicles/ATV.md`. **Design in progress:**
`research/findings/vehicles/votv-ATV-full-sync-DESIGN-2026-08-29.md`.

### What ships today — REWRITTEN 2026-08-30 (the paragraph below this block described the
### freeze model that arc 1 DELETED; it is kept further down as the dated indictment)
Since arc 1 (`070c7d29`, 2026-08-29) a mirrored ATV **SIMULATES natively** — physics ON, tick ON,
no PrepareMirror/freeze anywhere — and is *corrected* toward the authority (`atv_corrector`), with
collision damage authorship denied by the **impulse NEUTER** (`28a958e8`: a non-owner's seven
`ComponentHit` delegates dispatch with a ZEROED `NormalImpulse` — the notification runs whole, only
the damage magnitude dies). Since v147 (2026-08-30) the author's **CONDITION travels**: tire
durability/dirt/fixes/types, the spare trio, body dirt, fuel and health ride `AtvStatePayload`, a
mirror is overwritten and re-derives visuals through the game's own reducers on change edges
(`atv_condition_sync`; ATV.md 17.17). First cross-peer equality measured 2026-08-30: host and
client ended a driven run with `dur=(100.00, 96.51, 100.00, 98.63)` **byte-equal**.

**THE REMAINING CRUTCH SURFACE, named (2026-08-30):**
1. **Client-authored tire-eject PRESENCE is REFUSED by the arbiter** — deliberate, and the register
   exists for exactly this row. A client-author eject ships `tires[i]=false` whose paired
   `prop_atvWheel_C` birth structurally cannot travel (the express seam is host-only, the wheel key
   is a per-peer random mint — no dedupe can ever exist), so consuming the mask would convert a
   retained-wheel divergence into host-PERSISTED item loss. Until the act-as-host tire-eject INTENT
   lane (ATV.md 17.5) exists, that direction stays divergent-as-today: host retains the wheel, the
   `presence-skipped-differing` counter counts it, and the (b2) acceptance arm ASSERTS the
   divergence so no all-green sheet hides it. The proper fix is the filed intent lane, not a wider
   apply.
2. **The pose stream is still the RELIABLE stopgap transport** (the Phase-1 doc's "acceptable only
   as a stopgap"); the unreliable-sequenced datagram remains owed.
3. `stateBits` bit0-2 are still produced and never read.

### Why it WAS a crutch — the dated indictment of the RETIRED freeze model (kept as record)

**[V] It deletes the reason the entity exists.** `AATV_C` is a constraint rig: four wheels as
independent rigid bodies on `sus_FL1/FR1/BL1/BR1` suspension and `ax_*` axle constraints. Freezing
the body deletes suspension travel, weight transfer and body pitch — the entire output of that rig.
And it does not even freeze cleanly: **the wheels are never parked**, so four live rigid bodies hang
off constraints attached to a parent that teleports ~20×/s. Steering is frozen too, because
`setFrontWheelsOrientation` is only called from tick `[V]`, and there is no drive torque because
`applyWheelTorque` is also tick-only `[V]`. The result is a rigid block sliding along a lerped path
with locked wheels — `[RD]`, never observed, which is itself the point.

**[V] The root-only park is a whole class of defect, not a detail.** `PrepareMirror` parks the root;
the ATV binds **seven `ComponentHitSignature` delegates** (`mesh`, `car1_Capsule`, `backWheel_R`,
`backWheel_L`, `frontWheel_R`, `frontWheelRoot`, `backWheelRoot`) plus `OnActorHit` → `hitActor`.
Delegates are dispatched by the collision system, not by tick, and neither traced handler gates on
`isDriven` or on being local — so **six of the seven already fire on every mirror in shipped b145**
and mirrors author their own damage. The same root-blindness lets a player grab a frozen mirror by
aiming at a **wheel**, because the grab gate reads `HitComponent.IsSimulatingPhysics()` `[V]` and the
wheels still simulate.

**[V] The transport was knowingly the wrong one.** The 2026-06-08 Phase-1 blueprint offered an
unreliable newest-wins datagram as option 1, marked **"RECOMMENDED"**, and a reliable stream as
option 2, marked **"acceptable only as a stopgap"**. The stopgap shipped. MTA sends both player and
vehicle puresync as `PACKET_RELIABILITY_UNRELIABLE_SEQUENCED` `[V]` (`CNetAPI.cpp:338,350`), and this
codebase already has that lane (`PropPose=8`, `EntityPose=32`, `OwnerEntityPose=95`).

**[V] Idle ATVs diverge in shipped code.** ~~On receivers an unauthored ATV is physics-ON *and*
tick-ON, so `fuel` / `battery` / `dirt` accumulate independently on every peer — the exact
`COOP_WORLD_PROP_DIVERGENCE` shape, in the one lane that never got its progression owner.~~
**CLOSED 2026-08-30 by v147:** the lane HAS its progression owner now — the syncer's condition
block overwrites every receiver (fuel divergence was the measured symptom, §13: 99.439 vs
100.000; run-A equality is the closing evidence). `battery` is deliberately out — it is an
inserted PROP's charge, the prop lane's row.

**[V] Three wire bits are produced and never read.** `stateBits` bit0 `isDriven`, bit1 `brake`,
bit2 `grabbed` are written by `ReadPayload` and consumed nowhere; only bit3 `authored` is read.

**[V] A shipped identity lane rests on a premise that is false — but the lane itself is NEEDED.**
`atv_sync.cpp:123` states *"a bought ATV is delivered ONLY on the host"*; no row in the 473-row
`list_store`, and no craft recipe, sells an ATV. The code's real predicate is *"a mid-session ATV not
in the baseline set"* — broader than its comment, and correct. **The RULE-2 deletion this was gated
on is CANCELLED** (census 2026-08-29, whole-pak, `docs/vehicles/ATV.md` §11.4): `list_props` row
`atv` has `spawnAsObject = ATV_C`, `hidden = false`, reached via `lib.PropToObject` →
`spawnPropThroughGamemode` from `ui_spawnmenu`, so a runtime ATV with a random per-peer key really
can exist and the synthetic-key lane is what covers it. **This one is a wrong COMMENT, not a crutch**
— it is listed here because the false premise was read as evidence for deleting a working lane, which
is the more expensive error. Fix: correct the comment.

**[V] A runtime hazard the lane does NOT cover.** `ATV.createContainer()` rung 5 (`@1201`) calls
`GetActorOfClass(prop_inventoryContainer_atv_C)` — the first such container **anywhere in the world,
with no key check** — and then `@1454` **re-keys it** to this ATV's name. `_map_untitled_211` ships
**two** `ATV_C` placements, so >1 ATV is a shipped configuration. `processKeys()` (the "re-derive
everything" seam a receiver wants) begins with `createContainer()`, so a mirror calling it can
mutate a *different* ATV's container key. Not live today (`[V]` we call neither), but it constrains
the fix.

**Never hands-on tested**, with a third-party field report open against it.

**[V] MEASURED 2026-08-29 (autonomous two-peer, `docs/vehicles/ATV.md` §13).** The indictment now has
numbers, and one of them is not what this entry assumed:

| | host (driving) | client |
|---|---|---|
| suspension travel susBK | 2.32 cm | **29.58 cm** |
| fuel | 100 -> 99.439 | **100.000** |
| body separation | | up to **109.9 cm** |

But **idle, the two peers agree almost exactly** (susBK range 4.29 vs 4.29, median separation
0.3 cm), and the client's wild numbers are **not** a mirror deformed by the pose stream -- they are
its own ATV, **launched at 158 cm/s by `AtvRelease`'s "un-freeze + inherit"** and rolling free. So
the crutch's worst measured symptom comes from the RELEASE path, which this entry did not name, and
the "frozen corpse" reading is right about the freeze but wrong about which step does the damage.

### The proper fix (direction settled 2026-08-29) — LEDGER UPDATED 2026-08-30
A **vehicle-sync subsystem in MTA's shape**, not more patches to a 692-LOC file: always-simulating
corrected mirrors (velocity + turn speed + error spread + a sync-time-context staleness gate,
`CNetAPI::ReadVehiclePuresync`), single-syncer election covering the idle case
(`CUnoccupiedVehicleSync`), an unreliable-sequenced pose datagram split cleanly from a reliable
mid-join seed and reliable discrete edges, host-canonical arrays for config
(`CVehicleUpgrades` ships `count + slot states` on join), and syncer-authored vitals with the arbiter
owning discrete writes. The decisive argument is not fidelity: **it deletes the freeze/unfreeze state
machine**, and nearly every defect found in this lane lives in that machine's transitions.

**Landed so far:** the freeze machine is DELETED and mirrors always simulate + correct (arc 1);
the release-path launch velocity is DELETED (v146); single-syncer election incl. idle (v146
`authorSlot`); syncer-authored vitals + host-canonical condition arrays with arbiter-gated
presence (v147); `AtvRelease` carries nothing to un-freeze. **Still owed:** the
unreliable-sequenced pose datagram; the act-as-host tire-eject/putTire intent lane (the register's
row 1 above); the hook_C boundary (ATV.md 16.6) remains unmeasured.

---

## C2 — trash piles and clumps

**Status: OPEN, next after C1.** No design pass has been run; the evidence below is a survey, not a
converged analysis.

### The crutch chain, in the order it was built
Each step is quoted from the modules' own headers `[V]`.

1. **The real mirror "died".** A client's mirror of a trash entity was a real `actorChipPile_C` /
   `prop_garbageClump_C`. Per `trash_proxy.h`, that BP *"runs its own ubergraph -> it self-morphs /
   self-destructs / is GC-eligible (unrooted) on its OWN schedule … Within ~10 s it goes NOT-LIVE"*
   → a visible dup.
2. **Crutch: replace the engine entity with a fake.** `trash_proxy` swaps it for a bare
   `AStaticMeshActor` *"we own"* — *"NO blueprint -> never self-morphs / self-destructs"*. This is
   principle 3 inverted: rather than keeping the engine class and parking its brain, the engine class
   is removed from the picture.
3. **The fake broke the game's aim, so a parallel aim system was built.** Verbatim: *"the client-grab
   AIM is a CAMERA-RAY CONE (`EidForAimedPileProxy`), NOT the game's interaction trace -- a bare
   AStaticMeshActor proxy can never be lookAtActor (the `int_player_C` filter)"*. **A crutch that
   needs a crutch.**
4. **It also lost collision, hover GUI, occlusion and rotation**, with the fix deferred: *"The
   FAITHFUL future collision … is a garbageCollider-analog SHAPE component on the proxy."*
5. **The stated reason was later measured FALSE.** `native_pile_mirror.h` `[V]`: an inertness probe
   on 2026-06-30 *"PROVED that death was GC (unrooted)"* — a rooted runtime native stayed live and
   inert for 60 s **with collision ON** and showed the native hover GUI. **The root cause was
   `AddToRoot` all along**; the entity never needed replacing.
5b. **AND THE CRUTCH CHARGED A CRASH, 2026-09-01.** The fake `AStaticMeshActor` is `AddToRoot`ed
   because a runtime spawn has no save/world reference to keep it alive — and that pin is what made
   a leaked release fatal rather than merely untidy. `[V]` 871 proxies stayed rooted through a
   session teardown (the un-root sat inside `if (liveActor)`, false at a world teardown) and
   anchored the departed `UWorld` through their Outer chain; the world was never collected, and the
   next in-process map load adopted the corpse and died dereferencing its null `WorldSettings`. The
   LEAK is fixed at the level (`ue_wrap::GcPin` owns the pin and releases from its destructor,
   `bb881bab`) — **but the pin exists only because the mirror is a fake actor we spawn**, so the
   proper fix for C2 removes the pin's reason to exist along with everything else. Full RE:
   `research/findings/join-identity/votv-rejoin-loadmap-null-worldsettings-RE-2026-08-31.md` §9.
6. **The fix was applied to half the concept.** `native_pile_mirror` restores a real rooted
   `actorChipPile_C` **for the pile form only**. *"The CLUMP form stays a bare proxy (it has a
   LifeSpan + autonomous re-pile-on-contact -> too live to keep as a native)"* — which is the same
   brain-parking problem this project solves everywhere else, declined once.

### What that leaves in the tree today
- **Two mirror implementations for one concept** compiled together (proxy for clumps, rooted native
  for piles) — RULE 2.
- **The parallel camera-ray-cone aim survives**, because the proxy it exists for survives.
- **~11 modules** in the pile/trash lane (`trash_proxy`, `native_pile_mirror`, `pile_spawn_bind`,
  `trash_pile_sync`, `trash_clump_pose_stream`, `trash_collect_sync`, `trash_grab_intent`,
  `trash_use_intercept`, `trash_channel`, `prop_stick_sync`, `garbage_sync`) — the module count is
  itself the accretion signature.

### The probable proper fix `[?]`
The same shape as C1: keep the **engine entity**, park its **brain**, drive it — i.e. finish step 6
by extending the rooted-native recipe to the clump form (parking its LifeSpan and re-pile-on-contact
the way every other lane parks a brain), then **retire the proxy and its parallel aim whole** (RULE
2). Not designed; C2 gets its own `/qf` pass after C1 ships.

---

## C3 — KO respawn (the `canRagdoll` gate)

**CLOSED BY RETIREMENT 2026-08-31 (`33008d87`), the same day it shipped and the same day it was
registered. The lane is gone from the tree; this entry is kept as the record.** Everything below
describes what USED to ship, and is written in the past tense only where it would otherwise mislead.

**Design of record: `docs/DEATH_ARC.md`.** Chain RE:
`research/findings/world-systems/votv-player-death-chain-RE-2026-08-31.md`.
Shipped `74c48694`, retired `33008d87`.

### What ships today

`ko_respawn::Tick` takes `Holder::KoRespawn` on `coop::ragdoll_gate` for the whole session and holds
`mainPlayer_C::canRagdoll` FALSE. `ragdollMode`'s first instruction is `IFNOT(canRagdoll) POP`, so
every ragdoll cause on the local pawn early-outs — **including the death**, which is the point, and
including the manual ragdoll key, fall knockdowns and the exhaustion faint, which is the cost. The
lane then polls `saveSlot.health <= 0`, borrows its own gate for one
`ragdollMode(true, passOut=true, death=false)`, waits `ko_ragdoll_seconds`, `forceWakeup()`s and
teleports.

### Why it is a crutch

It is §2's pattern exactly, one level below the entity: **the game's own mechanism was inconvenient,
so it was switched off, instead of being kept and answered.** The declared cost was written into the
header and the config row as if declaring it settled it — and the user's answer when they saw the
design was that the cost is not acceptable and not necessary:

> *"Я хочу чтобы игрок ощущал нативную смерть, но без выкидывания в главное меню... момент когда игра
> захочет отослать игрока в главное меню и выгрузить карту и сломать весь мир - тут наш мод вступает и
> НОВОЕ ИЗМЕНЕННОЕ СОСТОЯНИЕ ПИШЕТ."*

The measurement that settles it: the death chain's last hop is a **NATIVE** `OpenLevel`
(`mainGamemode` uber `@7160`), and by the time it runs both `RetriggerableDelay`s have been consumed
— so the death can be allowed to happen in full and answered at the end, with no suppression
anywhere. (This paragraph used to name the `UFunction::Func` seam. IDA, 2026-08-31: the seam is a
MinHook detour on `UGameplayStatics::OpenLevel` at `0x142B530B0` instead, because cancelling the
exec THUNK would mean consuming the caller's parameter bytecode ourselves. `DEATH_ARC.md` §3.)

### Three HIGH defects are LIVE in the shipped build `[V]` 2026-08-31

Found by a post-ship audit and each re-verified by hand this session:

* **H1 — a cancelled or failed join strands the player permanently.** `ragdoll_gate::ReleaseAll()`
  has **zero call sites** (`grep`: declaration + definition only), and the only release path,
  `ko_respawn::OnDisconnect`, is reached solely via `subsystems::DisconnectAll()` from
  `net_pump.cpp:269`/`:612` — both of which require a peer to have actually connected. So a player who
  opens the browser, clicks a lobby and cancels keeps `canRagdoll = false` **for the rest of the
  process**: no ragdoll key, no fall knockdown, no faint, and no death either. Both the module header
  and the code comment promise this cannot happen.
* **H2 — the post-respawn immunity does not protect against the hit it exists for.** `ko_respawn.cpp`
  reads `hp` at `:191`, the immunity pin writes health back to 100 at `:201`, and the KO trigger at
  `:210` then evaluates the **stale** `hp` — so a >=100 damage hit inside the window pins health AND
  fires `StartKO` anyway. With `ko_spawn_at_start = 0` that is a permanent KO loop.
* **H3 — CLOSED THE SAME DAY IT WAS OPENED (2026-08-31).** `[V]` re-measured at `33008d87`:
  `g_pawn` is a `ue_wrap::CachedObjRef` (`ragdoll_gate.cpp:24`), read through `.Get()` at `:76`
  and `:84`, and `tools/reflection/islive_gate.ps1` prints *"islive_gate PASS: no bare IsLive on
  a static cached pointer"* tree-wide. The row is kept rather than deleted because the ORIGINAL
  hazard is the durable part and is why the fix has the shape it does: a bare `void*` cached
  across ticks and written into after a bare `IsLive` probe is the measured 2026-08-23
  dying-world shape, where a dead world's actors stay unflagged for 44+ seconds.
  ~~`docs/UE4SS_ARC.md` and `docs/LESSONS.md` both still assert "CI PASS ... tree-wide"; that
  claim is false as of `74c48694`.~~ — both were re-cited on 2026-08-31 and the assertion is
  true again; the citation this row carried (`:104`) is past EOF in the current 94-line file,
  which is what `lessons_gate` caught.

**So on one axis the shipped lane was worse than the bug it replaced** (H1 could take a
single-player's ragdoll away for good). **That call was made on 2026-08-31: retire.** Fixing three
defects in code the arc deletes is work with no destination, and H1 was live on the deployed build,
so the retire went first rather than last. All three died with `33008d87`; H3's `g_pawn` became a
`CachedObjRef` in the same commit, because the gate module survives.

### The proper fix

`docs/DEATH_ARC.md` in full. In one line: let the whole native death run, intercept the NATIVE
`OpenLevel` when the local pawn has `dead == true` (fail CLOSED), and write the revive in the game's
own verbs — `dead := false`, remove `blackScreen_C`, `forceWakeup()`,
`teleportWObackrooms(spawnLocation, true, false)`. The `ragdoll_gate` MODULE stays: `wisp_attack_sync`
has a real, pre-existing need for it. Only `ko_respawn`'s hold goes.

---

## 2. The pattern all three entries share

All three are the same move: **the thing the game already does was inconvenient, so it was
neutralised or replaced, instead of being kept and answered.** C1 and C2 do it to an ENTITY; C3 does
it to a MECHANISM (a boolean the game reads before every ragdoll), which is why it did not look like
the same mistake while it was being made. The ATV freezes it into a corpse; the clump swaps it
for a fake. In both cases the compensating machinery (interp over a dead rig; a camera-ray aim
system) is larger than the fix would have been, and in both cases the original obstacle turned out to
be one property — actor tick for the ATV, `AddToRoot` for the pile.

**Look here FIRST:** when a mirrored engine entity misbehaves, the question is *which brain do I park*
— tick, delegates, LifeSpan, timers, GC-rooting — **not** *how do I stop being the engine entity*.
Principle 3 is the test: if our design no longer holds a pointer to the game's own actor doing the
game's own rendering and physics, the design has left the architecture.

---

## 3. Register discipline

- An entry is **added** when a crutch is measured, with file:line evidence — never on a hunch.
- An entry is **retired** only when the crutch is gone from the tree, not when a fix is designed.
  Retired entries move to a CLOSED section with the commit that removed them.
- A crutch that is deliberately kept needs a written retirement plan naming what it masks, the
  targeted fix, and the gating criteria (principle 4's transitional-crutch exception).
- New crutches found during unrelated work belong here immediately; the register is worth exactly as
  much as its completeness.

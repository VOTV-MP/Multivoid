# VOTV trash-clump ball->pile conversion — exact BP trigger, the ~3 s mirror lag root cause, and an event-driven fix — 2026-06-08

Builds on `research/findings/votv-clump-mirror-grab-RE-2026-06-08.md` (the grab-trace + the
PhysicsOnly lock) and `…class-clone-migration-roadmap-2026-06-06.md`. This doc adds the part
those left as a guess: **WHEN and HOW** the clump ball turns into a ground pile, proven from the
kismet bytecode, and why our mirror lags ~3 s. Implementation-ready.

RE sources (all VERIFIED, re-runnable):
- `research/bp_reflection/prop_garbageClump.json` (cooked BP disassembly; 94 fns).
- Traces via `python research/bp_reflection/_cfg.py prop_garbageClump ExecuteUbergraph_prop_garbageClump <entry>`
  and raw statements via `_dumpstmt.py`.

---

## 0. TL;DR

- **The ball->pile trigger is the clump `StaticMesh.OnComponentHit` delegate** (bound in the
  CDO) → `ExecuteUbergraph_prop_garbageClump(2702)`. It is **NOT** a timer, velocity-settle,
  or fixed delay. It fires on physics contact.
- **It is NOT instant-on-first-landing.** There is a one-shot debounce: `delayOnHit` starts
  `true`; the FIRST ground hit only *arms* it (sets `delayOnHit:=false`) and returns. The
  **conversion fires on the SECOND+ ground contact**, gated by: `canConvert` (always true) AND
  the player is not still grabbing it AND the impact surface is near-flat (`Dot(HitNormal,
  Up) > slope=0.75`) AND the surface is static (`!IsSimulatingPhysics(HitComponent)`).
- **The prior doc's claim that self-conversion was "~3/10 unreliable due to impulse/slope
  gates" was a half-truth.** The slope gate is real but easy to satisfy on a floor. The actual
  reason a *naively-primed* mirror was unreliable is the **`delayOnHit` two-hit debounce + the
  "not still grabbing" + static-surface gates** — none of which the prior code reproduced. With
  the real gate known, the mirror CAN be made to convert exactly like SP. But the **recommended**
  fix is event-driven (Option A), which removes the dependency on physics entirely.
- **The ~3 s mirror lag is NOT network latency and NOT detection latency.** `TickWatchReleasedClumps`
  runs every game-thread frame. The lag is the **wall-clock time the OWNER's own clump takes to
  die**: our pipeline only broadcasts the authoritative pile *after the owner's clump actor is
  destroyed*, and the owner's clump isn't destroyed until IT bounces, satisfies the two-hit
  debounce, and self-converts. That physical settle (bounce → 2nd contact on flat static ground)
  is the ~1-3 s. Meanwhile our mirror is deliberately inert (hit-notify disabled at spawn), so it
  just sits there for that whole window = the grabbable-dupe window + the visible "ball lingers"
  lag.
- **Fix (Option A, MTA-shaped):** the OWNER broadcasts a **PropConvert** event the instant its
  clump converts — at the `OnComponentHit` PRE, the moment the BP is about to spawn the pile —
  carrying the dying clump's eid + the resolved pile transform + chipType + landing velocity.
  The peer **atomically** destroys the mirror ball (by eid) and spawns the authoritative pile.
  Zero lag, zero dupe, no dependence on the mirror's own physics.

---

## 1. The conversion is the StaticMesh hit delegate — exact wiring

CDO `Default__prop_garbageClump_C` binds (VERIFIED, CDO `ComponentDelegateBindings`):

```
ComponentPropertyName = "StaticMesh"
DelegatePropertyName  = "OnComponentHit"
FunctionNameToBind    = "BndEvt__prop_garbageClump_StaticMesh_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature"
```

That bound handler (VERIFIED, `_disasm.py`):

```
[0..4] UBERFRAME[K2Node_ComponentBoundEvent_*] = HitComponent / OtherActor / OtherComp / NormalImpulse / Hit
[5] CALL ExecuteUbergraph_prop_garbageClump(2702)
[6] RETURN
```

So every physics contact on the clump's `StaticMesh` (which IS the actor root — see the grab-RE
doc §3) runs the ubergraph from offset **@2702**.

Other candidate entry points were checked and are dead ends for conversion — each is a bare
`EX_PopExecutionFlow` (immediate return): `physDestroyed`(@2655), `physPreDestroyed`(@2656),
`steppedOn`(@2659), `receivedPhyiscsDamage`(@2663), `impactDamage`(@3893). **Conversion happens
only through the hit delegate.** (VERIFIED via `_dumpstmt.py`.)

---

## 2. The debounce + gate chain — byte-exact flow from @2702

(Offsets are kismet byte offsets in `ExecuteUbergraph_prop_garbageClump`; `_cfg.py` traces.)

```
@2702  IFNOT(delayOnHit) JUMP @2874        ; delayOnHit==true  -> fall to @2716 (ARM)
                                           ; delayOnHit==false -> JUMP @2874 (CONVERT gate)
--- ARM path (first hit) ---
@2716  delayOnHit := false                 ; one-shot disarm (never re-armed in the graph)
@2727  d := Max / 2
@2769  t := RandomFloatInRange(d, Max)      ; Max default = 1.0 (CDO); =2.0 for a grab-spawned clump
@2815  RetriggerableDelay(t) -> resume @15  ; @15: canConvert := true   <-- REDUNDANT refresh
@2873  POP (return)                         ; FIRST HIT DOES NOT CONVERT
--- CONVERT gate (second+ hit) ---
@2874  POPFLOWIFNOT(canConvert)             ; canConvert is ALWAYS true (only ever set true) -> passes
@2884  IsValid(holdPlayer) ?  no -> JUMP @2993 (convert) : yes -> @2927
@2927  IsValid(holdPlayer.grabbing_actor) ? no -> JUMP @2993 (convert) : yes -> @2992 POP (still held, abort)
@2993  BreakHitResult(Hit) ...
@3242  Greater( Dot(HitNormal,(0,0,1)) , slope=0.75 ) ?  no -> POP (too steep, abort)
@3462  IsSimulatingPhysics(HitComponent) ?  yes -> POP (hit a dynamic body, abort) : no -> @3536
@3536  StaticMesh.SetNotifyRigidBodyCollision(false)   ; stop further hits
@3573  Delay(+0f) -> resume @27                          ; 1-frame defer, then SPAWN
--- SPAWN block @27 (the actual conversion) ---
@27..  downward SphereTrace -> resting transform on the ground
@1722  pile := BeginDeferredActorSpawnFromClass(pile, restingXform)
@1764  pile.chipType := self.chipType
@2557  FinishSpawningActor(pile)
@2595  pile.SetLifeSpan(lifespan)
@2640  K2_DestroyActor()                                 ; destroy SELF (the clump)
```

### Field defaults (VERIFIED — class property table + CDO Data)
- `canConvert`  default **true**  (class bool default). Only ever written `true` (@15); never `false`.
- `delayOnHit`  default **true**  (class bool default). Only ever written `false` (@2716).
- `Max`         CDO **1.0**. **`actorChipPile_C::turnToPile` sets `Max:=2.0`** on the clump it
  spawns when you GRAB a pile (VERIFIED, `_disasm.py actorChipPile turnToPile` stmt [5]). So a
  player-grabbed clump carries `Max=2.0`.
- `slope`       CDO **0.75** (≈ within 41° of straight-up = floors/gentle slopes pass).
- `pile`        the chipPile class to spawn (the `clump<->pile` pair).

### What this means for "instant on the owner's screen"
The owner's clump still needs **two** ground contacts. In practice a thrown/dropped clump
bounces and re-contacts within a fraction of a second, so it *feels* instant. The
`RetriggerableDelay`→`canConvert:=true` is a **no-op for timing** (canConvert is already true);
it does not delay conversion. The only thing gating the SECOND hit is the physics: the clump must
actually touch flat static ground a second time while NOT held.

---

## 3. Where the ~3 s MIRROR lag comes from (our pipeline)

Trace of the existing flow (files/lines cited):

1. **Owner grabs a pile** → pile→clump via `turnToPile` (clump `Max=2.0`). `net_pump.cpp:832-882`
   streams the held clump's pose; `trash_collect_sync::EnsureHeldItemBroadcast`
   (`trash_collect_sync.cpp:111`) broadcasts a `PropSpawn(key=None, eid)` so the peer spawns a
   **mirror ball** and death-watches the clump (`WatchClump`, line 194).

2. **Peer spawns the mirror** → `remote_prop_spawn.cpp:432-457`: for a `garbageClump` it calls
   `SetActorRootNotifyRigidBodyCollision(spawned,false)` (**line 442**) — the mirror's
   `OnComponentHit` is **permanently disabled**, so the mirror can NEVER run its own ubergraph
   conversion. By design (prevents a double-pile). The mirror is then a kinematic puppet.

3. **Owner releases** → `net_pump.cpp:884-917` sends `PropRelease(vel)`. On the peer,
   `remote_prop::OnRelease` (`remote_prop.cpp:495-525`) re-enables physics on the mirror with
   **PhysicsOnly** collision (2) + the throw velocity → the mirror **flies, lands, and rests as a
   ball**, but with hit-notify off it never converts, and with PhysicsOnly it can't be grabbed.

4. **Owner's clump lands + self-converts** (per §2: bounce → 2nd flat-static contact → spawn pile
   + `K2_DestroyActor`). **This is the slow step.** Wall-clock from release to the owner's clump
   actually dying = the bounce/settle time ≈ **1-3 s** depending on drop height/throw.

5. **Only NOW** does our death-watch notice: `TickWatchReleasedClumps` (`trash_collect_sync.cpp:199`,
   runs **every game-thread frame** via `net_pump.cpp:672`) sees the watched clump's actor go
   dead (`IsLiveByIndex` false) → calls `BroadcastLandedPileNear` (line 70): finds the
   owner's freshly-spawned chipPile within 200 cm, mints an eid, broadcasts
   `PropSpawn(kFreshLanded)` → peer spawns the **authoritative pile** + `turnToPile` impact sound
   (`remote_prop_spawn.cpp:474-479`) → AND sends `PropDestroy(eid)` to despawn the mirror ball
   (line 217-220).

**So the lag = step 4 (the owner's physical settle), not the network and not detection.** During
the entire step-4 window the peer shows the inert mirror ball resting on the ground. That is both
the "~3 s ball lingers" symptom AND the dupe window (before the PhysicsOnly fix, the mirror was
grabbable in that window; the user already hit that).

### Secondary correctness note on the current despawn
The mirror is destroyed (step 5 `PropDestroy(eid)`) and the pile is spawned (step 5
`PropSpawn(kFreshLanded, eid')`) as **two separate reliable messages with two different eids**.
They are ordered (same lane, sent back-to-back) so the pile lands ~same frame as the ball
vanishes — acceptable. But it is NOT atomic by construction; an event-driven single packet (§4)
makes the swap atomic and removes the lag entirely.

---

## 4. THE FIX — Option A, event-driven convert (recommended)

**Shape (MTA precedent):** the OWNER is authoritative for the conversion; it sends a single
reliable **PropConvert** event the instant its clump converts, carrying everything the peer needs
to swap ball→pile in one step. This is the same shape as MTA `CElementRPCs` /
`Packet_EntityRemove`+spawn folded into one authoritative state-change event — the receiver never
runs the conversion physics itself; it just applies the host's result.
(`reference/mtasa-blue/Shared/mods/deathmatch/logic/CElementRPCs.cpp`, the "server says X happened
to entity E, client applies" pattern.)

### 4.1 Detection on the owner: intercept the clump's own conversion, PRE

Hook `prop_garbageClump_C::ExecuteUbergraph_prop_garbageClump` is not viable (ubergraph is one
giant function; we can't gate on the internal offset). Instead hook the **bound hit handler**
`BndEvt__prop_garbageClump_StaticMesh_…_ComponentHitSignature__DelegateSignature` (it is a real
UFunction on the class, ProcessEvent-dispatched — our observer system can bind it) as a **PRE
observer**, and re-evaluate the SAME gate the BP will: if `delayOnHit==false && canConvert &&
holdPlayer-not-grabbing && Dot(Hit.Normal,Up)>slope && !IsSimulatingPhysics(Hit.Component)` then
the BP is ABOUT to convert this clump on this very call → fire our broadcast now, using the same
inputs.

- This is RULE-1 clean: we read the real gate, we don't guess. The clump's fields are reflectable
  (`delayOnHit`, `canConvert`, `slope`, `holdPlayer`, `chipType` are BP properties; `Hit` is the
  delegate param the observer receives in `params`).
- We do NOT need to drive or suppress the owner's own conversion — let the BP destroy the owner's
  clump normally. We only *observe* the convert edge to time the broadcast.
- Owner-only relevance: this fires on whichever peer physically holds/threw the clump (the same
  peer that already runs `EnsureHeldItemBroadcast` + the death-watch for that eid).

**Why PRE not POST:** the conversion ends with `K2_DestroyActor()` on self inside the same
ubergraph call; by POST the clump actor and its transform may already be torn down. PRE gives us
the live clump (its eid + current position + velocity + chipType) and the impact `Hit` (the
resting location the pile will use).

Alternative detection (simpler, slightly less precise): keep the existing **death-watch** but stop
waiting for "find a nearby pile". The instant the watched clump's actor dies, we already know it
converted; broadcast PropConvert with the clump's `lastPos`/`lastVel`/chipType captured the frame
before death. This removes the `FindNearestChipPile` dependency and the 200 cm miss risk, but
still incurs the step-4 settle wait (it doesn't fix the lag — only the PRE-observer does). **Use
the PRE observer to kill the lag; the death-watch becomes a backstop only.**

### 4.2 Payload — reuse `PropSpawnPayload` + a flag, or a tiny new `PropConvertPayload`

Minimal, no new lane. Two viable encodings:

**(a) Reuse the existing `PropSpawn(kFreshLanded)` you already have, but send it at the PRE-convert
edge instead of at clump-death, AND carry the dying clump's eid so the receiver swaps atomically.**
Add one field to `PropSpawnPayload` is unnecessary — the payload already has `elementId` (the NEW
pile's minted eid) and we can pass the OLD clump eid in the existing `key`/an unused field. Cleaner:

**(b) New `PropConvertPayload` (preferred, explicit):**
```cpp
struct PropConvertPayload {
    uint32_t  oldEid;        // the mirror BALL to destroy (the clump's broadcast eid)
    uint32_t  newEid;        // freshly minted id for the authoritative pile
    WireClassName pileClass; // "actorChipPile_C" (or read off self.pile)
    float locX, locY, locZ;  // resting transform (from the BP's downward sphere-trace, or clump loc)
    float rotPitch, rotYaw, rotRoll;
    uint8_t chipType;        // the variant (clump.chipType)
    float velX, velY, velZ;  // landing velocity -> turnToPile impact dust+sound
};
```
Add a `ReliableKinds::PropConvert` + route in `event_feed`. ~56 B, one reliable datagram.

### 4.3 Receiver — atomic ball->pile swap (no dupe, no lag)

In a new `remote_prop::OnConvert(payload)` (game thread, via event_feed):
1. Resolve + **destroy the mirror ball** by `oldEid`: `ResolveLiveActorByEid(oldEid)` →
   `ConsumeLocalActor(actor)` (echo-suppressed) + `UnregisterPropMirror(oldEid)`. (Exactly the
   teardown `OnDestroy` already does for the eid path — reuse it.)
2. **Spawn the authoritative pile** at the payload transform via the existing
   `remote_prop_spawn::OnSpawn` fresh-spawn path with `kFreshLanded` set (so it fires
   `turnToPile(vel)` for the impact dust+sound) and `newEid` bound. This is the SAME `OnSpawn`
   code that step-5 already calls — no new spawn logic.
3. Both happen in the one handler, same tick → the ball disappears the exact frame the pile
   appears. No lingering ball, no second pile, nothing grabbable in between.

Because the broadcast now fires at the owner's **convert edge** (PRE, the instant the BP commits
to spawning the pile) rather than after the owner's clump finishes dying and our watcher notices,
the peer's pile appears ~1 RTT after the owner converts — effectively in lockstep with the owner's
own screen, killing the ~3 s window.

### 4.4 What gets RETIRED (RULE 2 — no parallel paths)

When Option A lands, remove from `trash_collect_sync.cpp` / `remote_prop_spawn.cpp`:
- `BroadcastLandedPileNear` + its `FindNearestChipPile`-200cm search (the convert event carries the
  exact transform; no spatial guess).
- The death-watch's pile-broadcast responsibility (`TickWatchReleasedClumps` keeps ONLY the
  PropDestroy backstop for the case where the clump dies WITHOUT converting — e.g. LifeSpan expiry
  / despawn — so the mirror still gets cleaned; but it no longer spawns piles).
- The `kFreshLanded` "find the pile after death" timing is replaced by the convert event setting
  `kFreshLanded` directly.
Keep the `SetActorRootNotifyRigidBodyCollision(spawned,false)` mirror-inert guard and the
PhysicsOnly release lock (they remain correct: the mirror must never self-convert or be grabbable;
the owner's convert event is the sole pile source).

---

## 5. Why NOT Option B (drive the mirror's own conversion)

Option B = re-enable the mirror's hit-notify on release and let its own ubergraph convert it on
landing. Now that the gate is fully known (§2) this is *implementable* (set `delayOnHit:=false` so
it converts on the FIRST hit, ensure `canConvert`, drop it on flat static ground). But it is
inferior:
- It still depends on the mirror physically bouncing to a flat static contact on the peer — the
  peer's ground geometry/scale must match, and a throw onto a slope/edge would NOT convert
  (slope>0.75 gate) → divergence vs the owner who DID convert. This is exactly the "~3/10
  unreliable" the prior doc saw, now explained: it's the slope + two-hit gates, not randomness.
- It produces TWO independent conversions (owner's + mirror's), each spawning its own pile with no
  shared identity → the peers' piles aren't the same entity → re-grab/destroy won't be in sync.
- It re-opens the grab-dupe window (a hit-notify-enabled, self-converting mirror is a real physics
  body again).

Option A makes the conversion **authoritative + atomic + transform-exact** and removes all physics
dependence on the receiver. It is the MTA shape and the RULE-1 root-cause fix. **Recommend A.**

---

## 6. Files / offsets cited (implementation map)

- Trigger: clump CDO binds `StaticMesh.OnComponentHit` →
  `BndEvt__prop_garbageClump_StaticMesh_…_ComponentHitSignature__DelegateSignature` →
  `ExecuteUbergraph_prop_garbageClump(@2702)`.
- Gate chain: @2702 (`delayOnHit`), @2716 (arm), @2874 (`canConvert`), @2884/@2927
  (`holdPlayer`/`grabbing_actor`), @3242 (`slope=0.75`, normal·Up), @3462/@3521
  (`IsSimulatingPhysics`), @3536 (`SetNotifyRigidBodyCollision(false)`), @3573 (`Delay+0`),
  @27 (`BeginDeferredActorSpawnFromClass(pile)` → @2640 `K2_DestroyActor`).
- `actorChipPile_C::turnToPile` sets the spawned clump's `Max:=2.0` (stmt [5]).
- Owner broadcast (today): `src/votv-coop/src/coop/trash_collect_sync.cpp`
  (`EnsureHeldItemBroadcast`:111, `BroadcastLandedPileNear`:70, `TickWatchReleasedClumps`:199).
- Owner release: `src/votv-coop/src/coop/net_pump.cpp:884-917`; watch tick call: `net_pump.cpp:672`.
- Mirror spawn (inert): `src/votv-coop/src/coop/remote_prop_spawn.cpp:432-457` (line 442 disables
  hit-notify); landed-pile spawn + sound: `remote_prop_spawn.cpp:474-479`.
- Mirror release (PhysicsOnly lock): `src/votv-coop/src/coop/remote_prop.cpp:495-525`.
- Receiver destroy-by-eid (reuse for the atomic swap): `remote_prop.cpp::OnDestroy:691-756`,
  `ConsumeLocalActor:758-773`, `ResolveLiveActorByEid:288-299`.
- New work: a PRE observer on the clump hit-handler UFunction (detection) + `ReliableKinds::PropConvert`
  + `PropConvertPayload` (`include/coop/net/protocol.h`) + `event_feed` route +
  `remote_prop::OnConvert` (atomic destroy-old-eid + OnSpawn-new-with-kFreshLanded).

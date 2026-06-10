# VOTV ATV (quadbike) — RE + coop-sync design (2026-06-08)

Status: **RE COMPLETE, implementation DEFERRED** (greenfield subsystem; needs a
focused hands-on-tested pass). User report that triggered this: *"ATV вообще не
синронизирован не зазеркален"* + its interactions (pick up as object, refuel,
spacebar handbrake, damage) are not synced.

Authoritative sources: `Game_0.9.0n/.../CXXHeaderDump/ATV.hpp` (class layout +
byte offsets), `research/bp_reflection/ATV_cfg/ATV.txt` (offset-aware CFG) +
`research/bp_reflection/ATV.json`. In scope per `docs/COOP_SCOPE.md:129`
("5N4 vehicles (ATV driver-authoritative…)") — no scope amendment needed.

## 1. `AATV_C` anatomy (`: public APawn`, size 0x8A1)

NOT a `UWheeledVehicleMovementComponent` vehicle. It is a **custom physics rig**:
the root `Mesh` (UStaticMeshComponent @ 0x0570) simulates PhysX; four wheels are
constrained to it via `UPhysicsConstraintComponent` suspension (`sus_*`) + axle
(`ax_*`); drive is torque on those constraints (`applyWheelTorque`,
`setFrontWheelsOrientation`). There is **no movement-component to feed input into
on a remote** — the body transform is the only ground truth.

| Field | Offset | Type | Meaning |
|---|---|---|---|
| `Mesh` | 0x0570 | UStaticMeshComponent* | physics-simulating root body (pose-stream THIS) |
| `playerHit` | 0x04F0 | UCapsuleComponent* | the SEAT anchor (driver placed at its world xform) |
| `Player` | 0x05B0 | AmainPlayer_C* | current driver/occupant (null when nobody drives) |
| `Speed` | 0x05C0 | float | current speed (FX/display) |
| `IsDrive` | 0x05D0 | bool | engine running |
| `fuel` | **0x05D4** | float | gas level (0..100; save-persisted) |
| `Empty` | 0x05D8 | bool | out of fuel |
| `Brake` | **0x05D9** | bool | handbrake state (SPACEBAR) |
| `lights` | 0x05E0 | bool | headlights |
| `health` | **0x05E4** | float | damage/health (0..100; <=0 -> explode) |
| `imp`/`brokenn` | 0x05E8/0x05E9 | bool | impacted / broken |
| `isDriven` | **0x05F7** | bool | TRUE while a player is seated (master "occupied" flag) |
| `lastloc` | 0x05F8 | FVector | last location (interp) |
| `hides` | 0x0608 | TArray<AActor*> | actors hidden while seated |
| `Key` | **0x0618** | FName | cross-peer-stable save identity (`setKey`/`getOnlyKey`) |
| `dirt` | 0x07D8 | float | body dirt (could reuse the grime keyed-scalar shape) |
| `battery` | 0x06E4 | float | battery |
| Modules/upgrade bools | 0x06D0… | TArray/bools | hasBigLights/Bumper/Solar/Guns/Floaties/Fly… (out of MVP) |

No dedicated "isBeingHeld" bool — held-state is inferred (see §3.5).

## 2. Movement model -> **kinematic pose-stream** (confirmed)

Position is authoritative from PhysX on the root `Mesh` (constraint-driven wheel
torque, gravity, collisions, `impulse()`). No input-fed movement component to
replicate. So: **stream the resolved root transform; drive the mirror
kinematically** — identical to `element::Npc` (and held-prop `PropPose`), NOT
input-replication.

MTA precedent: `CVehiclePuresyncPacket.cpp:122-143` streams the vehicle's
pos/rot/velocity/turnspeed off the syncing player; receiver
`SetPosition/SetRotationDegrees/SetVelocity/SetTurnSpeed`. Unoccupied vehicles get
a single nearest-player syncer (`CUnoccupiedVehicleSync::OverrideSyncer/FindSyncer`,
`CUnoccupiedVehicleSync.cpp:59,144`).

**Authority:** occupant-authoritative when driven, host-authoritative when at rest
(matches scope "driver-authoritative" + the project's existing host-as-default
single-syncer). On the mirror, disable the ATV's local Tick physics so its own
`applyWheelTorque`/gravity can't fight the streamed transform (the same "mirror
tick off" + `SetActorRootNotifyRigidBodyCollision(false)` discipline already used
for kerfur mesh-yaw and clump mirrors).

## 3. Byte-exact interaction paths (ubergraph offsets from `ATV_cfg/ATV.txt`)

3.1 **Seating (`playerSit`, ub 5446->5616->7013):** hide `hides[]`;
`player=&<mounter>`; `prevPlayer=player`; `player.Capsule.SetMassScale(None,0)`;
**`player.K2_AttachToActor(Self, …SnapToTarget…)`**; place `player` at `playerHit`
world xform; `player.SetActorHiddenInGame(true)`; **`GetPlayerController(0).Possess
(Self)`** (unpossesses mainPlayer_C — the discriminator the mod already relies on);
attach `player.light_R` to `lagFl`; **`isDriven=true`**. Player-side: `player.atv=
Self` (block 6755).

3.2 **Dismount (`playerUnsit` ub 7540 / `dismount` ub 45915):** reverse —
un-hide playermodel + `hides[]`, `player.unzoom()`, `player=NoObject`,
`audio_car_stop`, clear `sittingKerfuro`/`allKerfuros`. (`driven()`/`dismounted()`
are empty BP stubs.)

3.3 **Refuel (`fuelUp`->`getFuel`):** gas canister overlaps `fuelbox`
(UBoxComponent @ 0x0550) -> `fuelUp(gascan)` -> `gascan.getFuel(fuel /*by-ref*/,
100, hasFueled, changed)` transfers into **`fuel`@0x05D4** up to 100 + refuel
sound. **Mirror = stream the `fuel` float** (host/driver-authoritative); no need to
replicate the canister interaction.

3.4 **Handbrake (SPACEBAR):** `InpActEvt_atv_brake_…` (the `atv_brake` input action)
-> ub 47593 -> `f_brake` (ub 7283: `brake=true; setBrake()`) or `unbrake`
(`brake=false; setBrake(); sound_drive(); torqAlpha=0`). `setBrake()` writes
**`Brake`@0x05D9** + applies angular drive to the `ax_BL1`/`ax_BR1` rear-axle
constraints. **Mirror = write `Brake` + call `setBrake()`** on the mirror.

3.5 **Damage (`addDamage`/`damageByPlayer`/`ImpactDamage`/`receivedPhyiscsDamage`
-> block 19716):** `health -= damage*2*getBumperMult()` on **`health`@0x05E4**,
then `updHealth()` (block 20042: if health<=0 && fwd-speed>4 -> `explode()`;
`audio_car_damage`). `updHealth()` is pure (FMax/FClamp + smoke FX by health
fraction). **Mirror = stream `health`** (display + smoke via `updHealth()`) +
replicate the broken/explode edge as a DISCRETE reliable event. MTA precedent:
`CVehiclePuresyncPacket.cpp:150-171` applies health **only on decrease**, fires the
damage event, then `SetHealth`; keeps `LastSyncedHealth` to avoid feedback.

3.6 **Pickup/carry (`playerGrabbed` ub 7093 / `beginHoldingObject` ub 7095):**
`canPickup()` and `playerTryToHold()` BOTH return FALSE — so the ATV does NOT ride
the small-prop quick-hold/`takeObj` path. It is grabbed via the **heavy physics-grab**
(`setPropProps(Static,frozen,Active,sleeping)` toggling the root body's physics).
There is no held bool; held-vs-driven-vs-resting is disambiguated by `isDriven`
(driven) vs the body physics state from grab (held) vs `IsAnyRigidBodyAwake(mesh)`
(resting). For MVP carry is naturally covered by the pose stream; the discrete
grab/release edges only matter to suppress physics-fighting on the mirror (reuse the
clump's `SetActorRootNotifyRigidBodyCollision(false)`/`setPropProps` discipline).

3.7 **Seating -> puppet:** when peer X mounts, on the OTHER peers their X-puppet must
(a) be hidden / attached to the mirror ATV's `playerHit`, (b) the ATV mirror's
`isDriven=true`. Cleanest MVP: stream an `occupantSlot` on the ATV state; receiver,
when occupant==slot N, hides puppet N's body + parents it to the mirror ATV seat
each pose tick (the puppet pose stream then follows for free), un-hides on
un-occupy. MTA `SOccupiedSeatSync` + `GetOccupiedVehicleSeat`
(`CVehiclePuresyncPacket.cpp:107-118`).

## 4. Implementation blueprint (phased, reuse-first)

**Extend the NPC-sync pattern, NOT remote_prop.** The ATV is a non-player pawn with
a pose stream + reliable state payload + connect-snapshot — structurally the kerfur.
Reuse: `coop::LerpWindow` (`include/coop/lerp_window.h`), `element::Element`/
`Registry`/`MirrorManager` (`include/coop/element/*`), the host TickPoseStream +
connect-snapshot shape in `npc_sync.{h,cpp}`.

New files (principle-7 split, each <800 LOC):
- **`ue_wrap/atv.{h,cpp}`** — engine substrate (NO net/gameplay). Getters:
  `GetMesh`, `GetRootTransform/Velocity/AngularVelocity`, `GetFuel`, `GetHealth`,
  `GetBrake`, `IsDriven`, `GetOccupant` (resolve `Player`->peer slot), `GetKey`
  (`getOnlyKey`). Setters/thunks: `SetRootTransformKinematic` (+ disable mirror Tick/
  physics), `SetFuel`, `SetHealth`+`CallUpdHealth`, `SetBrake`+`CallSetBrake`,
  `CallExplode`/`CallBroken` for the death edge, `SetActorRootNotifyRigidBody
  Collision(false)` (exists in `ue_wrap::engine`). Offsets per §1 via
  `FindPropertyOffset` (like base_window/grime).
- **`coop/atv_sync.{h,cpp}`** — gameplay/network. Host/driver `TickAtvStream` reads
  each ATV's transform+vel+fuel+health+brake+occupant, publishes; client applies via
  an `element::Atv` (RemotePlayer-twin owning a LerpWindow). Connect-snapshot every
  ATV by `Key`. Symmetric occupant authority; host-as-default-syncer when unoccupied.
- **`coop/element/atv.{h,cpp}`** (or fold into atv_sync) — the `element::Atv` twin
  (copy `element::Npc`, swap CMC drive for kinematic root-transform drive + apply
  fuel/health/brake/occupant).

Protocol (`include/coop/net/protocol.h`, bump to **v44**):
- **Pose:** a new `AtvPosePacket` (header + `AtvPoseSnapshot{ elementId, x,y,z,
  pitch,yaw,roll, velX/Y/Z, angVel…, uint8 occupantSlot, uint8 stateBits(isDriven/
  brake/broken) }`). The ATV needs ROLL (it tips/flips) which the NPC yaw-only
  snapshot lacks — so its own snapshot (sibling of `PropPosePacket`), one ATV in
  world so no batching needed.
- **Reliable `AtvStatePayload`** (new ReliableKind): `{ WireKey key; uint32
  elementId; float fuel; float health; uint8 brake; uint8 occupantSlot; uint8
  broken; uint8 adopt; }`. On-change + connect-snapshot (adopt=1, trust-gated to
  senderSlot==0 like windows/grime). Health applies damage-gated (only-on-decrease)
  per MTA; fuel/brake/occupant verbatim.
- For MVP treat the one save-placed ATV as always-present; resolve by `Key` in the
  connect-snapshot (no spawn packet), like base windows. (Runtime-crafted ATVs —
  `crafted()` ub 9167 — would later need EntitySpawn.)

**Phasing:** Phase 1 (MVP) = pose stream (kinematic) + occupantSlot->puppet seating +
connect-snapshot by Key (this alone makes the ATV mirror + show the seated driver).
Phase 2 = `AtvStatePayload` fuel/Brake(+setBrake)/health(+updHealth, damage-gated) +
broken/explode edge. Phase 3 = pickup/carry physics-fight suppression
(`setPropProps`/`SetActorRootNotifyRigidBodyCollision`); modules/tires/dirt if asked
(dirt reuses grime keyed-scalar).

**MTA files to cite in the PR/design:** `Client/.../CClientVehicle.{h,cpp}`
(`SetHealth/GetHealth` :220-221, `SetTurnSpeed/SetVelocity` :197, petrol/`CanBe
Damaged` :249-253,319, `m_fHealth` :600); `Server/.../packets/CVehiclePuresync
Packet.cpp` (pos/rot/vel/turnspeed :122-143, damage-gated health :145-171, seat
:107-118); `Server/.../CUnoccupiedVehicleSync.cpp` (single-syncer :59,99,144);
`CVehicleDamageSyncPacket.*` + `CUnoccupiedVehiclePushPacket.*` (discrete edges).

## 5. Gotchas (root-cause notes for the implementer)
- Mounting `Possess(ATV)` unpossesses `mainPlayer_C` (Controller->null) — the mod
  already handles this (the 2026-06-08 quadbike perf fix validated `Registry::Local()`
  by `!IsPuppet` identity). The ATV mirror must NOT be possessed on remotes — it's a
  kinematic orphan like the clump/NPC mirrors. Guard host TickAtvStream by
  `GetController()!=nullptr` discrimination already used everywhere.
- Disable the mirror ATV local physics/Tick so its `applyWheelTorque`/gravity doesn't
  fight the streamed transform (same "mirror tick off" + `notifyRigidBody=false`).
- `health<=0 && speed>4 -> explode()` is a hard transition (debris + destroy);
  replicate as a discrete reliable edge, not by letting each mirror cross zero
  independently (would double-explode). Mirror MTA's `LastSyncedHealth`.
- `Key`@0x0618 is cross-peer stable (save-placed, same save both peers) — verify with
  an install-time keysHash diagnostic (like swinger/window did) in case a crafted ATV
  gets a per-peer NewGuid.

File-size note: all proposed files are new + small; nothing pushed past the 800-LOC
soft cap (work lands in `ue_wrap/atv` + `coop/atv_sync` + `coop/element/atv`, not
`harness.cpp`/`npc_sync.cpp`). `protocol.h` is a constants header (cap-exempt).

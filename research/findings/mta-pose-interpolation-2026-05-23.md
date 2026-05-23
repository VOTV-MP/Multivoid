# MTA:SA receiver-side pose interpolation -- and what we mirror for VOTV

Snapshot 2026-05-23. Per the `[[feedback-check-mta-and-document]]` rule:
before designing a coop feature, read how MTA does it (`reference/mtasa-blue/`)
and write down the pattern + the shape we ported.

## What MTA does

MTA's remote-player smoothing is **per-entity, single-target, linear LERP with
cached error and adaptive snap**. Not a render-delay buffer; not Hermite/SLERP.

### State (per entity, in `CClientPed::m_interp`)

`Client/mods/deathmatch/logic/CClientPed.h:752-769`:

```c
struct { CVector vecTarget;      // last received target position
         CVector vecError;       // (target - position at packet arrival)
         unsigned long ulStartTime, ulFinishTime;
         float    fLastAlpha; }   pos;
// rotation: m_fBeginRotation, m_fTargetRotationA, m_ulBeginRotationTime, fDeltaTime
```

One target only. A new sync packet overwrites it; there's no history ring.

### Open a window on each packet

`CPedSync.cpp:240` -- on every `PED_SYNC` packet:
```c
pPed->SetTargetPosition(vecPosition, PED_SYNC_RATE);  // 400 ms (their ped sync rate)
```
`SetTargetPosition` computes `vecError = target - currentPosition`, sets
`ulStartTime = now`, `ulFinishTime = now + duration`, `fLastAlpha = 0`.

### Apply per frame (`CClientPed::UpdateTargetPosition`, called from `StreamedInPulse`)

`CClientPed.cpp:5450-5460`:
```c
float fAlpha = Unlerp(start, now, finish);
fAlpha       = Clamp(0.f, fAlpha, 1.f);
float dAlpha = fAlpha - m_interp.pos.fLastAlpha;
m_interp.pos.fLastAlpha = fAlpha;
CVector vecCompensation = Lerp(CVector(), dAlpha, m_interp.pos.vecError);
CVector vecNewPosition  = vecCurrentPosition + vecCompensation;
```

Linear: `compensation = errorCached * dAlpha`. Recomputing `(target - cur)` each
frame would be geometric decay, NOT what they do.

### Rotation: shortest-arc linear LERP

`CClientPed.cpp:3450-3469` (ped) / `CClientVehicle.cpp:3841-3846` (vehicle):
- Compute the angle delta with wrap handling (`GetOffsetRadians` / `GetOffsetDegrees`):
  `d = fmod(to - from, 360); if (d > 180) d -= 360; if (d < -180) d += 360;`
- LERP across the same window using that delta.
- Vehicles do per-axis (pitch/yaw/roll); peds yaw only.
- No SLERP.

### Snap on real teleports

`CClientPed.cpp:5473-5478`:
```c
fThreshold = (PED_INTERPOLATION_WARP_THRESHOLD                       // 5 units
            + PED_INTERPOLATION_WARP_THRESHOLD_FOR_SPEED * speed)    // 5 per unit speed
            * gameSpeed * tickRate / 100;
```
If the cached error exceeds the threshold, abort the interp window and snap to
target. Velocity-scaled threshold: a fast-moving entity gets a larger "legal"
budget per packet, so jitter at speed doesn't false-positive into a snap.

### Extrapolation

**Peds: none.** When the window expires (alpha=1) the entity freezes at target
until the next packet rebases. They explicitly chose the freeze artifact (one
frame of no motion when a packet is late) over the prediction-error artifact
(extrapolating past the actual peer position then snapping back).

Vehicles: optional velocity-based extrapolation gated by server settings
(`CClientVehicle.cpp:3785-3801`). Default off; on it adds `velocity * tunedMs`
to the cached error after each packet.

## What we ported (VOTV: `src/votv-coop/{include,src}/coop/remote_player.*`)

Mirrors MTA's ped path with VOTV's units (cm, 30 Hz send rate, no
CharacterMovement on the puppet):

| MTA                                          | VOTV                                            |
| -------------------------------------------- | ----------------------------------------------- |
| `m_interp.pos.{vecTarget,vecError,...}`      | `targetPos_/Yaw_, errorPos_/Yaw_, interpStart/FinishMs_, lastAlpha_, curPos_/Yaw_/Speed_, hasPose_, dirty_` |
| `CPedSync::Packet_PedSync -> SetTargetPosition` | `SetTargetPose(snap)` called by `NetPumpTick` only when `outIsNew=true` |
| `StreamedInPulse -> UpdateTargetPosition`    | `Tick()` called every game-thread tick (60 Hz pump) |
| `PED_SYNC_RATE = 400 ms`                     | `kInterpWindowMs = 50 ms` (1.5x our 30 Hz send interval; covers the typical packet gap with a small render-delay-like smoothing) |
| Linear LERP, cached error                    | Same shape: `cur += errorPos * dAlpha` each frame; **not** `(target - cur) * dAlpha` (would be geometric decay) |
| Shortest-arc rotation w/ `GetOffsetDegrees`  | `OffsetDegrees(from, to)` -- identical formula |
| Velocity-scaled snap threshold               | `kSnapBaseCm = 1000` + `kSnapPerSpeedSec = 0.5` * speed. At sprint ~600 cm/s legal motion in one window is ~30 cm; legitimate teleports (door warps) cross 10+ m -- snap |
| Freeze on alpha=1 (no extrapolation)         | Same: `interpFinishMs_ = 0`, dirty=false, no engine writes |
| Vehicle extrapolation                        | Not ported (VOTV has no vehicles in scope; would belong with the host-authoritative input replication in Phase 4 anyway) |

### What we did NOT take from MTA

- **No render-delay buffer.** MTA doesn't have one (they apply-and-interpolate
  from arrival). The 1.5x-the-send-interval window gives us soft delay implicitly:
  when the next packet arrives, the puppet is still ~30 % short of the previous
  target, so the new packet smoothly rebases.
- **No history ring.** Single target. Replaces on new packet.
- **No SLERP.** Yaw-only shortest-arc linear is enough for the player (VOTV's
  player is upright; pitch/roll don't replicate).
- **No Hermite/Catmull.** Linear matches MTA + is cheaper + has no
  overshoot pathology.

### VOTV-specific add-ons (no MTA equivalent because their entities aren't ours)

- **`hasPose_` first-packet snap.** The puppet is spawned at a fake placement
  (250 cm in front of the local player) so the user sees it pre-handshake. The
  first real pose must snap, not LERP across that fake placement.
- **`ResetPoseState()` on Connected -> not-Connected.** A reconnect after a
  Bye/timeout must snap the first new pose (otherwise the puppet would slide
  across the disconnect-interval distance).
- **`dirty_` flag.** Engine UFunctions (SetActorLocation/Rotation) are not free.
  When the puppet is frozen at target between packets (alpha=1) we skip the
  engine writes -- the SkeletalMeshActor has no physics, so a frozen pose stays
  put without our re-pushing it (post-ship audit H2).
- **60 Hz pump (not 30, not 125).** The window of 50 ms covers ~3 Ticks at
  60 Hz -- smooth without oversampling. 125 Hz pumping starved the engine
  thread of CPU under 2-instance LAN-test contention and dragged story-load
  (observed in the first re-run of `tools/lan-test.ps1`).
- **Spawn-retry backoff (1 s).** The Phase 3 puppet spawn can fail when the
  engine world is mid-transition (OMEGA -> story). The old code retried every
  pump tick; backoff stops the log noise without crutching the engine.

## Result + verification

`[[project-remote-player-interp]]`. Loopback (`netloopback` scenario) + LAN-
two-process (`tools/lan-test.ps1`) both pass with the new path: pose moves
smoothly between packets, snaps on first-pose + on large teleports, freezes
between packets. See [[project-phase3-udp-transport]] and the LAN framework
in `research/findings/two-instance-lan-test-framework-2026-05-23.md`.

## Source pointers (in `reference/mtasa-blue/`)

| File                                                     | Symbol                            |
| -------------------------------------------------------- | --------------------------------- |
| `Client/mods/deathmatch/logic/CClientPed.h:752-769`      | `m_interp` state struct           |
| `Client/mods/deathmatch/logic/CClientPed.cpp:5430-5487`  | `UpdateTargetPosition`            |
| `Client/mods/deathmatch/logic/CClientPed.cpp:3380-3478`  | `SetTargetRotation` + LERP        |
| `Client/mods/deathmatch/logic/CPedSync.cpp:170-258`      | `Packet_PedSync` (packet handler) |
| `Client/mods/deathmatch/logic/CClientVehicle.cpp:3765-3917` | vehicle interp + extrapolation |
| `Shared/mods/deathmatch/logic/CTickRateSettings.h:11-46` | sync rates                        |
| `Shared/mods/deathmatch/logic/Utils.h:228-245`           | `GetOffsetDegrees / Radians`      |
| `Shared/sdk/SharedUtil.Misc.h:518-539`                   | `Lerp / Unlerp / UnlerpClamped`   |

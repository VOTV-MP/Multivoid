// ue_wrap/drone.h -- standalone engine access for the VOTV delivery DRONE (Adrone_C).
// Principle-7 engine-wrapper layer (NO network/coop state). coop::drone_sync drives the
// host-authoritative pose mirror through here.
//
// Adrone_C : AActor (direct) is the SINGLETON delivery drone (one per save, world-placed; NOT
// the player-piloted recon Aprop_rdrone_C). Its flight is a per-tick BP float integrator in
// ReceiveTick (Damping/Speed/Pitch/vecForce vs a world Spline) -- reproducing it bit-exact on a
// remote is fragile, so we HOST-AUTHORITATIVELY stream the resolved actor TRANSFORM and the client
// mirrors it kinematically (SuppressTick stops the client drone's own ReceiveTick so it can't fly
// on its own + fight the stream -- the npc/clump mirror discipline). The drone moves the ACTOR
// (not a physics body), so actor-level GetActorLocation/SetActorLocation is the transform.
//
// Identity = SINGLETON (resolve via FindObjectByClass(drone_C); both peers load the same placed
// drone). It has no top-level Key (its key lives inside Data@0x0410) -- but a singleton needs no
// key. State we read: Active@0x0370 (dormant<->flying).
//
// RE: research/findings/votv-delivery-drone-RE-and-coop-sync-design-2026-06-03.md.

#pragma once

#include "ue_wrap/types.h"  // FVector, FRotator

namespace ue_wrap::drone {

// Resolve Adrone_C + the Active offset. Idempotent; true once resolved. Game thread.
bool EnsureResolved();

// The singleton delivery drone actor, or nullptr if not present. Cached + IsLive-revalidated;
// the underlying FindObjectByClass scan is throttled (the per-frame-scan ban). Game thread.
void* Find();

// Active@0x0370 -- TRUE while the drone is flying a delivery (false = dormant/parked).
bool IsActive(void* drone);

// Root actor world transform (loc + full rotation -- the drone leans/pitches in flight).
bool GetTransform(void* drone, FVector& loc, FRotator& rot);

// CLIENT mirror: snap the drone to the streamed transform. SetActorLocation + SetActorRotation.
bool DriveMirror(void* drone, const FVector& loc, const FRotator& rot);

// CLIENT mirror: stop the drone's own ReceiveTick (its flight integrator) so it can't fly on its
// own + fight the host stream -- the client drone is ALWAYS a mirror. Idempotent at the engine
// level. Game thread.
void SuppressTick(void* drone);

// Restore the drone's ReceiveTick (on disconnect, so single-player flight works again). Game thread.
void RestoreTick(void* drone);

// ---- FX mirroring (Phase 2, 2026-06-09; dust reworked v69 2026-06-12) -------------------------
// The CLIENT suppresses the drone's ReceiveTick, which also kills the tick-driven FX: the
// rotor-wash DUST (eff_droneDust @0x0278) and the delivery "items ready" alarm cue (audio_alarm
// @0x0230 + light_alarm @0x0240). Bytecode RE (research/pak_re/drone_dust_notes.md): the drone's
// tick sphere-traces 2000uu down each tick and (1) SetActive(hit, false) on an IsActive!=hit edge,
// (2) K2_SetWorldLocation's eff_droneDust to the ground hit -- the component is bAbsoluteLocation
// with NO relative offset, so unmoved it sits at WORLD ORIGIN (invisible: frustum-culled fixed
// bounds), and (3) SetFloatParameter('dust', 1 - dist/2000) (a [0,1] RateScale). The PS is
// EmitterLoops=1/20s -- it self-completes and needs the per-tick IsActive!=want re-arm. So the
// HOST streams the FX bits + the dust component's world location in DroneState (20 Hz while
// Active) and the CLIENT replays the BP's exact three calls per packet (ApplyDustMirror). The
// alarm cue fires on the canTakeOff false->true edge (drone within 25cm of its drop). Pose RE:
// votv-delivery-drone-RE-and-coop-sync-design-2026-06-03.md.

// stateBits packing (DroneStatePayload.stateBits): bit0 = dust active, bit1 = canTakeOff (arrived /
// the interaction gate), bit2 = hasSack (cargo aboard / the action-option prerequisite).
inline constexpr uint8_t kFxDust     = 0x01;
inline constexpr uint8_t kFxArrived  = 0x02;
inline constexpr uint8_t kFxHasSack  = 0x04;

// HOST: read the synced drone state into the packed stateBits byte (kFxDust|kFxArrived|kFxHasSack).
// Reads eff_droneDust.IsActive() + canTakeOff + hasSack. Game thread.
uint8_t ReadFxBits(void* drone);

// HOST: read eff_droneDust's world location (the BP pins it to its ground-trace hit per tick) for
// the DroneState stream. False if the FX refs / the component are unavailable. Game thread.
bool ReadDustAnchor(void* drone, FVector& out);

// CLIENT mirror: replay the BP's per-tick dust update from the streamed state -- SetActive on an
// IsActive!=on edge (also re-arms the self-completing 20s system), pin the component to `anchor`,
// write the 'dust' intensity (1 - dist/2000). Call per received DroneState packet. Game thread.
void ApplyDustMirror(void* drone, bool on, const FVector& anchor);

// CLIENT mirror: play the delivery "items ready" alarm cue once (audio_alarm.Activate). Call on the
// canTakeOff rising edge. Game thread.
void PlayArrivalCue(void* drone);

// CLIENT mirror: show/hide the arrival signal light (light_alarm). On while parked-with-cargo. GT.
void SetSignalLight(void* drone, bool on);

// CLIENT mirror: write the interaction-gate fields (canTakeOff@0x0500, hasSack@0x0501) onto the
// mirror drone so a PARKED drone is interactable (else the gate prints "drone is in motion" + offers
// no options -- the suppressed tick never sets them). Game thread.
void WriteGateFields(void* drone, bool canTakeOff, bool hasSack);

// CLIENT mirror: point the mirror drone's container@0x04F8 at the already-prop-mirrored cargo
// container (Aprop_inventoryContainer_drone_C) so openPropInv opens it. Idempotent. Game thread.
void RepointContainer(void* drone);

}  // namespace ue_wrap::drone

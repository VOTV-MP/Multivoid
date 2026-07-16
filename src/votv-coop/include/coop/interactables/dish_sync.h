// coop/dish_sync.h -- L4 (v113): the satellite-dish sync lane.
//
// THE ROOT (measured, votv-dish-impl-RE-2026-07-16.md): every dish slew runs a
// per-peer BP frame-loop with per-slew RNG (start delays, speed), the download
// ARM rolls PER-PEER RNG polarity at initDownloadSignal(-1), and calibration
// has four independent writers -- so dish poses, the armed download's polarity,
// and calibration all DIVERGE across peers. Design of record:
// votv-dish-L4-impl-DESIGN-2026-07-16.md (10-round /qf, 2026-07-16).
//
// The shape:
//  - CLIENT dish sim PARKED: ticker_disher (ClearTimer) + ticker_dishUncalib
//    (TickEnabled false) killed with paired restores on the teardown fanout;
//    the client's own unpreventable ping slews are killed-with-cleanup by the
//    catch detector (signal_catch_sync calls KillOwnPingSlews right after the
//    catch payload is sent).
//  - HOST streams DishPose (unreliable 39): movers-only rows at 4 Hz + a
//    settle-tail (full-24 sweeps 1 Hz x3 after MovingCount hits 0). ONE
//    applier (stream rows AND the DishSnapshot=100 join seed): wire-shadow ->
//    K2_SetRelativeRotation both axes -> raw isMoving -> activeDishes -> cue
//    edges. Guard: skip a dish whose LOCAL loop is live (own-ping pre-kill
//    window: local isMoving && !shadow).
//  - ARM axis has ONE author: the host's 4 Hz raw poll (mesh EDGE | DL signal
//    FName change | polarity change) -> DishArm=99 {armed, decoded, polarity};
//    client apply = pre-clear mirrored moving state -> reflected
//    checkFordDishes() (the native display tail) -> ArmDownloadFromSignal with
//    the HOST polarity. Disarm -> ResetDownloadMachine + deleteSignalActor.
//    (The v70 pending-adopt + the joiner direct-arm are RETIRED -- RULE 2.)
//  - CALIBRATION is symmetric: 1 Hz diff-poll on every peer; local changes
//    broadcast as absolute-value batches (DishCalib=101); host relay = total
//    order; apply+prime GT-atomic (echo-proof).
//
// One concept = one folder: lives with the other desk/device interactables.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dish_sync {

void Install(coop::net::Session* session);

// Game thread, per pump tick. HOST: the 4 Hz pose sweep (+ settle tail) + the
// 4 Hz arm poll. CLIENT: drain + apply DishPose batches; the 1 Hz park latch
// (tickers + the cue reconciler). ALL peers: the 1 Hz calibration diff-poll.
void Tick();

// Reliable appliers (event_dispatch_state).
void OnDishArm(const coop::net::DishArmPayload& p, uint8_t senderSlot);
void OnDishSnapshot(const coop::net::DishSnapshotPayload& p, uint8_t senderSlot);
void OnDishCalib(const coop::net::DishCalibPayload& p, uint8_t senderSlot);

// HOST: the joiner's connect-replay rows -- DishSnapshot (poses/calibration/
// activeDishes) and, when the host machine is armed, a DishArm row (AFTER the
// desk rows + the kind=0 catch row on the same ordered lane).
void QueueConnectBroadcastForSlot(int peerSlot);

// CLIENT, called by signal_catch_sync IMMEDIATELY AFTER the catch payload is
// sent: kill the client's own unpreventable ping slews -- for every dish where
// local isMoving && !wire-shadow: reflected stop() + deactivate both satellite
// cues + activeDishes[i]=false. (The ordering -- payload first, kill second --
// is what keeps ReadSlewFromMovingDish able to see a moving dish.)
void KillOwnPingSlews();

// Teardown fanout: the wire-residue sweep (clear OUR mirrored isMoving/
// activeDishes/cues on every shadow-true dish) THEN the ticker restores
// (disher = PE ReceiveBeginPlay, uncalib = TickEnabled true), then module
// state reset.
void OnDisconnect();

}  // namespace coop::dish_sync

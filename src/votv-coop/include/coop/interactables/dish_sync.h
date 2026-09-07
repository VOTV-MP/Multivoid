// coop/interactables/dish_sync.h -- the satellite-dish sync lane.
//
// THE ROOT, measured: every dish slew runs a per-peer Blueprint frame loop with per-slew RNG
// in its start delays and speed; the download ARM rolls per-peer RNG polarity when it
// initialises the signal; and calibration has four independent writers. So dish poses, an
// armed download's polarity and the calibration all DIVERGE across peers on their own.
//
// So each axis gets ONE author. POSES: a client's dish simulation is PARKED -- both its
// tickers stopped, with paired restores on the teardown fanout -- and the host streams
// movers-only rows plus a settle tail, which one applier drives kinematically through an
// interpolation window, so the stream's rate is not visible as stepping. That applier serves
// the stream rows and the join seed alike, and skips a dish whose own local loop is still
// live. ARM: the host's raw poll is the only author, and a client applies the host's polarity
// rather than rolling its own. CALIBRATION is symmetric instead, having no RNG to diverge on:
// every peer diff-polls, broadcasts absolute values, and the host relay gives them one order.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dish_sync {

void Install(coop::net::Session* session);

// Game thread, per pump tick. HOST: the 4 Hz pose sweep (+ settle tail) + the
// 4 Hz arm poll. CLIENT: drain + apply DishPose batches and drive the LerpWindow
// mirror interp; the 1 Hz park latch (tickers + the cue reconciler). ALL peers:
// the 1 Hz calibration diff-poll.
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

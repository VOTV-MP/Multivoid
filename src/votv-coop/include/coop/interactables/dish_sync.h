// coop/interactables/dish_sync.h -- the satellite-dish sync lane.
//
// THE ROOT, measured: every dish slew runs a per-peer Blueprint frame loop with per-slew RNG
// in its start delays and speed; the download ARM rolls per-peer RNG polarity when it
// initialises the signal; and calibration has random writers on every peer. So dish poses, an
// armed download's polarity and the calibration all DIVERGE across peers on their own.
//
// So each axis gets ONE author. POSES: a client's dish simulation is PARKED -- its
// dish-moving ticker stopped, with a paired restore on the teardown fanout -- and the host streams
// movers-only rows plus a settle tail, which one applier drives kinematically through an
// interpolation window, so the stream's rate is not visible as stepping, for the stream rows and
// the join seed alike; a client's ping never succeeds on its own machine (desk_ping_sync). ARM:
// the download machine's arm and reset are coop/interactables/download_arm_sync's, whose replay
// settles the mirror here first (SettleForArm). CALIBRATION: the host authors it too, in a lane
// of its own (coop/interactables/dish_calib_sync), polled from this lane's slow tick, with its own join seed.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::dish_sync {

void Install(coop::net::Session* session);

// Game thread, per pump tick. HOST: the 4 Hz pose sweep (+ settle tail). CLIENT: drain + apply DishPose batches and drive the LerpWindow
// mirror interp; the 1 Hz park latch (tickers + the cue reconciler). ALL peers:
// the 1 Hz calibration poll (dish_calib_sync).
void Tick();

// Reliable applier (event_dispatch_signal).
void OnDishSnapshot(const coop::net::DishSnapshotPayload& p);

// CLIENT, from download_arm_sync's arm replay, before it runs the game's own arm: every dish inactive and every
// mirrored mover settled, as the host's arm proves its dishes are.
void SettleForArm();

// HOST: the joiner's connect-replay row, DishSnapshot (poses and activeDishes).
void QueueConnectBroadcastForSlot(int peerSlot);

// Teardown fanout: the wire-residue sweep (clear OUR mirrored isMoving/
// activeDishes/cues on every shadow-true dish) THEN the ticker restore
// (disher = PE ReceiveBeginPlay), then module
// state reset.
void OnDisconnect();

}  // namespace coop::dish_sync

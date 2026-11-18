// coop/interactables/oven_progress_sync.h -- the kitchen oven's repair steps before its wiring
// (OvenProgressState): the four screws and the lid, kept the same on every peer.
//
// The repair runs on a screen each peer owns (ue_wrap/devices/oven_screen.h). A screw taken out and
// the lid taken off are bools nothing sets back, so the lane is a merge: a peer whose screen gained
// a bit sends its mask, the host relays a client's, and a receiver raises on its own screen every
// bit it lacks. The wiring under the lid is drawn at random per screen and stays each peer's own;
// the repair it ends in crosses as OvenRepairState (coop/interactables/interactable_sync). The save
// carries no step, so a joiner's screen starts closed and the host's connect replay sends it the
// host's mask. docs/devices.md carries the lane.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::oven_progress_sync {

// Idempotent: caches `session`.
void Install(coop::net::Session* session);

// Per net-pump pass: the ovens of this world are found, and a mask that arrived before its oven was
// found is applied. A screen's own steps are not polled: the game calls its upd() right after each
// bool it writes, and a script-gate watch on it sends the new mask. Game thread.
void Tick();

// OvenProgressState from the wire (router: event_dispatch_signal.cpp).
void OnProgress(const coop::net::OvenProgressStatePayload& p, uint8_t senderSlot);

// HOST: ship every oven's mask that has a bit to a joiner (connect replay).
void QueueConnectBroadcastForSlot(int slot);

// How many bits this peer has raised on a screen from the wire. Game thread.
uint64_t BitsRaised();

// Full state reset. Wired into the subsystems teardown fanout.
void OnDisconnect();

}  // namespace coop::oven_progress_sync

// coop/interactables/oven_progress_sync.h -- the kitchen oven's repair steps before its wiring
// (OvenProgressState): the four screws and the lid, kept the same on every peer.
//
// The repair runs on a screen each peer owns (ue_wrap/devices/oven_screen.h). A screw taken out and
// the lid taken off are bools nothing sets back, so the lane is a merge: a peer whose screen gained
// a bit sends its mask, the host judges a client's (coop/interactables/oven_repair_judge) and sends what
// it grants to the other clients, and a receiver raises on its own screen every bit it lacks. The wiring under the lid is drawn at random per screen and stays each peer's own;
// the repair it ends in crosses as OvenRepairState (coop/interactables/interactable_sync). The save
// carries no step, so a joiner's screen starts closed and the host's connect replay sends it the
// host's record of the steps. docs/devices.md carries the lane.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>
#include <string>

namespace coop::net { class Session; }

namespace coop::oven_progress_sync {

// Idempotent: caches `session`.
void Install(coop::net::Session* session);

// Per net-pump pass: settles the watch on the repair screen's upd(). Nothing is polled. The game calls
// upd() right after each bool it writes, and the watch sends the new mask; the ovens are the oven
// lane's index (coop/interactables/interactable_sync), whose rebuild places a mask that arrived before
// its oven was indexed. Game thread.
void Tick();

// OvenProgressState from the wire (router: event_dispatch_signal.cpp), as a CLIENT takes it: the host's record. On the host a
// client's mask is routed to the judge instead (coop/interactables/oven_repair_judge), never to this.
void OnProgress(const coop::net::OvenProgressStatePayload& p, uint8_t senderSlot);

// HOST: ship the record of every unrepaired oven that holds a step, its screen's bits included, to a joiner (connect replay).
void QueueConnectBroadcastForSlot(int slot);

// The steps this peer's record holds for the oven under `key`: what it sent, took or granted, which the save does not
// carry. 0 for a key it has never held. The host's judge reads its own record, never the screen, so an oven with no
// screen open still judges (coop/interactables/oven_repair_judge). Game thread.
uint8_t KnownMask(const std::wstring& key);

// HOST: the judge granted `add` (bits the record lacked). The record takes them FIRST, then the host's own screen if the
// oven has one open, and the record's whole mask goes to every client but `exceptSlot`, the asker, whose screen holds
// what it asked. A failed raise on the host's screen still records and sends: the record is the state. Game thread.
void GrantSteps(coop::net::Session& s, void* oven, const std::wstring& key, uint8_t add, uint8_t exceptSlot);

// CLIENT, ahead of a repair ask: the record and the screen's bits as one mask, to the host, so a step the host never took
// reaches it before the ask in the same ordered lane. Sends nothing when no step is held. Game thread.
void ResendCumulative(coop::net::Session& s, void* oven, const std::wstring& key);

// How many bits this peer has raised on a screen from the wire. Game thread.
uint64_t BitsRaised();

// How many masks from the wire carried a bit the receiving screen lacked, counted where the screen is
// read and before any bit is raised: the arrival event a drill judges on. Game thread.
uint64_t MasksArrived();

// Full state reset. Wired into the subsystems teardown fanout.
void OnDisconnect();

}  // namespace coop::oven_progress_sync

// coop/interactables/desk_panel_sync.h -- the desk's back panels (DeskPanelState): which are bolted and
// which are on the desk, kept the same on every peer.
//
// A wrench unbolts a panel, E takes an unbolted panel off as a prop in the hand, and the prop carried
// back into its place puts it on again (ue_wrap/desk/desk_panels.h). Each writes one of two bool
// arrays on the peer whose player did it, so a peer compares the arrays with what it last knew on
// every pass and at the end of the desk's E press, where a take-off's edit must leave before the panel
// prop's birth. The host's arrays are the canonical: a client sends each changed bit to the host,
// which applies what its own desk allows and broadcasts both masks, a refused edit going back to its
// author with them. Two take-offs of one panel can cross: the host keeps the first, denies the second
// by name and drops its panel prop's birth, and the denied peer destroys the prop its press spawned. One
// desk puts a panel back: its carrier's, or the host's when nobody carries it; the overlap is refused on
// every other copy, and a peer that takes a mask with a panel gone raises the desk's panelTakeOff, so
// the copy born inside the lid does not re-attach. A joiner's desk comes from the save and the connect
// replay's canonical. docs/signals.md carries the lane.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::desk_panel_sync {

// Idempotent: caches `session` and registers the watches on the desk's E press and its overlap, once.
void Install(coop::net::Session* session);

// Per net-pump pass: a canonical that arrived before the desk resolved is applied once it has, and
// the arrays are compared with what this peer last knew. Game thread.
void Tick();

// DeskPanelState from the wire (router: event_dispatch_signal.cpp).
void OnDeskPanel(const coop::net::DeskPanelStatePayload& p, uint8_t senderSlot);

// HOST: ship the canonical masks to a joiner (connect replay).
void QueueConnectBroadcastForSlot(int slot);

// HOST consult from the client fresh-birth author (prop_drop_intent): true, once, for the panel prop
// of a take-off this host refused from `senderSlot`, whose birth is then dropped.
bool HostShouldReapPanelBirth(uint8_t senderSlot, void* panelClass);

// How many canonical mask pairs this peer has written into its desk, and how many of its own
// take-offs the host denied. Game thread.
uint64_t CanonicalsAdopted();
uint64_t TakeOffsDenied();

// Full state reset. Wired into the subsystems teardown fanout.
void OnDisconnect();

}  // namespace coop::desk_panel_sync

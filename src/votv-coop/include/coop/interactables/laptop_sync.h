// coop/interactables/laptop_sync.h -- the stationary PC's (Alaptop_C) power, and the LaptopState wire the portable
// PC's lid (coop/interactables/portable_pc_lid) rides as op 6.
//
// The edge is authored by the presser, the host re-fans it, and a joiner gets the host's target rather than a
// replayed history. The PC's disc slot is coop/interactables/floppy_slot_sync's, its file quad
// coop/interactables/laptop_buffer_sync's. Game thread throughout, on the net-pump tick and the reliable dispatch.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::laptop_sync {

void Install(coop::net::Session* session);

// 4 Hz: resolve, the power target, and the power edge poll: the isOpened edge, polled because every entry verb is
// EX-invisible. A receiver replays the native actionOptionIndex(b8) when local differs from wire, once no toggle runs,
// and the edge that ends its own replay is consumed rather than sent as its own; one whose powered gate declines
// retries until it converges, and coop/world/power_panel converges the wall-power input (every peer's setPower runs
// from the host's canonical).
// The portable PC (prop_portablePc_C) is a remote terminal to THIS laptop: its lid is
// coop/interactables/portable_pc_lid's, on op 6.
void Tick();

// Wire ingest (both roles). A client applies the host's lines. The HOST keeps each client's newest power edge, applies
// a changed one at a pace and re-fans it (except to its origin), and refuses op 3, its own state for a joiner, from a
// client. Op 6 goes to the lid lane.
void OnLaptopState(const coop::net::LaptopStatePayload& p, uint8_t senderSlot);

// Each event_feed update after the reconcile, game thread: the lane's refusal window flushed on both roles; on the
// HOST, a client's edge owed by its pace applied once a token refills, unless the target already holds it.
void PayOwed(coop::net::Session& session);

// HOST: ship the joiner the laptop's target (op=3): the wire's while one is pending, else the laptop's power. The lid
// lane sends its own rows.
void QueueConnectBroadcastForSlot(int peerSlot);

// Instruments (the world_latest drill, coop/dev/hostile_drill_world), game thread: how many of a client's lines the
// host applied from `slot`; how many edges this peer broadcast as its own; this peer's laptop power; and the
// target this peer converges on (the wire's while one is pending, else the laptop's own). False when unread.
// Settled: no toggle runs or waits to be judged, no target is pending and the lane's poll has seen the laptop's
// current state; `opened` is that state.
uint32_t ClientEdgesApplied(int slot);
uint32_t OrganicEdgesSent();
bool ReadLocalOpened(bool& out);
bool Target(bool& out);
bool Settled(bool& opened);

void OnDisconnect();

}  // namespace coop::laptop_sync

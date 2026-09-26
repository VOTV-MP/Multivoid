// coop/interactables/portable_pc_lid.h -- the portable PC's lid (Aprop_portablePc_C::opened), on
// ReliableKind::LaptopState op 6. Gameplay/network layer (principle 7): it reaches the engine only
// through ue_wrap::portable_pc, and laptop_sync, which owns the wire kind, hands it the op-6 lines.
//
// The lid has one writer, the PC's own `open`: the PC's actions 10 and 11 run it, and so does the
// top's action 11 on its PC; nothing else sets `opened`, and the save does not write it, so every copy
// loads closed. A class-scoped watch on `open` reads the lid at the body's entry and sends the change
// at its exit, from whichever peer ran it. The host applies a client's line and sends it to every
// client, its author too, so every peer ends at the host's order of lines. A receiver runs the same
// `open` inside the lane's own scope, which sends nothing. A line for a PC this peer has not enrolled
// yet waits, keyed by its eid, for its birth to land, and does not age while this peer's join is still
// streaming the world in; an edge on a PC no element names yet waits by the actor until the element
// lane names it. A joiner gets every open lid at its world-ready. Game thread throughout.

#pragma once

#include "coop/net/protocol.h"

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::portable_pc_lid {

void Install(coop::net::Session* session);

// Per tick: the watch until it is live, then the waiting lines and held edges once a second while any
// waits.
void Tick();

// Wire ingest: an op-6 LaptopState line.
void OnLid(const coop::net::LaptopStatePayload& p, uint8_t senderSlot);

// HOST: every open lid to a joiner at its world-ready (ConnectReplayForSlot).
void QueueConnectBroadcastForSlot(int peerSlot);

void OnDisconnect();

// [dev] The lid lines this peer authored (its own `open` bodies) this session, and the ones it applied
// from the wire, for the lid drill's echo check. Game thread.
struct Counts { uint64_t sent; uint64_t applied; };
Counts LineCounts();

}  // namespace coop::portable_pc_lid

// coop/dispatch/event_dispatch.h -- INTERNAL per-domain reliable-message handlers for
// coop::event_feed::Update's dispatch switch. The case BODIES live in the per-domain files; the
// exhaustive switch itself stays in event_feed.cpp, so the kind->handler table is greppable in one
// place.
//
// Sibling-internal header, the coop/net/session_lanes.h shape: not part of the public coop/ include
// surface, and only event_feed.cpp and the event_dispatch_*.cpp translation units include it.
//
// Each Handle* re-switches on msg.kind and carries the full case body -- payload-length check,
// trust-boundary validation (sender slot and role gates, finite and bounds checks, eid range), then
// the module dispatch -- and returns true when msg.kind is in its family, whether it processed the
// message or dropped it in validation. event_feed's drain chains them, and a kind no family claims
// falls to the unknown-kind log. That makes each family's own switch the SINGLE membership
// declaration, so a new kind is the enum plus one case; no list of kinds is repeated here, because
// a second list is the wiring hazard the split removed.

#pragma once

#include "coop/net/session.h"

#include <cstdint>

namespace coop::event_feed {

// Role-range validation for an inbound eid-carrying packet (see the definition's comment for the
// full trust model). Shared by the entity family (ItemActivate) and the world family (RedSky,
// LightningStrike, WeatherState). Defined in event_dispatch_world.cpp.
bool VerifySenderEidRange(int senderPeerSlot, uint32_t senderElementId,
                          const char* kind);

// Entity lifecycle and held-item family: prop and world-actor spawn, destroy and convert,
// owner-entity mirroring, position snaps, and item activation. `localPlayer` threads into
// remote_prop for held-grab release and destroy safety.
bool HandleEntityEvent(net::Session& session,
                       const net::Session::ReliableMessage& msg,
                       void* localPlayer);

// Keyed device-state family: the mirrored state of doors, lights, lockers, containers, appliances,
// keypads, power, the ATV, drones, turbines, cleanliness and grime, plus the per-device claim,
// sleep, email, inventory and voice rows. `localPlayer` threads into the KerfurConvert client apply
// (prop teardown and materialize).
bool HandleStateEvent(net::Session& session,
                      const net::Session::ReliableMessage& msg,
                      void* localPlayer);

// Signal-pipeline family: the sky-signal chain, the laptop and its blobs, the dish and its aim, the
// play deck and drive slots, the rack, the desk with its input, log and scans, the meadow store,
// and the saved-signal list.
bool HandleSignalEvent(net::Session& session,
                       const net::Session::ReliableMessage& msg);

// CLIENT->HOST intent and request family: a client asks the host to perform something it alone is
// authoritative for. Most cases gate on role()==Host plus a client sender slot before handing the
// request to the authoritative module, but not all of them, so read the case: CoinGunResult and
// OrderRefused are HOST->CLIENT answers that drop on the host; OrderRequest, CoinGunSell and
// CoinCollect check the slot here and leave the role gate to their module; RoachConsumed defers
// both to roach_sync::OnConsumedIntent.
bool HandleIntentEvent(net::Session& session,
                       const net::Session::ReliableMessage& msg,
                       void* localPlayer);

// Ambient and world-event family: fireflies, scheduled events with their cues and snapshots,
// alarms, the server rack, roaches, inventory pickups, chat, and the time, sky and weather stream.
bool HandleWorldEvent(net::Session& session,
                      const net::Session::ReliableMessage& msg);

}  // namespace coop::event_feed

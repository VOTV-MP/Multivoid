// coop/dispatch/event_dispatch.h -- INTERNAL per-domain reliable-message handlers for
// coop::event_feed::Update's dispatch switch. event_feed.cpp handles the session and handshake
// kinds inline and chains these seven families from its default case; each family's own switch is
// where its kinds are declared.
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
// sleep, email, inventory and voice rows, the PC user's tab, and the host's server-scope settings. `localPlayer`
// threads into the KerfurConvert client apply (prop teardown and materialize).
bool HandleStateEvent(net::Session& session,
                      const net::Session::ReliableMessage& msg,
                      void* localPlayer);

// Signal-pipeline family: the sky-signal chain, the laptop with its blobs and floppy box, the dish
// and its aim, the play deck, the drive slots and their rack, the desk with its input, log, scans
// and physics mods, the refiner's decode pane, the task board, the meadow store and the
// saved-signal list.
bool HandleSignalEvent(const net::Session::ReliableMessage& msg);

// CLIENT->HOST intent and request family: a client asks the host to perform something it alone is
// authoritative for. Read the case for its gate rather than assuming one. GrabIntent, ThrowIntent,
// PropDropIntent, ReelEjectIntent, DoorVerbIntent, CameraToggleIntent, LaptopTabReport and KeypadIntent check role()==Host AND a client
// sender slot; KerfurConvertRequest and KerfurCommand check the role only, mapping an out-of-range
// slot to the 0xFF sentinel and handing it on; RoachConsumed defers both gates to
// roach_sync::OnConsumedIntent.
bool HandleIntentEvent(net::Session& session,
                       const net::Session::ReliableMessage& msg);

// Economy family: the delivery order with its queue, the coin gun's sale and collect, the laptop
// upgrade purchase and the point sack. OrderRequest, CoinGunSell and CoinCollect check the slot here
// and leave the role gate to the module they call; UpgradeIntent and PointSackRedeem check role()==Host
// AND a client sender slot; CoinGunResult and OrderRefused are HOST->CLIENT answers that drop on the
// host; OrderQueue, the host's delivery queue mirrored to clients, leaves both checks to
// order_queue_sync. `localPlayer` is the host's own player, passed into a collected coin.
bool HandleEconomyEvent(net::Session& session,
                        const net::Session::ReliableMessage& msg,
                        void* localPlayer);

// Driven-prop family: the close of a driven prop's stream (PropDriveEnd), and the prop syncer's claim
// and answer. PropDriveEnd on the host is a syncing client's End, handed to the claim judge; on a client
// it is the host's End, applied by the stream's receiver. PropSyncClaim checks role()==Host AND a client
// sender slot; PropSyncAnswer is the HOST->CLIENT verdict, refused from a client at the host's edge. DevLoadFiller
// is the developer filler of the syncer drill's load arm, host to client; DevParityReport is the pile deferral
// drill's report, client to host; DevContainerParity is the container join drill's.
bool HandlePropDriveEvent(net::Session& session,
                          const net::Session::ReliableMessage& msg);

// Ambient and world-event family: fireflies, scheduled events with their cues and snapshots,
// alarms, the server box, roaches, inventory pickups, chat, and the time, sky and weather stream.
bool HandleWorldEvent(const net::Session::ReliableMessage& msg);

}  // namespace coop::event_feed

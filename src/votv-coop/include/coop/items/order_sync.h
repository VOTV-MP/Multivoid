// coop/order_sync.h -- delivery-drone ECONOMY: client->host shop-order forward (protocol v49).
//
// Gameplay/network layer (principle 7): owns the OrderRequest wire (chunked serialize/assemble),
// the client poll-and-forward policy, and the host re-commit. Talks to the engine ONLY through
// ue_wrap::order_economy.
//
// MODEL (RULE 1 / host-authoritative economy). VOTV has NO UE replication, so a CLIENT's laptop
// order is 100% client-LOCAL (makeAnOrder Array_Adds into the CLIENT's own saveSlot.orders + flies
// the CLIENT's mirror drone). The commit verb is BP-internal (unobservable), so the client POLLS its
// saveSlot.orders.Num each net-pump tick; on an increment it serializes the new order (each item's
// `object` class NAME + price/size/category + time), CHUNKS it across reliable datagrams
// (kMaxReliablePayload=228 B), forwards to the host, then RESETS its mirror drone (so its locally-run
// sendShop can't fake a takeoff). The HOST assembles the chunks per (senderSlot, orderId) and
// re-commits via the native Uui_laptop_C::makeAnOrder(order, automatic=true) -- the proven
// commit+deliver+native-drain path. The delivered cargo box (Aprop_C) then rides the EXISTING prop
// pipeline + the drone body rides DroneState; nothing else is on the wire.
//
// RE: research/findings/votv-delivery-drone-RE-and-coop-sync-design-2026-06-03.md (ECONOMY BUILD
// PLAN + the 5-question commit/remove/charge RE).

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::order_sync {

// Store the session pointer + reset state. Game thread.
void Install(coop::net::Session* session);

// Per-tick pump (net-pump, game thread):
//   CLIENT -> poll saveSlot.orders.Num; forward each new order; quiet the mirror drone.
//   HOST   -> retry pending completed-order commits + evict stale partial assemblies.
void Tick();

// Receiver entry (HOST ingest): an OrderRequest chunk arrived from `senderSlot` (a client). The
// payload is the variable-length OrderRequestHeader + packed items (range-checked here). Assembles
// per (senderSlot, orderId); a completed order is queued for commit in Tick. Called from event_feed's
// reliable drain loop (game thread). A client receiving this (shouldn't happen) no-ops.
void OnReliable(const void* payload, int len, uint8_t senderSlot);

// Session teardown: reset the client watermark + drop host assembly/commit queues. Game thread.
void OnDisconnect();

}  // namespace coop::order_sync

// coop/items/order_sync.h -- delivery-drone ECONOMY: the client-to-host shop-order forward.
//
// Gameplay and network layer (principle 7): it owns the OrderRequest wire, chunked serialize and
// assemble, the client's poll-and-forward policy and the host's re-commit, and reaches the
// engine only through ue_wrap::order_economy.
//
// MODEL: the economy is host-authoritative. VOTV has no engine replication, so a CLIENT's laptop
// order is entirely client-local -- makeAnOrder appends to the CLIENT's own saveSlot.orders and
// flies the CLIENT's mirror drone. The commit verb is blueprint-internal and unobservable, so
// the client polls its saveSlot.orders count each net-pump tick; on an increment it serializes
// the new order (each item's `object` class NAME, price, size, category and time), chunks it
// across reliable datagrams of kMaxReliablePayload, forwards it, then RESETS its mirror drone,
// so its locally-run sendShop cannot fake a takeoff. The HOST assembles the chunks per
// (senderSlot, orderId) and re-commits through Uui_laptop_C::makeAnOrder(order, automatic=true).
// The delivered cargo box rides the existing prop pipeline and the drone body rides DroneState.

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

// Receiver entry (HOST ingest): an OrderRequest chunk arrived from `senderSlot`, a client. The
// payload is the variable-length OrderRequestHeader plus packed items, range-checked here.
// Assembles per (senderSlot, orderId); a completed order is queued for commit in Tick. Called
// from event_feed's reliable drain loop, on the game thread. A client receiving this (it
// should not) no-ops.
void OnReliable(const void* payload, int len, uint8_t senderSlot);

// Receiver entry (CLIENT ingest): the HOST refused a forwarded order (OrderRefused). Renders
// one feed line naming the reason and puts the refused items back in the laptop cart, because
// the base game's own affordability gate pops BEFORE it clears the cart -- a refused purchase
// normally leaves the cart intact, while our refusal arrives after the local run has already
// cleared it. The balance correction rides a direct BalanceSync from the host, not this
// message. A host receiving this (it should not) no-ops. Game thread.
void OnReliableRefused(const void* payload, int len);

// Session teardown: reset the client watermark + drop host assembly/commit queues. Game thread.
void OnDisconnect();

}  // namespace coop::order_sync

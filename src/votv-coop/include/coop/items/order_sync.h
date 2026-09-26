// coop/items/order_sync.h -- delivery-drone ECONOMY: the client-to-host shop-order forward.
//
// Gameplay and network layer (principle 7): it owns the OrderRequest wire, chunked serialize and
// assemble, the client's forward at the order gate and the host's re-commit, and reaches the engine only
// through ue_wrap::order_economy.
//
// MODEL: the economy is host-authoritative. VOTV has no engine replication, so a CLIENT's laptop order
// would be entirely client-local: makeAnOrder appends to the client's own saveSlot.orders and sends the
// client's drone. At the script gate, on a client, makeAnOrder's entry reads the order the player placed
// from its own parameter and sends its row names to the host, chunked across reliable datagrams of
// kMaxReliablePayload; the addOrderCart it runs is refused (the client's queue is the host's,
// order_queue_sync) and so is the drone's sendShop (the host's drone flies). A world event's automatic
// order is not sent: the host's own copy of the event makes it. The HOST assembles the chunks per
// (senderSlot, orderId) and re-commits through Uui_laptop_C::makeAnOrder(order, automatic=true). The
// cargo box rides the existing prop pipeline and the drone body rides DroneState.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::order_sync {

// Store the session pointer and register the gate's watches (idempotent), the queue mirror's too.
// Game thread.
void Install(coop::net::Session* session);

// Per-tick pump (net-pump, game thread): the HOST retries pending completed-order commits and
// evicts stale partial assemblies; the queue mirror applies what waited on the laptop.
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

// Session teardown: drop the client's in-flight orders, the host's assemblies and commit queues,
// and the queue mirror's state. Game thread.
void OnDisconnect();

}  // namespace coop::order_sync

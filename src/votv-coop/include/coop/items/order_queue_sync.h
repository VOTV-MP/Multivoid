// coop/items/order_queue_sync.h -- the delivery order queue (saveSlot.orders), the host's, mirrored on
// every client (OrderQueue; FIX_PLAN R-G, F-54). Gameplay/network layer (principle 7): it reaches the
// engine only through ue_wrap::order_economy.
//
// The host's queue is the only one. Its own addOrderCart and removeOrderCart are watched at the script
// gate, and each change is sent as it ran -- an order appended at the end, or the first order popped --
// to every peer. A client's own orders never enter its queue (order_sync refuses them at the gate and
// sends them to the host as requests), so a client's queue holds exactly what the host sends, applied
// through its own laptop's addOrderCart and removeOrderCart, which also keep the order slot widgets. A
// client's drone never delivers (drone_sync), so it never popped: before this lane a client's queue
// grew with its own orders and never shrank (runAQ, 2026-09-26).
// JOIN (principle 8): a joiner's world comes from the host's save, whose queue may be stale by the time
// the joiner is ready. At its world-ready the host sends a reset and every queued order; a client applies
// no change before its first reset, since what the host broadcast before it is the snapshot's to state.

#pragma once

#include <cstdint>

namespace coop::net { class Session; }

namespace coop::order_queue_sync {

// Keeps the session and registers the host's verb watches (idempotent). Game thread.
void Install(coop::net::Session* session);

// CLIENT: applies, in order, a change the laptop could not take yet. A single check when none waits.
// Game thread.
void Tick();

// CLIENT: an OrderQueue message from the host (router: event_dispatch_intent.cpp). Game thread.
void OnReliable(const void* payload, int len, uint8_t senderSlot);

// HOST: a reset and every queued order, to a joiner at its world-ready. Game thread.
void QueueConnectBroadcastForSlot(int slot);

// Session teardown. Game thread.
void OnDisconnect();

}  // namespace coop::order_queue_sync

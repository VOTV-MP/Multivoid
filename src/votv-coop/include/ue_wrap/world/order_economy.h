// ue_wrap/world/order_economy.h -- engine access for the laptop shop order queue (the save slot's
// orders, the delivery-drone economy). Engine-wrapper layer: no network or coop state; order_sync drives
// the client-to-host order economy through here. The game has no replication, so a client's order is
// client-local; coop forwards it to the host, which re-commits it here through the same native
// makeAnOrder, the host being the delivery authority. An item on the wire is a `list_store` row name and
// nothing else. The host prices the order from its own copy of the table: makeAnOrder itself charges
// nothing (the charge is in the laptop's order button, before the call), so a client-supplied price could
// never be collected by the host. A class name cannot name an item, since many rows share one object
// class, and the shop stamps the row key into the store struct's name, so a placed order already carries
// the right identity. The commit copies the live table row wholesale: the delivered order box branches on
// the item's object and asProp fields and passes asProp to the player, so a row with those fields blank
// mis-delivers. The one field overwritten is subcategory, stamped with the pinned empty FText (the order
// box's own items ship the same), since copying the live FText would rest on who deep-copies it and when.
// The queue MIRROR is the one place a class does name an item: a world event builds its order outside the
// shop (the daily delivery, a gift) with no row at all, setting only the item's class, so that travels.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::order_economy {

// One order as the wire and the arbiter see it: the row names of its items in cart order,
// multiplicity preserved, and its delivery time. Everything else (price, object, size, category,
// asProp) a peer resolves from its own table; it never travels.
struct OrderData {
    std::vector<std::wstring> rowNames;
    float eta = 0.f;  // Fstruct_storeOrder.time, seconds
};

// The count of orders in the local queue; -1 if unresolved (booting, the menu). The host also
// uses it to confirm a commit: the reflected call reports only that the dispatch happened, and
// a +1 edge across CommitOrder is what proves the order queued. The edge is exact, since
// nothing pops the queue synchronously inside the commit.
int32_t OrderCount();

// One line item of a queued order as the queue mirror carries it: a shop item by its list_store row,
// or an item a world event built outside the shop by its class, with the list_props name its asProp
// holds when the class is the generic prop_C (the daily delivery's reel case). Exactly one of `row` and
// `cls` is set.
struct QueuedItem {
    std::wstring row;     // the list_store key a shop item carries in its name
    std::wstring cls;     // an unnamed item's class, by name
    std::wstring asProp;  // an unnamed item's asProp, empty for None
};

// One queued order as the mirror carries it: every item, and its delivery time.
struct QueuedOrder {
    std::vector<QueuedItem> items;
    float eta = 0.f;  // Fstruct_storeOrder.time, seconds
};

// Read the queue's order at `index`, every item by its row or, with none, by its class; an item
// named neither way is left out and said. False if the queue is unresolved, the index is out of
// range, or the row struct's layout has not resolved (store_catalog::ReadLayout, which owns the row's
// shape and does not wait on the price gate). An order of no items reads true and empty. Game thread.
bool ReadQueuedOrder(int32_t index, QueuedOrder& out);

// Read the order stored at `order`, an Fstruct_storeOrder (makeAnOrder's parameter, say), into
// `out`: its items' row names and its time. False when the catalog is unusable, the order holds no
// items, or none of them carries a row name. Game thread.
bool ReadOrderAt(const void* order, OrderData& out);

// Host: commit `order` as a real delivery through the native makeAnOrder. Every item is looked
// up in store_catalog and the live table row copied wholesale into a heap items buffer the
// native deep-copies (then freed here), with the pinned empty FText stamped over subcategory.
// Returns false, committing nothing, if the catalog is unusable or any row name is unknown: a
// partial order would charge for goods the arbiter could not name. `etaSeconds` is the ETA the
// host rolled (the game's own order button rolls 120 to 180 s); the client's number is not on
// the wire. `automatic` is passed to the native and is not about payment: it gates a branch
// that adds the committing peer's own cart count to a bought-items statistic, so true keeps a
// client's purchase out of the host's stats.
bool CommitOrder(const OrderData& order, float etaSeconds, bool automatic);

// Host: are the actors CommitOrder dereferences present (the drone, the radio tower, the
// laptop, the drone's sell location), so a commit cannot null-fault? A busy drone is fine: the
// native appends to the queue and the drone pops the next order on arrival, so orders queue
// natively. A broken radio tower is fine too; the drone's own send handles it. Only a
// still-loading world must defer. Game thread.
bool CanCommit();

// A queue change as a mirror applies it: Done when the queue moved exactly as asked, Later when it
// could not move yet (the laptop, the queue or the catalog not ready, or the verb ran and moved
// nothing) and the caller should try again.
enum class Applied : uint8_t { Done, Later };

// Client: append `order` to the local queue through the laptop's own addOrderCart, which also adds
// the queue's order slot widget: the host's queue as this peer mirrors it. A shop item is built as
// CommitOrder builds it; an unnamed one in the one shape every world-event builder of the game makes
// (its class, its asProp, one of it, no price, row, category or subcategory). The mirror shows the host's queue
// and delivers nothing, so it is built item by item: one this machine cannot build (a row its
// catalog lacks, a class it has not loaded) is left out and said, down to an order of no items,
// which still takes its place so the host's next pop takes the same order off. Done only when the
// queue grew by exactly one: the call reports its dispatch, not its effect. Game thread.
// `leftOut`, when given, receives how many items were left out. Game thread.
Applied AppendOrder(const QueuedOrder& order, int* leftOut = nullptr);

// Client: pop the local queue's first order through the laptop's own removeOrderCart, which also
// removes its slot widget. Done only when the queue shrank by exactly one; Later on an empty or
// unresolved queue, the caller telling those apart. Game thread.
Applied PopOrder();

// Client: put `rowNames` back into the laptop's cart through the native addStoreCart, once per
// row with the live table row. Used only when the host refuses a forwarded order: single
// player's own affordability gate fires before the cart is cleared, so a refused purchase
// keeps its cart, while a client's order has already run the whole button path locally and
// its cart is gone; restoring it makes the refusal behave as the game does. Best-effort:
// returns the rows re-added; a client with an unusable catalog gets 0 and is still told why by
// the feed line. Game thread.
int32_t RestoreCartItems(const std::vector<std::wstring>& rowNames);

// Client, dev only: place a shop order the way a player does. Run the laptop's own
// generateStore, find the shop slots whose stamped name matches `rowNames`, addStoreCart each,
// then makeAnOrder with the resulting cart. Returns the summed price of the items added (0 on
// failure), so the caller can apply the same local debit the order button applies. It touches
// no store_catalog on purpose: a drill that warms the catalog the production path never built
// proves only itself. Row identity comes from the shop slots the game generated, and the
// struct offsets are resolved off the cart property's inner struct, so afterwards the local
// queue holds exactly what a human purchase leaves and the forward path starts cold. The
// button clears the cart after committing and this does not, so the items stay in the local
// cart; one-shot use. Game thread.
int32_t PlaceOrderFromShopUI(const std::vector<std::wstring>& rowNames, float etaSeconds);

// Host, dev only: queue the day cycle's own daily delivery as the game does at six -- the cycle's
// "Make Default Order", then the laptop's makeAnOrder with automatic set -- for a drill that needs a
// world event's order, whose items carry no row. False, queueing nothing, when CanCommit is not met;
// true when the queue grew by one. Game thread.
bool MakeDailyOrder();

}  // namespace ue_wrap::order_economy

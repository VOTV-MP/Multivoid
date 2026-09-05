// ue_wrap/world/order_economy.h -- engine access for the laptop shop order queue (the save
// slot's orders, the delivery-drone economy). Engine-wrapper layer: no network or coop state;
// order_sync drives the client-to-host order economy through here. The game has no
// replication, so a client's order is client-local; coop forwards it to the host, which
// re-commits it here through the same native makeAnOrder, the host being the delivery
// authority. An item on the wire is a `list_store` row name and nothing else. The host prices
// the order from its own copy of the table: makeAnOrder itself charges nothing (the charge is
// in the laptop's order button, before the call), so a client-supplied price could never be
// collected by the host. A class name cannot name an item, since many rows share one object
// class, and the shop stamps the row key into the store struct's name, so a placed order
// already carries the right identity. The commit copies the live table row wholesale: the
// delivered order box branches on the item's object and asProp fields and passes asProp to
// the player, so a row with those fields blank mis-delivers. The one field overwritten is
// subcategory, stamped with the pinned empty FText (the order box's own items ship the same),
// since copying the live FText would rest on who deep-copies it and when.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::order_economy {

// One order as the wire and the arbiter see it: the row names of its items in cart order,
// multiplicity preserved. Everything else (price, object, size, category, asProp) the host
// resolves from its own table; it never travels.
struct OrderData {
    std::vector<std::wstring> rowNames;
};

// The count of orders in the local queue; -1 if unresolved (booting, the menu). The host also
// uses it to confirm a commit: the reflected call reports only that the dispatch happened, and
// a +1 edge across CommitOrder is what proves the order queued. The edge is exact, since
// nothing pops the queue synchronously inside the commit.
int32_t OrderCount();

// Read one queued order's item row names into `out`. False if unresolved, out of range, the
// order has no items, or the catalog is unusable (the row-name offset comes from
// store_catalog, which owns the row's shape). Game thread.
bool ReadOrder(int32_t index, OrderData& out);

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

// Client: after forwarding a locally-placed order, reset the mirror drone's self-takeoff (the
// local makeAnOrder set this peer's drone active with an order) so it cannot fake a local
// flight; the drone stays a pure host-driven mirror. Writes the empty-queue rest state. Safe if
// the send never ran; false if the drone is unresolvable. Game thread. Not undone: the local
// send also stores the drone's order (inert, since the client drone's tick is suppressed) and,
// with a broken radio tower, adds an email locally, which the host also does on commit, a
// duplicate-email path not suppressible from here.
bool QuietLocalDrone();

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

}  // namespace ue_wrap::order_economy

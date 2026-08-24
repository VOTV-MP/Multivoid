// ue_wrap/order_economy.h -- standalone engine access for the laptop shop ORDER QUEUE
// (saveSlot.orders, the delivery-drone economy). Principle-7 engine-wrapper layer: NO
// network/coop state. coop::order_sync drives the client->host order economy through here.
//
// The order data path is bytecode-verified (votv-delivery-drone-RE-and-coop-sync-design-
// 2026-06-03.md): Uui_laptop_C::makeAnOrder(Fstruct_storeOrder, bool automatic) -> addOrderCart
// Array_Adds into AmainGamemode_C.saveSlot.orders @0x0490 (the ONLY persistent order writer) and
// drone.sendShop launches it. VOTV has NO UE replication -> a client's order is 100% client-local,
// so coop forwards it to the host, who re-commits it here via the SAME native makeAnOrder (the host
// is the delivery authority; the drone + cargo then sync via DroneState + the prop pipeline).
//
// v136 (2026-08-24, security A34/A35) -- WHAT AN ORDER IS ON THE WIRE CHANGED, and this header is
// where the reason lives.
//
// An item is now a `list_store` ROW NAME and nothing else. It used to be the client's own
// price + size + category + object CLASS NAME, which the host wrote through verbatim. Two separate
// defects wore that one shape:
//
//   * THE SHOP WAS FREE. `[V]` `makeAnOrder`'s blocks 0/56/271/619/788 contain ZERO `addPoints` --
//     the charge lives in the CALLER, `ui_laptop`'s Button_order ubergraph (@6122
//     Multiply(storePrice,-1), @6168 addPoints, THEN @6302 makeAnOrder). So our host re-commit could
//     not charge for ANY value of `automatic`: the client debited itself locally, the host was never
//     charged, the goods were delivered, and the client's debit was refunded by the host's next
//     balance broadcast. `docs/COOP_SYNCER_MODEL.md` 2b: an intent may name WHAT, never WHAT IT
//     COSTS. The host prices the order from `ue_wrap::store_catalog` -- its own copy of the table.
//
//   * A CLASS NAME CANNOT NAME A SHOP ITEM. `[V]` The 473 rows map onto only 368 distinct object
//     classes (`prop_C` shared by 50 rows, `prop_seed_C` by 26), so 112 of 473 rows were not
//     uniquely identified by their class. `[V]` `generateStore` stamps the row key into
//     `Fstruct_store.name`, so a locally-placed order already carries the right identity and
//     ReadOrder simply reads it.
//
// AND THE COMMIT NOW COPIES THE LIVE TABLE ROW WHOLESALE, which fixes a third, older defect nobody
// had reported: this header used to say the omitted fields were "cosmetic for MVP" because "the
// spawn uses `object` directly". `[V]` `prop_orderBox` -- the delivered box -- branches on
// `item.object == prop_C`, branches on `item.asProp == None`, and calls
// `player->sendName(item.asProp)`. With 101 rows carrying an `asProp` variant name and 40 carrying
// `parseRowNameToObject`, writing NAME_None for those fields mis-delivered ~141 of 473 rows on every
// CLIENT order. Copying the row removes the question entirely.
//
// THE ONE FIELD STILL OVERWRITTEN is `subcategory`, stamped with the pinned empty FText.
// `[V]` `prop_orderBox` builds its own order items with `subcategory = EX_TextConst`, so a second
// in-game producer already ships const-empty subcategories -- but note this is the ONE place our
// committed row deliberately differs from what the host's own Button_order would have produced
// (that path carries the table's real FText). It is kept because copying the live FText would rest
// on an unmeasured claim about who deep-copies it and when.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::order_economy {

// One order, as the wire and the arbiter both see it: the `list_store` row names of its items, in
// cart order, multiplicity preserved (the same row twice is two line items). Everything else --
// price, object, size, category, asProp, parseRowNameToObject -- is resolved by the HOST from its
// own table and never travels.
struct OrderData {
    std::vector<std::wstring> rowNames;
};

// Count of orders currently in saveSlot.orders (the local queue). -1 if unresolved (booting/menu).
// The host also uses this to CONFIRM a commit: `ue_wrap::Call` reports only that the dispatch
// happened, so a +1 edge across CommitOrder is what actually proves the order queued. `[V]` That
// edge is exact -- `drone::checkOrders` contains no removeOrderCart and no Array_Remove, so nothing
// pops synchronously inside the commit.
int32_t OrderCount();

// Read saveSlot.orders[index]'s item ROW NAMES into `out` (clears + fills it). False if unresolved /
// index out of range / the order has no items / the catalog is unusable (the row-name offset comes
// from ue_wrap::store_catalog, which owns the row's shape). Game thread.
bool ReadOrder(int32_t index, OrderData& out);

// HOST: commit `order` as a real delivery via the native Uui_laptop_C::makeAnOrder.
//
// Every item is looked up in ue_wrap::store_catalog and the LIVE table row is copied wholesale into
// a heap items buffer the native deep-copies (then we free it), with the pinned empty FText stamped
// over `subcategory`. Returns false -- committing NOTHING -- if the catalog is unusable or ANY row
// name is unknown: a partial order would charge for goods the arbiter could not name.
//
// `etaSeconds` is the delivery ETA the HOST rolled (RandomFloatInRange(120,180) is what the game's
// own Button_order does); the client's number is not on the wire, per the host-authoritative-RNG
// rule.
//
// `automatic` is passed to the native and is NOT about payment. `[V]` It gates the branch at
// makeAnOrder@271 which does `save_main.stats.items_bought += cart.Num` -- a STATISTIC, and one
// computed from the committing peer's own (empty) cart at that. We pass true so the host's stats are
// not polluted by a client's purchase. The header used to call `automatic=true` "the auto/unpaid
// path", which framed the flag as a choice between a paid and an unpaid variant when there is no
// paid variant inside this function at all; that framing is why nobody looked.
bool CommitOrder(const OrderData& order, float etaSeconds, bool automatic);

// HOST: are the actors CommitOrder dereferences all present, so a commit can't null-fault?
// (drone present + radiotower present + laptop present + drone.sellLocation present). A BUSY drone
// is fine -- the native addOrderCart APPENDS to saveSlot.orders and the drone's own checkOrders
// pops the next on arrival (sendShop no-ops while Active), so multiple orders QUEUE natively; we do
// NOT require idle. A BROKEN radiotower is also fine -- sendShop handles it (queues + emails, no
// fly). Only a still-loading world (a null actor) must DEFER (caller retries). Game thread.
bool CanCommit();

// CLIENT: after forwarding a locally-placed order, reset the mirror drone's self-takeoff so its
// own makeAnOrder->sendShop (which set Active:=true / flyingType:=0 / hasOrder:=true on this peer's
// drone) can't fake a local flight -- the drone must stay a pure host-driven mirror. Writes the
// checkOrders empty-queue arm: Active@0x0370:=false, flyingType@0x0300:=-1, hasOrder@0x0360:=false.
// (Bytecode-verified field set; RE Q2.) No-op-safe if sendShop never ran (fields already at rest).
// Returns false if the drone isn't resolvable. Game thread.
//
// NOTE what this does NOT undo, measured 2026-08-24 (drone uber @13422): sendShop also sets
// `drone.order` (inert here, because drone_sync suppresses the client drone's ReceiveTick so
// checkOrders never runs) and, when `gamemode.radiotower.isBroken`, calls `lib::addEmail` locally --
// which the host also does when it commits, so that is a duplicate-email path. It is NOT
// suppressible (both sendShop and addEmail are EX_LocalVirtualFunction); it is filed as security
// A47 rather than papered over here.
bool QuietLocalDrone();

// CLIENT: put `rowNames` back into the laptop's cart (the native Uui_laptop_C::addStoreCart, once
// per row, with the live list_store row as its argument). Used ONLY when the host refuses a
// forwarded order.
//
// WHY IT EXISTS AT ALL: `[V]` single-player's own affordability gate pops at Button_order @5990,
// BEFORE `Array_Clear(cart)` @6326 -- so when the base game refuses a purchase the cart survives
// untouched. A client's order, by contrast, has already run the whole ubergraph locally by the time
// the host sees it, so its cart is gone. Restoring it is what makes the refusal behave the way the
// game behaves; not restoring it would invent a punishment single-player does not have.
//
// Best-effort by design: returns the number of rows actually re-added. A client whose store_catalog
// is unusable gets 0 and still gets told why by the feed line -- the balance correction is the
// correctness half of a refusal, the cart is the courtesy half. Game thread.
int32_t RestoreCartItems(const std::vector<std::wstring>& rowNames);

// CLIENT, DEV-ONLY: place a shop order the way a PLAYER does -- run the laptop's own
// `generateStore`, find the shop slots whose stamped `name` matches `rowNames`, `addStoreCart` each
// one, then `makeAnOrder` with the resulting cart. Returns the summed price of the items actually
// added (0 on any failure), so the caller can apply the same local debit Button_order applies.
//
// WHY IT EXISTS, and it is not a convenience: the order selftest used to build its order through
// `CommitOrder`, i.e. through `ue_wrap::store_catalog` -- which meant the DRILL warmed the catalog
// before the production path ever touched it. That masked a CRITICAL defect for a whole session
// (`ReadOrder` asked for a cached offset and never BUILT the catalog, so on a real client every
// order silently failed to forward while the client had already paid locally). An instrument that
// sets up state the real path does not set up proves only that the instrument works.
//
// So this deliberately touches NO store_catalog: the row identity comes from the shop slots the game
// itself generated, and the Fstruct_store member offsets are resolved off the `cart` property's own
// inner struct. After this runs, the client's saveSlot.orders holds exactly what a human purchase
// would have left there, and the forward path starts from cold.
//
// DRILL ARTEFACT, stated rather than hidden: `Button_order` clears `cart` after committing and this
// does not (clearing a TArray of FText-bearing structs by hand is a memory-correctness question the
// drill has no reason to answer), so the items stay in the local cart. One-shot use only.
// Game thread.
int32_t PlaceOrderFromShopUI(const std::vector<std::wstring>& rowNames, float etaSeconds);

}  // namespace ue_wrap::order_economy

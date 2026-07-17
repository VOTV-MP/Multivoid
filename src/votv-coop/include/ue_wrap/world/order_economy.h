// ue_wrap/order_economy.h -- standalone engine access for the laptop shop ORDER QUEUE
// (saveSlot.orders, the delivery-drone economy). Principle-7 engine-wrapper layer: NO
// network/coop state. coop::order_sync drives the client->host OrderRequest economy through here.
//
// The order data path is bytecode-verified (votv-delivery-drone-RE-and-coop-sync-design-
// 2026-06-03.md): Uui_laptop_C::makeAnOrder(Fstruct_storeOrder, bool automatic) -> addOrderCart
// Array_Adds into AmainGamemode_C.saveSlot.orders @0x0490 (the ONLY persistent order writer) and
// drone.sendShop launches it. VOTV has NO UE replication -> a client's order is 100% client-local,
// so coop forwards it to the host, who re-commits it here via the SAME native makeAnOrder (the host
// is the delivery authority; the drone + cargo then sync via DroneState + the prop pipeline).
//
// Identity of an item = its `object` TSubclassOf<AActor> serialized AS A CLASS NAME (both peers
// load the same cooked classes -> FindClass round-trips). Fstruct_store carries an FText
// (subcategory) -> host reconstruction uses ue_wrap::ftext_utils for a valid empty one.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ue_wrap::order_economy {

// One shop line-item, reduced to the wire-relevant fields (Fstruct_store, struct_store.hpp).
// `objectClass` (the spawned TSubclassOf) is load-bearing; price/size/category are box fidelity.
struct OrderItem {
    std::wstring objectClass;  // leaf class name of object @0x10 (e.g. "prop_reelbox_C")
    int32_t      price    = 0; // @0x00
    int32_t      size     = 0; // @0x40
    uint8_t      category = 0; // @0x20 (enum_shopCats)
};

// One order = a list of items + the delivery-ETA time (Fstruct_storeOrder, 0x18: items@0x00, time@0x10).
struct OrderData {
    std::vector<OrderItem> items;
    float                  time = 0.f;
};

// Count of orders currently in saveSlot.orders (the local queue). -1 if unresolved (booting/menu).
int32_t OrderCount();

// Read saveSlot.orders[index] into `out` (clears + fills it). Resolves each item's object class
// name + price/size/category. False if unresolved / index out of range / the order has no items.
bool ReadOrder(int32_t index, OrderData& out);

// HOST: commit `order` as a real delivery via the native Uui_laptop_C::makeAnOrder(order, automatic).
// Builds the native Fstruct_storeOrder (objects via FindClass; a pinned empty FText for subcategory;
// a heap items buffer the native deep-copies then we free). `automatic=true` = the auto/unpaid path
// (no points charge -- the coop economy delivers for the group; shared-wallet charging is a separate
// concern). False if unresolved / no item class resolved / the dispatch failed. Game thread.
bool CommitOrder(const OrderData& order, bool automatic);

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
bool QuietLocalDrone();

}  // namespace ue_wrap::order_economy

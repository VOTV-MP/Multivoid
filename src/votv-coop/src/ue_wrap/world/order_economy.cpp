// ue_wrap/world/order_economy.cpp -- see ue_wrap/world/order_economy.h.
//
// Reads the local laptop order queue (saveSlot.orders) for the host's queue mirror and a client's
// forward, re-commits an order on the host via the native Uui_laptop_C::makeAnOrder (the proven
// commit+deliver+drain path), and applies the host's queue changes on a client.
// All offsets are reflected by NAME (cooked offsets shift across recooks) + cached; struct-internal
// field offsets are the SDK-dump constants (struct_store.hpp / the Fstruct_storeOrder layout), which
// are stable for this version. Game-thread only (UObject access + a ProcessEvent dispatch).

#include "ue_wrap/world/order_economy.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"  // ClassByName: an unnamed item's class
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/daynightcycle.h"  // Cycle: the daily order's builder
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/world/store_catalog.h"

#include <cstring>
#include <vector>

namespace ue_wrap::order_economy {
namespace {

namespace R = ue_wrap::reflection;

// ---- struct-internal layout (SDK dump; stable for Alpha 0.9.0-n) ----------------------------
// Fstruct_storeOrder (0x18): items TArray @0x00 (Data@+0, Num@+8, Max@+0xC), time f32 @0x10.
constexpr int32_t kOrderStride   = 0x18;
constexpr int32_t kOrderItemsOff = 0x00;
constexpr int32_t kOrderTimeOff  = 0x10;
// Fstruct_store: 0x4D, TArray element stride 0x50 (8-byte aligned for the FText member).
//
// No per-FIELD offset lives here. The commit copies the whole row, and the two offsets it still
// needs (`subcategory` to stamp, `name` to read) are resolved BY NAME by ue_wrap::store_catalog,
// which owns the row's shape. What remains here is the two STRIDES, which are properties of the
// native TArray rather than of any field.
constexpr int32_t kItemStride = 0x50;

// Defensive cap when reading an order's items array (a garbage Num must not drive a huge read).
constexpr int32_t kReadItemCap = 256;
// Defensive cap on a host commit (an absurd item count is clamped). The coop wire enforces its own
// kMaxOrderItems independently; this is the engine layer's own bound (principle 7: no net dependency).
constexpr size_t  kCommitItemCap = 64;

// ---- cached offsets (the gamemode itself is the world singleton's) ----------------------------------

// mainGamemode_C field offsets (constant per class; resolved once).
int32_t g_offSaveSlot   = -1;
int32_t g_offLaptop     = -1;
int32_t g_offDrone      = -1;
int32_t g_offRadiotower = -1;
int32_t g_offOrders     = -1;  // on saveSlot's class

bool ResolveGmOffsets(void* gm) {
    void* gmCls = R::ClassOf(gm);
    if (!gmCls) return false;
    if (g_offSaveSlot   < 0) g_offSaveSlot   = R::FindPropertyOffset(gmCls, L"saveSlot");
    if (g_offLaptop     < 0) g_offLaptop     = R::FindPropertyOffset(gmCls, L"laptop");
    if (g_offDrone      < 0) g_offDrone      = R::FindPropertyOffset(gmCls, L"drone");
    if (g_offRadiotower < 0) g_offRadiotower = R::FindPropertyOffset(gmCls, L"radiotower");
    return g_offSaveSlot >= 0;
}

// Adrone_C field offset (constant per class; resolved ONCE -- never FindPropertyOffset on a
// per-tick path, the standing perf ban). CanCommit runs while a commit is pending.
bool    g_droneOffsetsDone = false;
int32_t g_offDroneSell     = -1;  // sellLocation (sendShop/beginFly read it)
void ResolveDroneOffsets(void* drone) {
    if (g_droneOffsetsDone) return;
    void* dCls = R::ClassOf(drone);
    if (!dCls) return;
    g_offDroneSell     = R::FindPropertyOffset(dCls, L"sellLocation");
    g_droneOffsetsDone = true;
}

template <typename T>
T ReadAt(void* obj, int32_t off) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + off); }

void* ReadPtr(void* obj, int32_t off) {
    void* p = ReadAt<void*>(obj, off);
    return (p && R::IsLive(p)) ? p : nullptr;
}

// Resolve the live saveSlot + the orders TArray offset (cached). Returns the saveSlot ptr or null.
void* ResolveSaveSlot(int32_t* outOrdersOff) {
    void* gm = world_singleton::Gamemode();
    if (!gm || !ResolveGmOffsets(gm)) return nullptr;
    void* save = ReadPtr(gm, g_offSaveSlot);
    if (!save) return nullptr;
    if (g_offOrders < 0) g_offOrders = R::FindPropertyOffset(R::ClassOf(save), L"orders");
    if (g_offOrders < 0) return nullptr;
    if (outOrdersOff) *outOrdersOff = g_offOrders;
    return save;
}

// The row names of the items, and the time, of the Fstruct_storeOrder at `order`. generateStore
// stamps the list_store row key into each Fstruct_store.name, so that IS the shop identity of a line
// item -- and the only part of it that travels. True when a row name was read.
bool ReadItems(const void* order, int32_t nameOff, OrderData& out) {
    const uint8_t* o = static_cast<const uint8_t*>(order);
    const uint8_t* itemsData = *reinterpret_cast<const uint8_t* const*>(o + kOrderItemsOff + 0);
    int32_t itemsNum = *reinterpret_cast<const int32_t*>(o + kOrderItemsOff + 8);
    out.eta = *reinterpret_cast<const float*>(o + kOrderTimeOff);
    if (!itemsData || itemsNum <= 0) return false;
    if (itemsNum > kReadItemCap) itemsNum = kReadItemCap;
    out.rowNames.reserve(static_cast<size_t>(itemsNum));
    for (int32_t i = 0; i < itemsNum; ++i) {
        R::FName nm;
        std::memcpy(&nm, itemsData + static_cast<size_t>(i) * kItemStride + nameOff, sizeof(nm));
        std::wstring s = R::ToString(nm);
        if (s.empty() || s == L"None") {
            UE_LOGW("order_economy: an order's item %d carries no row name -- skipping", i);
            continue;
        }
        out.rowNames.push_back(std::move(s));
    }
    return !out.rowNames.empty();
}

}  // namespace

int32_t OrderCount() {
    int32_t off = -1;
    void* save = ResolveSaveSlot(&off);
    if (!save) return -1;
    return ReadAt<int32_t>(save, off + 8);  // TArray.Num
}

bool ReadQueuedOrder(int32_t index, QueuedOrder& out) {
    out = QueuedOrder{};
    namespace SC = ue_wrap::store_catalog;
    // The reflected layout, which the price gate does not withhold: this reads the save's own order.
    SC::Layout lay;
    if (!SC::ReadLayout(lay)) return false;
    const int32_t nameOff = lay.name;
    const int32_t objectOff = lay.object;
    const int32_t asPropOff = lay.asProp;
    if (nameOff < 0) return false;
    int32_t off = -1;
    void* save = ResolveSaveSlot(&off);
    if (!save) return false;
    const int32_t num = ReadAt<int32_t>(save, off + 8);
    if (index < 0 || index >= num) return false;
    const uint8_t* ordersData = ReadAt<const uint8_t*>(save, off + 0);
    if (!ordersData) return false;
    const uint8_t* o = ordersData + static_cast<size_t>(index) * kOrderStride;
    const uint8_t* itemsData = *reinterpret_cast<const uint8_t* const*>(o + kOrderItemsOff + 0);
    int32_t itemsNum = *reinterpret_cast<const int32_t*>(o + kOrderItemsOff + 8);
    out.eta = *reinterpret_cast<const float*>(o + kOrderTimeOff);
    if (!itemsData || itemsNum <= 0) return true;  // an order of no items is still an order
    if (itemsNum > kReadItemCap) itemsNum = kReadItemCap;
    for (int32_t i = 0; i < itemsNum; ++i) {
        const uint8_t* item = itemsData + static_cast<size_t>(i) * kItemStride;
        R::FName nm;
        std::memcpy(&nm, item + nameOff, sizeof(nm));
        std::wstring row = R::ToString(nm);
        if (!row.empty() && row != L"None") {
            out.items.push_back(QueuedItem{std::move(row), {}});
            continue;
        }
        void* cls = objectOff >= 0 ? *reinterpret_cast<void* const*>(item + objectOff) : nullptr;
        if (cls && R::IsLive(cls)) {
            std::wstring asProp;
            if (asPropOff >= 0) {
                R::FName ap;
                std::memcpy(&ap, item + asPropOff, sizeof(ap));
                asProp = R::ToString(ap);
                if (asProp == L"None") asProp.clear();
            }
            out.items.push_back(QueuedItem{{}, R::ToString(R::NameOf(cls)), std::move(asProp)});
            continue;
        }
        UE_LOGW("order_economy: queued order %d's item %d carries neither a row nor a class -- left out", index, i);
    }
    return true;
}

bool ReadOrderAt(const void* order, OrderData& out) {
    out.rowNames.clear();
    // Ready() BUILDS the catalog; NameOffset() alone only reads what a previous build cached, and nothing
    // else on a client's forward path builds it.
    if (!order || !ue_wrap::store_catalog::Ready()) return false;
    const int32_t nameOff = ue_wrap::store_catalog::NameOffset();
    return nameOff >= 0 && ReadItems(order, nameOff, out);
}

bool CanCommit() {
    void* gm = world_singleton::Gamemode();
    if (!gm || !ResolveGmOffsets(gm)) return false;
    if (g_offDrone < 0 || g_offRadiotower < 0 || g_offLaptop < 0) return false;
    void* drone = ReadPtr(gm, g_offDrone);
    if (!drone) return false;
    if (!ReadPtr(gm, g_offRadiotower)) return false;
    if (!ReadPtr(gm, g_offLaptop)) return false;
    // drone.sellLocation -- sendShop/beginFly read it; a null would fault.
    ResolveDroneOffsets(drone);
    if (g_offDroneSell < 0 || !ReadPtr(drone, g_offDroneSell)) return false;
    return true;
}

namespace {

// The laptop widget (ui_laptop_C), which owns the order verbs, or null.
void* Laptop() {
    void* gm = world_singleton::Gamemode();
    if (!gm || !ResolveGmOffsets(gm) || g_offLaptop < 0) return nullptr;
    return ReadPtr(gm, g_offLaptop);
}

// Build `order` as a native Fstruct_storeOrder in `orderStruct`, its items in `itemsBuf` (which must
// outlive the call it is handed to): each item the live list_store row copied wholesale, the pinned
// empty FText over subcategory and the row key stamped into name. ALL-OR-NOTHING: an unknown row
// builds nothing.
bool BuildOrder(const OrderData& order, float etaSeconds, std::vector<uint8_t>& itemsBuf,
                uint8_t (&orderStruct)[kOrderStride], const char* who) {
    namespace SC = ue_wrap::store_catalog;
    size_t n = order.rowNames.size();
    if (n == 0) return false;
    if (n > kCommitItemCap) n = kCommitItemCap;

    if (!SC::Ready()) {
        UE_LOGW("order_economy: %s -- store_catalog INVALID; refusing to build an order whose items we cannot "
                "name or price", who);
        return false;
    }
    const int32_t subcatOff = SC::SubcategoryOffset();
    const int32_t nameOff   = SC::NameOffset();
    if (subcatOff < 0 || nameOff < 0) return false;

    // A valid empty FText for every item's subcategory slot. See the header: this is the ONE field
    // where our committed row deliberately differs from what the host's own Button_order builds.
    uint8_t emptyText[ue_wrap::ftext_utils::kFTextSize];
    if (!ue_wrap::ftext_utils::EmptyFText(emptyText)) {
        UE_LOGW("order_economy: %s -- empty FText unresolved (Kismet not ready) -- defer", who);
        return false;
    }

    // Build a contiguous items buffer the native addOrderCart deep-copies (then we free it), by
    // copying each LIVE list_store row wholesale. ALL-OR-NOTHING: an unknown row name aborts the
    // whole commit, because a partial delivery would hand over goods the arbiter could not name
    // while the caller has already priced the full basket.
    itemsBuf.assign(n * static_cast<size_t>(kItemStride), 0);
    for (size_t i = 0; i < n; ++i) {
        const SC::Row* row = SC::Find(order.rowNames[i]);
        if (!row || !row->data) {
            UE_LOGW("order_economy: %s -- unknown store row '%ls' -- building NOTHING", who,
                    order.rowNames[i].c_str());
            return false;
        }
        uint8_t* base = itemsBuf.data() + i * static_cast<size_t>(kItemStride);
        std::memcpy(base, row->data, static_cast<size_t>(kItemStride));
        std::memcpy(base + subcatOff, emptyText, ue_wrap::ftext_utils::kFTextSize);
        // ...and stamp the row KEY into `name`, because the table does not carry it: every one of
        // the 473 stored rows has name = "None". `generateStore` writes the key into the SHOP
        // SLOT's copy at store-generation time (@1419), so a cart item built by the game's own
        // Button_order carries it and a raw table row does not. Copying the row alone would
        // produce an item that is NOT what the host's own purchase produces, and would strand the
        // identity the forward is keyed on.
        //
        // The FName comes from the catalog's own RowMap key, not from `StringToFName`: that helper
        // is a ProcessEvent dispatch PER ITEM and returns NAME_None SILENTLY when Kismet is
        // unresolved, i.e. it could re-create the exact defect above without a word in the log.
        *reinterpret_cast<R::FName*>(base + nameOff) = row->key;
    }

    // Wrap in a native Fstruct_storeOrder { items TArray; time f32 }.
    std::memset(orderStruct, 0, kOrderStride);
    *reinterpret_cast<void**>(orderStruct + kOrderItemsOff + 0)   = itemsBuf.data();
    *reinterpret_cast<int32_t*>(orderStruct + kOrderItemsOff + 8) = static_cast<int32_t>(n);
    *reinterpret_cast<int32_t*>(orderStruct + kOrderItemsOff + 12) = static_cast<int32_t>(n);
    *reinterpret_cast<float*>(orderStruct + kOrderTimeOff) = etaSeconds;
    return true;
}

// Build a mirror's `order`: each shop item as BuildOrder builds it; each unnamed item in the one shape
// every world-event builder of the game makes (its class, its asProp, one of it, the rest zero: no row,
// price, category or achievement, and the pinned empty FText for subcategory). An item this machine
// cannot build is left out and counted in `left`, down to an order of no items. False only while the
// layout, the empty FText, an asProp's name or (for a row) a catalog not built yet is not ready; a
// catalog the price gate refused leaves its rows out instead, since nothing will ever find one.
bool BuildQueuedOrder(const QueuedOrder& order, std::vector<uint8_t>& itemsBuf, uint8_t (&orderStruct)[kOrderStride],
                      int& left) {
    namespace SC = ue_wrap::store_catalog;
    left = 0;
    itemsBuf.clear();
    std::memset(orderStruct, 0, kOrderStride);
    *reinterpret_cast<float*>(orderStruct + kOrderTimeOff) = order.eta;
    if (order.items.empty()) return true;  // an order of no items needs no catalog
    SC::Layout lay;
    if (!SC::ReadLayout(lay)) return false;
    const int32_t subcatOff = lay.subcategory;
    const int32_t nameOff   = lay.name;
    const int32_t objectOff = lay.object;
    const int32_t asPropOff = lay.asProp;
    const int32_t sizeOff   = lay.size;
    if (subcatOff < 0 || nameOff < 0) return false;
    uint8_t emptyText[ue_wrap::ftext_utils::kFTextSize];
    if (!ue_wrap::ftext_utils::EmptyFText(emptyText)) return false;
    const size_t cap = order.items.size() < kCommitItemCap ? order.items.size() : kCommitItemCap;
    itemsBuf.assign(cap * static_cast<size_t>(kItemStride), 0);
    size_t n = 0;
    for (size_t i = 0; i < cap; ++i) {
        const QueuedItem& it = order.items[i];
        uint8_t* base = itemsBuf.data() + n * static_cast<size_t>(kItemStride);
        if (!it.row.empty()) {
            if (!SC::Ready()) {
                if (SC::Refused()) { ++left; continue; }
                return false;  // not built yet: the order waits rather than lose the row
            }
            const SC::Row* row = SC::Find(it.row);
            if (!row || !row->data) { ++left; continue; }
            std::memcpy(base, row->data, static_cast<size_t>(kItemStride));
            *reinterpret_cast<R::FName*>(base + nameOff) = row->key;
        } else {
            void* cls = (objectOff >= 0 && !it.cls.empty()) ? object_index::ClassByName(it.cls.c_str()) : nullptr;
            if (!cls || (!it.asProp.empty() && asPropOff < 0)) { ++left; continue; }
            if (!it.asProp.empty()) {
                // The name through Kismet, which answers None while it is not up: then the whole order
                // waits rather than build the generic prop_C this item's asProp names.
                const R::FName ap = fname_utils::StringToFName(it.asProp);
                if (ap.ComparisonIndex == 0 && ap.Number == 0) return false;
                *reinterpret_cast<R::FName*>(base + asPropOff) = ap;
            }
            *reinterpret_cast<void**>(base + objectOff) = cls;
            if (sizeOff >= 0) *reinterpret_cast<int32_t*>(base + sizeOff) = 1;
        }
        std::memcpy(base + subcatOff, emptyText, ue_wrap::ftext_utils::kFTextSize);
        ++n;
    }
    left += static_cast<int>(order.items.size() - cap);
    itemsBuf.resize(n * static_cast<size_t>(kItemStride));
    *reinterpret_cast<void**>(orderStruct + kOrderItemsOff + 0)   = n ? itemsBuf.data() : nullptr;
    *reinterpret_cast<int32_t*>(orderStruct + kOrderItemsOff + 8) = static_cast<int32_t>(n);
    *reinterpret_cast<int32_t*>(orderStruct + kOrderItemsOff + 12) = static_cast<int32_t>(n);
    return true;
}

// The chain the laptop's queue verbs reach the queue through -- the widget's own laptop actor, its
// gamemode, the save slot -- set, before a verb is dispatched: an unset link skips the verb's Add or
// Remove while the rest of its body runs, and addOrderCart's slot widget would stack once per retry.
bool QueueChainSet(void* widget) {
    static int32_t sWidgetLaptopOff = -1, sActorGamemodeOff = -1;
    if (sWidgetLaptopOff < 0) sWidgetLaptopOff = R::FindPropertyOffset(R::ClassOf(widget), L"laptop");
    if (sWidgetLaptopOff < 0) return false;
    void* actor = ReadPtr(widget, sWidgetLaptopOff);
    if (!actor) return false;
    if (sActorGamemodeOff < 0) sActorGamemodeOff = R::FindPropertyOffset(R::ClassOf(actor), L"gamemode");
    if (sActorGamemodeOff < 0) return false;
    void* gm = ReadPtr(actor, sActorGamemodeOff);
    return gm && ResolveGmOffsets(gm) && ReadPtr(gm, g_offSaveSlot) != nullptr;
}

}  // namespace

bool CommitOrder(const OrderData& order, float etaSeconds, bool automatic) {
    void* laptop = Laptop();
    if (!laptop) { UE_LOGW("order_economy: CommitOrder -- laptop null"); return false; }
    std::vector<uint8_t> itemsBuf;
    uint8_t orderStruct[kOrderStride];
    if (!BuildOrder(order, etaSeconds, itemsBuf, orderStruct, "CommitOrder")) return false;
    const size_t n = itemsBuf.size() / static_cast<size_t>(kItemStride);

    void* fn = R::FindFunction(R::ClassOf(laptop), L"makeAnOrder");
    if (!fn) { UE_LOGW("order_economy: CommitOrder -- makeAnOrder UFunction not found"); return false; }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    if (!f.SetRaw(L"NewItem", orderStruct, kOrderStride)) {
        UE_LOGW("order_economy: CommitOrder -- SetRaw(NewItem) failed");
        return false;
    }
    f.Set<bool>(L"automatic", automatic);
    const bool ok = ue_wrap::Call(laptop, f);
    UE_LOGI("order_economy: CommitOrder -- makeAnOrder(items=%zu, eta=%.1f, automatic=%d) dispatch=%d",
            n, etaSeconds, automatic ? 1 : 0, ok ? 1 : 0);
    // itemsBuf freed here -- safe: addOrderCart's Array_Add already deep-copied into saveSlot.orders.
    // NOTE `ok` is the DISPATCH result, NOT makeAnOrder's outcome. The caller confirms the commit
    // with an OrderCount() +1 edge before it charges anyone.
    return ok;
}

Applied AppendOrder(const QueuedOrder& order, int* leftOut) {
    void* laptop = Laptop();
    void* fn = laptop ? R::FindDispatchFunctionCached(R::ClassOf(laptop), L"addOrderCart") : nullptr;
    const int32_t before = OrderCount();
    if (!fn || before < 0 || !QueueChainSet(laptop)) return Applied::Later;
    std::vector<uint8_t> itemsBuf;
    uint8_t orderStruct[kOrderStride];
    int left = 0;
    if (!BuildQueuedOrder(order, itemsBuf, orderStruct, left)) return Applied::Later;
    ue_wrap::ParamFrame f(fn);
    // itemsBuf outlives the call: addOrderCart's Array_Add deep-copies the struct into the queue.
    if (!f.valid() || !f.SetRaw(L"NewItem", orderStruct, kOrderStride) || !ue_wrap::Call(laptop, f))
        return Applied::Later;
    // The verb's body can skip its Add (an unset reference inside it), so the append is the queue
    // growing by exactly one, not the call returning.
    if (OrderCount() != before + 1) return Applied::Later;
    if (left > 0)
        UE_LOGW("order_economy: AppendOrder -- %d of %zu item(s) cannot be built here (a row this catalog "
                "lacks, or a class not loaded) and were left out of the mirrored order", left, order.items.size());
    if (leftOut) *leftOut = left;
    return Applied::Done;
}

bool MakeDailyOrder() {
    if (!CanCommit()) return false;  // makeAnOrder's sendShop reads the drone and the tower
    void* cycle = ue_wrap::daynightcycle::Cycle();
    void* laptop = Laptop();
    void* makeFn = cycle ? R::FindDispatchFunctionCached(R::ClassOf(cycle), L"Make Default Order") : nullptr;
    void* orderFn = laptop ? R::FindDispatchFunctionCached(R::ClassOf(laptop), L"makeAnOrder") : nullptr;
    const int32_t before = OrderCount();
    if (!makeFn || !orderFn || before < 0) return false;
    ue_wrap::ParamFrame built(makeFn);
    uint8_t order[kOrderStride] = {};
    if (!built.valid() || !ue_wrap::Call(cycle, built) || !built.GetRaw(L"struct_storeOrder", order, kOrderStride))
        return false;
    ue_wrap::ParamFrame f(orderFn);
    const bool ok = f.valid() && f.SetRaw(L"NewItem", order, kOrderStride) && f.Set<bool>(L"automatic", true) &&
                    ue_wrap::Call(laptop, f);
    // makeAnOrder's addOrderCart deep-copied the items; the array the builder made is ours to release.
    R::EngineFree(*reinterpret_cast<void**>(order + kOrderItemsOff));
    return ok && OrderCount() == before + 1;
}

Applied PopOrder() {
    const int32_t before = OrderCount();
    if (before <= 0) return Applied::Later;  // removeOrderCart pops index 0 with no bound check
    void* laptop = Laptop();
    void* fn = laptop ? R::FindDispatchFunctionCached(R::ClassOf(laptop), L"removeOrderCart") : nullptr;
    if (!fn || !QueueChainSet(laptop)) return Applied::Later;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !ue_wrap::Call(laptop, f)) return Applied::Later;
    // As AppendOrder: the effect, not the dispatch. The verb takes the order off through the widget's
    // own laptop reference, and an unset one skips it without a word.
    return OrderCount() == before - 1 ? Applied::Done : Applied::Later;
}

int32_t RestoreCartItems(const std::vector<std::wstring>& rowNames) {
    namespace SC = ue_wrap::store_catalog;
    if (rowNames.empty()) return 0;
    if (!SC::Ready()) {
        UE_LOGW("order_economy: RestoreCartItems -- store_catalog unusable; the refused cart cannot "
                "be rebuilt (the balance correction still applies)");
        return 0;
    }
    void* gm = world_singleton::Gamemode();
    if (!gm || !ResolveGmOffsets(gm) || g_offLaptop < 0) return 0;
    void* laptop = ReadPtr(gm, g_offLaptop);
    if (!laptop) return 0;

    void* fn = R::FindFunction(R::ClassOf(laptop), L"addStoreCart");
    if (!fn) { UE_LOGW("order_economy: RestoreCartItems -- addStoreCart not found"); return 0; }

    // The native takes the struct BY VALUE, and its declared size is the STRUCT size (0x4D), not the
    // TArray element STRIDE (0x50). Ask the UFunction rather than assuming which of the two it is --
    // writing 0x50 bytes into a 0x4D parameter slot would scribble on whatever follows it.
    const int32_t paramSize = R::FindParamSize(fn, L"struct_store");
    if (paramSize <= 0 || paramSize > kItemStride) {
        UE_LOGW("order_economy: RestoreCartItems -- addStoreCart param size %d is not plausible "
                "(stride %d) -- skipping", paramSize, kItemStride);
        return 0;
    }

    int32_t added = 0;
    for (const std::wstring& name : rowNames) {
        const SC::Row* row = SC::Find(name);
        if (!row || !row->data) {
            UE_LOGW("order_economy: RestoreCartItems -- unknown store row '%ls' -- skipped",
                    name.c_str());
            continue;
        }
        ue_wrap::ParamFrame f(fn);
        if (!f.valid()) break;
        // The LIVE row, unmodified: this is a local UI restore, so unlike the commit path there is
        // no reason to stamp over `subcategory` -- the real FText is what the cart row would have
        // held, and the native copies it out of our frame the same way the game's own path does.
        if (!f.SetRaw(L"struct_store", row->data, paramSize)) break;
        if (!ue_wrap::Call(laptop, f)) break;
        ++added;
    }
    UE_LOGI("order_economy: RestoreCartItems -- re-added %d of %zu refused item(s) to the cart",
            added, rowNames.size());
    return added;
}

int32_t PlaceOrderFromShopUI(const std::vector<std::wstring>& rowNames, float etaSeconds) {
    // EVERY bail below logs. A silent `return 0` here is indistinguishable from "the shop was
    // empty", which is exactly the ambiguity that has already cost this feature three smoke runs.
    if (rowNames.empty()) return 0;
    void* gm = world_singleton::Gamemode();
    if (!gm || !ResolveGmOffsets(gm) || g_offLaptop < 0) {
        UE_LOGW("order_economy: PlaceOrderFromShopUI -- gamemode/laptop offset unresolved");
        return 0;
    }
    void* laptop = ReadPtr(gm, g_offLaptop);
    void* lapCls = laptop ? R::ClassOf(laptop) : nullptr;
    if (!laptop || !lapCls) {
        UE_LOGW("order_economy: PlaceOrderFromShopUI -- laptop widget not live yet");
        return 0;
    }

    void* genFn = R::FindFunction(lapCls, L"generateStore");
    void* addFn = R::FindFunction(lapCls, L"addStoreCart");
    void* mkFn  = R::FindFunction(lapCls, L"makeAnOrder");
    if (!genFn || !addFn || !mkFn) {
        UE_LOGW("order_economy: PlaceOrderFromShopUI -- generateStore/addStoreCart/makeAnOrder "
                "not all found");
        return 0;
    }

    // Fstruct_store's member offsets -- asked of `addStoreCart`'s own `struct_store` PARAMETER,
    // which IS a struct-typed property. Deliberately NOT store_catalog (see the header), and
    // deliberately not the `cart` member either: `cart` is a TArray<Fstruct_store>, so the struct
    // is the ARRAY'S INNER and PropertyInnerStruct returns null for it -- measured, the first cut of
    // this function failed with "name@-1 price@-1" while cart@2832 resolved perfectly.
    void* rowStruct = R::PropertyInnerStruct(addFn, L"struct_store");
    const int32_t offName  = rowStruct ? R::FindPropertyOffsetByPrefix(rowStruct, L"name_")  : -1;
    const int32_t offPrice = rowStruct ? R::FindPropertyOffsetByPrefix(rowStruct, L"price_") : -1;
    const int32_t offCart  = R::FindPropertyOffset(lapCls, L"cart");
    const int32_t offSlots = R::FindPropertyOffset(lapCls, L"storeSlots");
    if (offName < 0 || offPrice < 0 || offCart < 0 || offSlots < 0) {
        UE_LOGW("order_economy: PlaceOrderFromShopUI -- unresolved (name@%d price@%d cart@%d "
                "storeSlots@%d)", offName, offPrice, offCart, offSlots);
        return 0;
    }

    // The game's own store generation -- this is what stamps the list_store row key into each
    // slot's Fstruct_store.name (the table itself stores "None" on all 473 rows).
    { ue_wrap::ParamFrame f(genFn); if (f.valid()) ue_wrap::Call(laptop, f); }

    const int32_t addParamSize = R::FindParamSize(addFn, L"struct_store");
    if (addParamSize <= 0 || addParamSize > kItemStride) {
        UE_LOGW("order_economy: PlaceOrderFromShopUI -- addStoreCart param size %d implausible",
                addParamSize);
        return 0;
    }

    const int32_t slotsNum = ReadAt<int32_t>(laptop, offSlots + 8);
    void* slotsData = ReadAt<void*>(laptop, offSlots + 0);
    if (!slotsData || slotsNum <= 0) {
        UE_LOGW("order_economy: PlaceOrderFromShopUI -- generateStore produced %d slots", slotsNum);
        return 0;
    }
    // Every shop slot is a widget; `data` is its Fstruct_store. Resolved once off the first slot.
    void* firstSlot = ReadAt<void*>(slotsData, 0);
    const int32_t offData = firstSlot ? R::FindPropertyOffset(R::ClassOf(firstSlot), L"data") : -1;
    if (offData < 0) { UE_LOGW("order_economy: PlaceOrderFromShopUI -- shopSlot.data unresolved"); return 0; }

    int32_t total = 0;
    int32_t added = 0;
    for (const std::wstring& want : rowNames) {
        bool hit = false;
        for (int32_t i = 0; i < slotsNum && !hit; ++i) {
            void* slot = ReadAt<void*>(slotsData, static_cast<int32_t>(i * sizeof(void*)));
            if (!slot || !R::IsLive(slot)) continue;
            auto* data = reinterpret_cast<uint8_t*>(slot) + offData;
            if (R::ToString(*reinterpret_cast<R::FName*>(data + offName)) != want) continue;
            ue_wrap::ParamFrame f(addFn);
            if (!f.valid() || !f.SetRaw(L"struct_store", data, addParamSize)) break;
            if (!ue_wrap::Call(laptop, f)) break;
            total += *reinterpret_cast<int32_t*>(data + offPrice);
            ++added;
            hit = true;
        }
        if (!hit) UE_LOGW("order_economy: PlaceOrderFromShopUI -- no shop slot named '%ls' (locked "
                          "by an achievement, or not in this build's store)", want.c_str());
    }
    if (added == 0) {
        UE_LOGW("order_economy: PlaceOrderFromShopUI -- generateStore produced %d slots but none "
                "matched the requested rows", slotsNum);
        return 0;
    }

    // Commit the cart exactly as Button_order does: items = cart, time = the ETA.
    uint8_t orderStruct[kOrderStride] = {0};
    std::memcpy(orderStruct + kOrderItemsOff, reinterpret_cast<uint8_t*>(laptop) + offCart, 16);
    *reinterpret_cast<float*>(orderStruct + kOrderTimeOff) = etaSeconds;
    ue_wrap::ParamFrame f(mkFn);
    if (!f.valid() || !f.SetRaw(L"NewItem", orderStruct, kOrderStride)) {
        UE_LOGW("order_economy: PlaceOrderFromShopUI -- makeAnOrder frame/SetRaw failed");
        return 0;
    }
    f.Set<bool>(L"automatic", false);
    if (!ue_wrap::Call(laptop, f)) {
        UE_LOGW("order_economy: PlaceOrderFromShopUI -- makeAnOrder dispatch failed");
        return 0;
    }

    UE_LOGI("order_economy: PlaceOrderFromShopUI -- added %d of %zu item(s) from the generated shop, "
            "total %d, committed via makeAnOrder", added, rowNames.size(), total);
    return total;
}

}  // namespace ue_wrap::order_economy

// ue_wrap/order_economy.cpp -- see ue_wrap/order_economy.h.
//
// Reads the local laptop order queue (saveSlot.orders) for the client forward, and re-commits an
// order on the host via the native Uui_laptop_C::makeAnOrder (the proven commit+deliver+drain path).
// All offsets are reflected by NAME (cooked offsets shift across recooks) + cached; struct-internal
// field offsets are the SDK-dump constants (struct_store.hpp / the Fstruct_storeOrder layout), which
// are stable for this version. Game-thread only (UObject access + a ProcessEvent dispatch).

#include "ue_wrap/world/order_economy.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/ftext_utils.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
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
// v136: the per-FIELD offsets that used to live here (price@0x00, object@0x10, category@0x20,
// subcategory@0x28, size@0x40) are GONE -- five version-coupled literals retired, not moved. The
// commit copies the whole row, and the two offsets still needed (`subcategory` to stamp, `name` to
// read) are resolved BY NAME by ue_wrap::store_catalog, which owns the row's shape. What remains
// here is the two STRIDES, which are properties of the native TArray rather than of any field.
constexpr int32_t kItemStride = 0x50;

// Defensive cap when reading an order's items array (a garbage Num must not drive a huge read).
constexpr int32_t kReadItemCap = 256;
// Defensive cap on a host commit (an absurd item count is clamped). The coop wire enforces its own
// kMaxOrderItems independently; this is the engine layer's own bound (principle 7: no net dependency).
constexpr size_t  kCommitItemCap = 64;

// ---- cached resolution (mirrors ue_wrap/economy.cpp) ----------------------------------------
ue_wrap::CachedObjRef g_gm;  // islive-zeroav row :47
void* ResolveGamemode() {
    if (g_gm.Alive()) return g_gm.Raw();
    g_gm.Set(R::FindObjectByClass(L"mainGamemode_C"));
    return g_gm.Raw();
}

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

// Adrone_C field offsets (constant per class; resolved ONCE -- never FindPropertyOffset on a
// per-tick path, the standing perf ban). CanCommit + QuietLocalDrone run while a commit is
// pending / just after a client forward.
bool    g_droneOffsetsDone = false;
int32_t g_offDroneSell     = -1;  // sellLocation (sendShop/beginFly read it)
int32_t g_offDroneActive   = -1;  // Active@0x0370
int32_t g_offDroneFlying   = -1;  // flyingType@0x0300
int32_t g_offDroneHasOrder = -1;  // hasOrder@0x0360
void ResolveDroneOffsets(void* drone) {
    if (g_droneOffsetsDone) return;
    void* dCls = R::ClassOf(drone);
    if (!dCls) return;
    g_offDroneSell     = R::FindPropertyOffset(dCls, L"sellLocation");
    g_offDroneActive   = R::FindPropertyOffset(dCls, L"Active");
    g_offDroneFlying   = R::FindPropertyOffset(dCls, L"flyingType");
    g_offDroneHasOrder = R::FindPropertyOffset(dCls, L"hasOrder");
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
    void* gm = ResolveGamemode();
    if (!gm || !ResolveGmOffsets(gm)) return nullptr;
    void* save = ReadPtr(gm, g_offSaveSlot);
    if (!save) return nullptr;
    if (g_offOrders < 0) g_offOrders = R::FindPropertyOffset(R::ClassOf(save), L"orders");
    if (g_offOrders < 0) return nullptr;
    if (outOrdersOff) *outOrdersOff = g_offOrders;
    return save;
}

}  // namespace

int32_t OrderCount() {
    int32_t off = -1;
    void* save = ResolveSaveSlot(&off);
    if (!save) return -1;
    return ReadAt<int32_t>(save, off + 8);  // TArray.Num
}

bool ReadOrder(int32_t index, OrderData& out) {
    out.rowNames.clear();
    const int32_t nameOff = ue_wrap::store_catalog::NameOffset();
    if (nameOff < 0) {
        UE_LOGW("order_economy: ReadOrder -- store_catalog unusable, so the row-name field cannot be "
                "located; refusing to read an order whose items we could not name");
        return false;
    }
    int32_t off = -1;
    void* save = ResolveSaveSlot(&off);
    if (!save) return false;
    const int32_t num = ReadAt<int32_t>(save, off + 8);
    if (index < 0 || index >= num) return false;
    void* ordersData = ReadAt<void*>(save, off + 0);
    if (!ordersData) return false;

    void* order = reinterpret_cast<uint8_t*>(ordersData) + static_cast<size_t>(index) * kOrderStride;
    void* itemsData = ReadAt<void*>(order, kOrderItemsOff + 0);
    int32_t itemsNum = ReadAt<int32_t>(order, kOrderItemsOff + 8);
    if (!itemsData || itemsNum <= 0) return false;
    if (itemsNum > kReadItemCap) itemsNum = kReadItemCap;

    out.rowNames.reserve(static_cast<size_t>(itemsNum));
    for (int32_t i = 0; i < itemsNum; ++i) {
        void* item = reinterpret_cast<uint8_t*>(itemsData) + static_cast<size_t>(i) * kItemStride;
        // `[V]` generateStore stamps the list_store row key into Fstruct_store.name, so this IS the
        // shop identity of the line item -- and the only part of it that travels.
        const R::FName nm = ReadAt<R::FName>(item, nameOff);
        std::wstring s = R::ToString(nm);
        if (s.empty() || s == L"None") {
            UE_LOGW("order_economy: ReadOrder(%d) item %d carries no row name -- skipping", index, i);
            continue;
        }
        out.rowNames.push_back(std::move(s));
    }
    return !out.rowNames.empty();
}

bool CanCommit() {
    void* gm = ResolveGamemode();
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

bool CommitOrder(const OrderData& order, float etaSeconds, bool automatic) {
    namespace SC = ue_wrap::store_catalog;

    void* gm = ResolveGamemode();
    if (!gm || !ResolveGmOffsets(gm) || g_offLaptop < 0) return false;
    void* laptop = ReadPtr(gm, g_offLaptop);
    if (!laptop) { UE_LOGW("order_economy: CommitOrder -- laptop null"); return false; }

    size_t n = order.rowNames.size();
    if (n == 0) return false;
    if (n > kCommitItemCap) n = kCommitItemCap;

    if (!SC::Ready()) {
        UE_LOGW("order_economy: CommitOrder -- store_catalog INVALID; refusing to commit an order "
                "whose items we cannot name or price");
        return false;
    }
    const int32_t subcatOff = SC::SubcategoryOffset();
    const int32_t nameOff   = SC::NameOffset();
    if (subcatOff < 0 || nameOff < 0) return false;

    // A valid empty FText for every item's subcategory slot. See the header: this is the ONE field
    // where our committed row deliberately differs from what the host's own Button_order builds.
    uint8_t emptyText[ue_wrap::ftext_utils::kFTextSize];
    if (!ue_wrap::ftext_utils::EmptyFText(emptyText)) {
        UE_LOGW("order_economy: CommitOrder -- empty FText unresolved (Kismet not ready) -- defer");
        return false;
    }

    // Build a contiguous items buffer the native addOrderCart deep-copies (then we free it), by
    // copying each LIVE list_store row wholesale. ALL-OR-NOTHING: an unknown row name aborts the
    // whole commit, because a partial delivery would hand over goods the arbiter could not name
    // while the caller has already priced the full basket.
    std::vector<uint8_t> itemsBuf(n * static_cast<size_t>(kItemStride), 0);
    for (size_t i = 0; i < n; ++i) {
        const SC::Row* row = SC::Find(order.rowNames[i]);
        if (!row || !row->data) {
            UE_LOGW("order_economy: CommitOrder -- unknown store row '%ls' -- committing NOTHING",
                    order.rowNames[i].c_str());
            return false;
        }
        uint8_t* base = itemsBuf.data() + i * static_cast<size_t>(kItemStride);
        std::memcpy(base, row->data, static_cast<size_t>(kItemStride));
        std::memcpy(base + subcatOff, emptyText, ue_wrap::ftext_utils::kFTextSize);
        // ...and stamp the row KEY into `name`, because the table does not carry it. `[V]` every
        // one of the 473 stored rows has name = "None"; `generateStore` writes the key into the
        // SHOP SLOT's copy at store-generation time (@1419), so a cart item built by the game's own
        // Button_order carries it and a raw table row does not. Copying the row alone would
        // therefore produce an item that is NOT what the host's own purchase produces -- and would
        // strand the identity that v136 made load-bearing. Found by the order selftest, whose
        // forward failed with "ReadOrder(0) failed" until this line existed.
        *reinterpret_cast<R::FName*>(base + nameOff) =
            ue_wrap::fname_utils::StringToFName(order.rowNames[i]);
    }

    // Wrap in a native Fstruct_storeOrder { items TArray; time f32 }.
    uint8_t orderStruct[kOrderStride] = {0};
    *reinterpret_cast<void**>(orderStruct + kOrderItemsOff + 0)   = itemsBuf.data();
    *reinterpret_cast<int32_t*>(orderStruct + kOrderItemsOff + 8) = static_cast<int32_t>(n);
    *reinterpret_cast<int32_t*>(orderStruct + kOrderItemsOff + 12) = static_cast<int32_t>(n);
    *reinterpret_cast<float*>(orderStruct + kOrderTimeOff) = etaSeconds;

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

bool QuietLocalDrone() {
    void* gm = ResolveGamemode();
    if (!gm || !ResolveGmOffsets(gm) || g_offDrone < 0) return false;
    void* drone = ReadPtr(gm, g_offDrone);
    if (!drone) return false;
    ResolveDroneOffsets(drone);
    if (g_offDroneActive   >= 0) *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(drone) + g_offDroneActive)    = false;
    if (g_offDroneFlying   >= 0) *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(drone) + g_offDroneFlying) = -1;
    if (g_offDroneHasOrder >= 0) *reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(drone) + g_offDroneHasOrder)  = false;
    return true;
}

int32_t RestoreCartItems(const std::vector<std::wstring>& rowNames) {
    namespace SC = ue_wrap::store_catalog;
    if (rowNames.empty()) return 0;
    if (!SC::Ready()) {
        UE_LOGW("order_economy: RestoreCartItems -- store_catalog unusable; the refused cart cannot "
                "be rebuilt (the balance correction still applies)");
        return 0;
    }
    void* gm = ResolveGamemode();
    if (!gm || !ResolveGmOffsets(gm) || g_offLaptop < 0) return 0;
    void* laptop = ReadPtr(gm, g_offLaptop);
    if (!laptop) return 0;

    void* fn = R::FindFunction(R::ClassOf(laptop), L"addStoreCart");
    if (!fn) { UE_LOGW("order_economy: RestoreCartItems -- addStoreCart not found"); return 0; }

    // The native takes the struct BY VALUE, and its declared size is the STRUCT size (0x4D), not the
    // TArray element STRIDE (0x50). Ask the UFunction rather than assuming which of the two it is --
    // writing 0x50 bytes into a 0x4D parameter slot would scribble on whatever follows it.
    int32_t paramSize = -1;
    for (const auto& prm : R::FunctionParams(fn))
        if (prm.name == L"struct_store") { paramSize = prm.size; break; }
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

}  // namespace ue_wrap::order_economy

// ue_wrap/order_economy.cpp -- see ue_wrap/order_economy.h.
//
// Reads the local laptop order queue (saveSlot.orders) for the client forward, and re-commits an
// order on the host via the native Uui_laptop_C::makeAnOrder (the proven commit+deliver+drain path).
// All offsets are reflected by NAME (cooked offsets shift across recooks) + cached; struct-internal
// field offsets are the SDK-dump constants (struct_store.hpp / the Fstruct_storeOrder layout), which
// are stable for this version. Game-thread only (UObject access + a ProcessEvent dispatch).

#include "ue_wrap/order_economy.h"

#include "ue_wrap/call.h"
#include "ue_wrap/ftext_utils.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

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
// Fstruct_store (0x4D, element stride 0x50 -- 8-byte aligned): price@0x00, object@0x10 (TSubclassOf),
// category@0x20 (u8), subcategory@0x28 (FText 0x18), size@0x40.
constexpr int32_t kItemStride   = 0x50;
constexpr int32_t kItemPriceOff = 0x00;
constexpr int32_t kItemObjOff   = 0x10;
constexpr int32_t kItemCatOff   = 0x20;
constexpr int32_t kItemSubcatOff= 0x28;  // FText subcategory -- filled with a pinned empty FText
constexpr int32_t kItemSizeOff  = 0x40;

// Defensive cap when reading an order's items array (a garbage Num must not drive a huge read).
constexpr int32_t kReadItemCap = 256;
// Defensive cap on a host commit (an absurd item count is clamped). The coop wire enforces its own
// kMaxOrderItems independently; this is the engine layer's own bound (principle 7: no net dependency).
constexpr size_t  kCommitItemCap = 64;

// ---- cached resolution (mirrors ue_wrap/economy.cpp) ----------------------------------------
void* g_gm = nullptr;
void* ResolveGamemode() {
    if (g_gm && R::IsLive(g_gm)) return g_gm;
    g_gm = R::FindObjectByClass(L"mainGamemode_C");
    return g_gm;
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
    out.items.clear();
    out.time = 0.f;
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
    out.time = ReadAt<float>(order, kOrderTimeOff);
    if (!itemsData || itemsNum <= 0) return false;
    if (itemsNum > kReadItemCap) itemsNum = kReadItemCap;

    out.items.reserve(static_cast<size_t>(itemsNum));
    for (int32_t i = 0; i < itemsNum; ++i) {
        void* item = reinterpret_cast<uint8_t*>(itemsData) + static_cast<size_t>(i) * kItemStride;
        OrderItem it;
        void* objCls = ReadAt<void*>(item, kItemObjOff);  // TSubclassOf<AActor> -- a UClass*
        if (objCls && R::IsLive(objCls)) it.objectClass = R::ToString(R::NameOf(objCls));
        it.price    = ReadAt<int32_t>(item, kItemPriceOff);
        it.size     = ReadAt<int32_t>(item, kItemSizeOff);
        it.category = ReadAt<uint8_t>(item, kItemCatOff);
        out.items.push_back(std::move(it));
    }
    return !out.items.empty();
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

bool CommitOrder(const OrderData& order, bool automatic) {
    void* gm = ResolveGamemode();
    if (!gm || !ResolveGmOffsets(gm) || g_offLaptop < 0) return false;
    void* laptop = ReadPtr(gm, g_offLaptop);
    if (!laptop) { UE_LOGW("order_economy: CommitOrder -- laptop null"); return false; }

    size_t n = order.items.size();
    if (n == 0) return false;
    if (n > kCommitItemCap) n = kCommitItemCap;

    // A valid empty FText for every item's subcategory slot (zeroed bytes would deref-fault if the
    // host opens the laptop orders UI -- bytecode says commit/deliver never reads it, but be robust).
    uint8_t emptyText[ue_wrap::ftext_utils::kFTextSize];
    if (!ue_wrap::ftext_utils::EmptyFText(emptyText)) {
        UE_LOGW("order_economy: CommitOrder -- empty FText unresolved (Kismet not ready) -- defer");
        return false;
    }

    // Build a contiguous items buffer the native addOrderCart deep-copies (then we free it). Only
    // resolvable item classes are packed (a bad class name is skipped, not faulted).
    std::vector<uint8_t> itemsBuf(n * kItemStride, 0);
    int32_t nValid = 0;
    for (size_t i = 0; i < n; ++i) {
        const OrderItem& it = order.items[i];
        if (it.objectClass.empty()) continue;
        void* cls = R::FindClass(it.objectClass.c_str());
        if (!cls) {
            UE_LOGW("order_economy: CommitOrder -- unknown class '%ls' -- skipping item",
                    it.objectClass.c_str());
            continue;
        }
        uint8_t* base = itemsBuf.data() + static_cast<size_t>(nValid) * kItemStride;
        *reinterpret_cast<int32_t*>(base + kItemPriceOff) = it.price;
        *reinterpret_cast<void**>(base + kItemObjOff)     = cls;       // object TSubclassOf
        *reinterpret_cast<uint8_t*>(base + kItemCatOff)   = it.category;
        std::memcpy(base + kItemSubcatOff, emptyText, ue_wrap::ftext_utils::kFTextSize);
        *reinterpret_cast<int32_t*>(base + kItemSizeOff)  = it.size;
        // name/asProp/achievementUnlock FNames left NAME_None (0,0); parseRowNameToObject false --
        // the spawn uses `object` directly (RE Q5), so these are cosmetic for MVP.
        ++nValid;
    }
    if (nValid == 0) { UE_LOGW("order_economy: CommitOrder -- no resolvable item classes -- dropped"); return false; }

    // Wrap in a native Fstruct_storeOrder { items TArray; time f32 }.
    uint8_t orderStruct[kOrderStride] = {0};
    *reinterpret_cast<void**>(orderStruct + kOrderItemsOff + 0) = itemsBuf.data();  // items.Data
    *reinterpret_cast<int32_t*>(orderStruct + kOrderItemsOff + 8) = nValid;          // items.Num
    *reinterpret_cast<int32_t*>(orderStruct + kOrderItemsOff + 12) = nValid;         // items.Max
    *reinterpret_cast<float*>(orderStruct + kOrderTimeOff) = order.time;

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
    UE_LOGI("order_economy: CommitOrder -- makeAnOrder(items=%d, automatic=%d) ok=%d",
            nValid, automatic ? 1 : 0, ok ? 1 : 0);
    // itemsBuf freed here -- safe: addOrderCart's Array_Add already deep-copied into saveSlot.orders.
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

}  // namespace ue_wrap::order_economy

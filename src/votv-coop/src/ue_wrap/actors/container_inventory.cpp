// ue_wrap/actors/container_inventory.cpp -- see ue_wrap/actors/container_inventory.h.

#include "ue_wrap/actors/container_inventory.h"

#include "ue_wrap/actors/prop.h"           // WalksToBase
#include "ue_wrap/actors/save_record.h"    // ReadArr
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"

#include <cstring>

namespace ue_wrap::container_inventory {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;
namespace II = ue_wrap::inventory_item;

int32_t g_offCanvas    = -2;  // inventoryComponent_C.canvasInventory
int32_t g_offInvOwner  = -2;  // inventoryComponent_C.owner  -- set by makeOwner at BeginPlay, never cleared
int32_t g_offComponent = -2;  // prop_container_C.inventoryComponent
int32_t g_offTotalVol  = -2;  // inventoryComponent_C.totalVolume
int32_t g_offMaxVol    = -2;  // inventoryComponent_C.maxVolume  -- the capacity addItemAdvanced's volume check reads
int32_t g_offDisplay   = -2;  // inventoryComponent_C.activeDisplayCanvas
int32_t g_offAllContainers = -2;  // mainGamemode_C.allContainers

int32_t CachedOffset(int32_t& slot, void* cls, const wchar_t* name) {
    if (slot == -2) {
        slot = cls ? R::FindPropertyOffset(cls, name) : -1;
        if (slot < 0)
            UE_LOGW("container_inventory: could not resolve %ls -- the contents lane is inert for it", name);
    }
    return slot;
}

template <class T> T ReadAt(const void* base, int32_t off) {
    T v{};
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

// Looked up per use, one index lookup each, so a class not loaded yet is asked for again.
void* ContainerClass() { return ue_wrap::object_index::ClassByName(L"prop_container_C"); }
void* InventoryClass() { return ue_wrap::object_index::ClassByName(L"inventoryComponent_C"); }
void* PlayerContainerClass() { return ue_wrap::object_index::ClassByName(L"prop_inventoryContainer_player_C"); }

}  // namespace

bool IsContainer(void* actor) {
    void* base = ContainerClass();
    return base && actor && ue_wrap::prop::WalksToBase(R::ClassOf(actor), base);
}

bool IsInventory(void* obj) {
    void* base = InventoryClass();
    return base && obj && ue_wrap::prop::WalksToBase(R::ClassOf(obj), base);
}

void* InventoryOf(void* container) {
    if (!container) return nullptr;
    if (CachedOffset(g_offComponent, R::ClassOf(container), L"inventoryComponent") < 0) return nullptr;
    void* inv = ReadAt<void*>(container, g_offComponent);
    return (inv && R::IsLive(inv)) ? inv : nullptr;
}

void* OwnerOf(void* inventory) {
    if (!inventory) return nullptr;
    void* const owner = R::OuterOf(inventory);
    return (owner && R::IsLive(owner)) ? owner : nullptr;
}

bool IsWorldInventory(void* inventory) {
    void* const owner = OwnerOf(inventory);
    if (!owner || !IsContainer(owner)) return false;
    void* const player = PlayerContainerClass();
    return !(player && ue_wrap::prop::WalksToBase(R::ClassOf(owner), player));
}

uint8_t* ContentsSlot(void* inventory) {
    if (!inventory) return nullptr;
    if (CachedOffset(g_offCanvas, R::ClassOf(inventory), L"canvasInventory") < 0) return nullptr;
    return static_cast<uint8_t*>(inventory) + g_offCanvas;
}

namespace {

// A bitfield bool of an object's class, read by byte and mask. Containers are several classes (a barrel, a crate, a
// bin), so a few classes are kept; a class not kept is asked of reflection once and takes the oldest place.
struct BoolField {
    void* cls = nullptr;
    int32_t off = -1;
    uint8_t mask = 0;
};
struct BoolFields {
    static constexpr size_t kKept = 8;
    BoolField kept[kKept];
    size_t next = 0;
};

bool ReadBool(BoolFields& fields, void* obj, const wchar_t* name, bool& out) {
    void* const cls = R::ClassOf(obj);
    BoolField* f = nullptr;
    for (BoolField& k : fields.kept)
        if (k.cls == cls) { f = &k; break; }
    if (!f) {
        f = &fields.kept[fields.next++ % BoolFields::kKept];
        *f = BoolField{cls, -1, 0};
        if (!R::FindBoolProperty(cls, name, f->off, f->mask)) f->off = -1;
    }
    if (f->off < 0 || f->mask == 0) return false;
    out = (*(static_cast<const uint8_t*>(obj) + f->off) & f->mask) != 0;
    return true;
}

}  // namespace

bool ReadInventoryPutTerms(void* inventory, PutTerms& out) {
    static BoolFields sInfinite;
    out = PutTerms{};
    PutTerms t{};
    if (!inventory || !MaxVolume(inventory, t.maxVolume) || !CurrentVolume(inventory, t.totalVolume) ||
        !ReadBool(sInfinite, inventory, L"infiniteVolume", t.infinite))
        return false;
    out = t;
    return true;
}

bool ReadPutTerms(void* container, PutTerms& out) {
    static BoolFields sLocked;
    static ue_wrap::CachedObjRef sMailbox;   // the mailbox class, asked of the object index until it is found
    out = PutTerms{};
    PutTerms t{};
    if (!ReadInventoryPutTerms(InventoryOf(container), t) || !ReadBool(sLocked, container, L"locked", t.locked))
        return false;
    if (!sMailbox.Alive()) sMailbox.Set(ue_wrap::object_index::ClassByName(L"prop_mailbox_C"));
    t.ownerIsMailbox = sMailbox.Alive() && ue_wrap::prop::WalksToBase(R::ClassOf(container), sMailbox.Raw());
    out = t;
    return true;
}

bool ReadContents(void* inventory, std::vector<Item>& out, size_t maxItems, int32_t* countOut) {
    if (countOut) *countOut = -1;
    uint8_t* const slot = ContentsSlot(inventory);
    if (!slot || !II::Resolve()) return false;
    const SR::Arr items = SR::ReadArr(slot, 0);
    if (countOut) *countOut = items.num;
    if (static_cast<size_t>(items.num) > maxItems) return false;
    const size_t stride = static_cast<size_t>(II::Stride());
    out.clear();
    out.reserve(static_cast<size_t>(items.num));
    for (int32_t i = 0; i < items.num; ++i) {
        Item it;
        if (!II::Read(items.data + static_cast<size_t>(i) * stride, it)) return false;
        out.push_back(std::move(it));
    }
    return true;
}

bool IsContainerItem(const Item& it) {
    if (II::IsEmpty(it)) return false;
    void* const base = ContainerClass();
    if (!base) return false;
    void* const cls = ue_wrap::object_index::ClassByName(II::ClassLeaf(it).c_str());
    return cls && ue_wrap::prop::WalksToBase(cls, base);
}

bool AddItemAsObject(void* inventory, void* actor) {
    if (!inventory || !actor) return false;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(inventory), L"addItemAsObject");
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !f.Set<void*>(L"object", actor) || !ue_wrap::Call(inventory, f)) return false;
    return f.Get<bool>(L"success");
}

bool AddItemAdvanced(void* inventory, const Item& data) {
    if (!inventory || II::IsEmpty(data) || !II::Resolve()) return false;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(inventory), L"addItemAdvanced");
    if (!fn) return false;
    const size_t stride = static_cast<size_t>(II::Stride());
    void* const entry = SR::AllocZeroed(1, stride);   // engine-owned buffers, orphaned like every entry we build
    if (!entry || !II::Write(static_cast<uint8_t*>(entry), data)) return false;
    ue_wrap::ParamFrame f(fn);
    const float position[2] = {data.posX, data.posY};
    if (!f.valid() || !f.SetRaw(L"data", entry, static_cast<int32_t>(stride)) ||
        !f.Set<bool>(L"setPositionAndRotation", true) || !f.SetRaw(L"itemPosition", position, sizeof(position)) ||
        !f.Set<float>(L"itemRotation", data.rotation) || !ue_wrap::Call(inventory, f))
        return false;
    return f.Get<bool>(L"success_volume") && f.Get<bool>(L"validclass");
}

WriteResult WriteContents(void* inventory, const std::vector<Item>& items) {
    uint8_t* const slot = ContentsSlot(inventory);
    if (!slot || !II::Resolve()) return WriteResult::NotReady;
    // Without the allocator an empty array would silently replace real contents.
    if (void* probe = R::EngineAlloc(16)) {
        R::EngineFree(probe);
    } else {
        UE_LOGW("container_inventory: EngineAlloc unavailable -- the entries are left as they are");
        return WriteResult::Refused;
    }
    const int32_t n = static_cast<int32_t>(items.size());
    const size_t stride = static_cast<size_t>(II::Stride());
    void* const buf = SR::AllocZeroed(static_cast<size_t>(n), stride);
    if (!buf && n > 0) {
        UE_LOGW("container_inventory: alloc of %d entries failed -- the entries are left as they are", n);
        return WriteResult::Refused;
    }
    for (int32_t i = 0; i < n; ++i) {
        if (!II::Write(static_cast<uint8_t*>(buf) + static_cast<size_t>(i) * stride, items[static_cast<size_t>(i)])) {
            UE_LOGW("container_inventory: entry %d did not write -- the entries are left as they are", i);
            return WriteResult::Refused;   // the half-built buffer is orphaned, never installed
        }
    }
    void* const shown = DetachDisplay(inventory);
    SR::WriteArrHeader(slot, 0, buf, n);
    if (shown) AttachDisplay(inventory, shown);
    else RederiveShownState(inventory);
    return WriteResult::Written;
}

// Looked up on the instance's own class through the memoised dispatch lookup, which holds its answer by the class's
// slot and serial, so no function of a class that is gone is called.
void RederiveShownState(void* inventory) {
    if (!inventory) return;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(inventory), L"calculateVolumes");
    if (!fn) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGW("container_inventory: calculateVolumes MISSING -- applied contents will show a STALE volume");
        }
        return;
    }
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return;
    f.Set<bool>(L"countSlotBar_A", false);
    f.Set<bool>(L"updateVisible", true);
    f.Set<bool>(L"recalucateMaxVolume", true);   // the game's own spelling
    ue_wrap::Call(inventory, f);
}

void* DetachDisplay(void* inventory) {
    if (!inventory || CachedOffset(g_offDisplay, R::ClassOf(inventory), L"activeDisplayCanvas") < 0) return nullptr;
    void* const canvas = ReadAt<void*>(inventory, g_offDisplay);
    if (!canvas || !R::IsLive(canvas)) return nullptr;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(canvas), L"despawncanvas");
    if (!fn) return nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !ue_wrap::Call(canvas, f)) return nullptr;
    return canvas;
}

void AttachDisplay(void* inventory, void* canvas) {
    if (!inventory || !canvas || !R::IsLive(canvas)) return;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(inventory), L"assignCanvas");
    if (!fn) return;
    ue_wrap::ParamFrame f(fn);
    if (f.valid() && f.Set<void*>(L"activedisplaycanvas", canvas)) ue_wrap::Call(inventory, f);
}

bool CurrentVolume(void* inventory, float& out) {
    out = 0.f;
    if (!inventory || CachedOffset(g_offTotalVol, R::ClassOf(inventory), L"totalVolume") < 0) return false;
    out = ReadAt<float>(inventory, g_offTotalVol);
    return true;
}

bool MaxVolume(void* inventory, float& out) {
    out = 0.f;
    if (!inventory || CachedOffset(g_offMaxVol, R::ClassOf(inventory), L"maxVolume") < 0) return false;
    out = ReadAt<float>(inventory, g_offMaxVol);
    return true;
}

bool SetMaxVolume(void* inventory, float value) {
    if (!inventory || CachedOffset(g_offMaxVol, R::ClassOf(inventory), L"maxVolume") < 0) return false;
    std::memcpy(reinterpret_cast<uint8_t*>(inventory) + g_offMaxVol, &value, sizeof(value));
    return true;
}

// The host's world-settle edge: makeOwner sets `owner` at the component's BeginPlay and nothing clears it, so a live
// `owner` is the mark that a container's inventory has begun play.
bool InventoryInitialized(void* container) {
    void* const inv = InventoryOf(container);
    if (!inv || CachedOffset(g_offInvOwner, R::ClassOf(inv), L"owner") < 0) return false;
    void* const owner = ReadAt<void*>(inv, g_offInvOwner);
    return owner && R::IsLive(owner);
}

bool AllContainersResolved(void* gamemode) {
    return gamemode && CachedOffset(g_offAllContainers, R::ClassOf(gamemode), L"allContainers") >= 0;
}

// The gamemode's allContainers (each container adds itself at BeginPlay and leaves at its end): the
// registry the host's world-settle edge counts against. Null and non-live entries are skipped.
size_t AllContainers(void* gamemode, void** out, size_t cap) {
    if (!gamemode || !out || cap == 0) return 0;
    if (CachedOffset(g_offAllContainers, R::ClassOf(gamemode), L"allContainers") < 0) return 0;
    const SR::Arr arr = SR::ReadArr(gamemode, g_offAllContainers);
    size_t n = 0;
    for (int32_t i = 0; i < arr.num && n < cap; ++i) {
        void* const c = ReadAt<void*>(arr.data, i * static_cast<int32_t>(sizeof(void*)));
        if (c && R::IsLive(c)) out[n++] = c;
    }
    return n;
}

}  // namespace ue_wrap::container_inventory

// ue_wrap/actors/container_view.cpp -- see container_view.h.
#include "ue_wrap/actors/container_view.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>
#include <cstring>

namespace ue_wrap::container_view {
namespace {

namespace R  = reflection;
namespace II = inventory_item;

// Offsets only -- a class's layout is the same in every world -- resolved per class object, so a class a world change
// replaced resolves again. The functions go through the memoised dispatch lookup.
struct Fields {
    void* gmCls = nullptr;
    int32_t screen = -1;       // mainGamemode_C.ui_FLI_inventory
    void* screenCls = nullptr;
    int32_t component = -1;    // ui_FreeLayoutInventory_C.containerInventoryComponent
    int32_t dragging = -1;     // ui_FreeLayoutInventory_C.currentlyDragging
    int32_t massSelect = -1;   // ui_FreeLayoutInventory_C.massSelect
    int32_t player = -1;       // ui_FreeLayoutInventory_C.playerInventoryComponent
    void* activeCls = nullptr;
    int32_t activeOff = -1;    // ui_FreeLayoutInventory_C.inventoryActive, a bitfield: its byte and mask
    uint8_t activeMask = 0;
    void* itemCls = nullptr;
    int32_t itemData = -1;     // uicomp_FLI_item_C.dataReference
    int32_t itemOwner = -1;    // uicomp_FLI_item_C.ownerComponent
    int32_t itemIndex = -1;    // uicomp_FLI_item_C.index
};
Fields g;

void* PointerAt(const void* obj, int32_t off) {
    void* p = nullptr;
    if (obj && off >= 0) std::memcpy(&p, static_cast<const uint8_t*>(obj) + off, sizeof(p));
    return p;
}

// The live screen, its fields resolved.
void* ResolvedScreen() {
    void* const gm = world_singleton::Gamemode();
    if (!gm) return nullptr;
    if (void* const cls = R::ClassOf(gm); cls != g.gmCls) {
        g.gmCls = cls;
        g.screen = R::FindPropertyOffset(cls, L"ui_FLI_inventory");
    }
    void* const screen = PointerAt(gm, g.screen);
    if (!screen || !R::IsLive(screen)) return nullptr;
    if (void* const cls = R::ClassOf(screen); cls != g.screenCls) {
        g.screenCls = cls;
        g.component = R::FindPropertyOffset(cls, L"containerInventoryComponent");
        g.dragging = R::FindPropertyOffset(cls, L"currentlyDragging");
        g.massSelect = R::FindPropertyOffset(cls, L"massSelect");
        g.player = R::FindPropertyOffset(cls, L"playerInventoryComponent");
    }
    if (void* const cls = R::ClassOf(screen); cls != g.activeCls) {
        g.activeCls = cls;
        if (!R::FindBoolProperty(cls, L"inventoryActive", g.activeOff, g.activeMask)) g.activeOff = -1;
    }
    return screen;
}

bool Active(void* screen) {
    return screen && g.activeOff >= 0 && g.activeMask != 0 &&
           (*(static_cast<const uint8_t*>(screen) + g.activeOff) & g.activeMask) != 0;
}

bool CallNoArgs(void* obj, const wchar_t* name) {
    void* const fn = obj ? R::FindDispatchFunctionCached(R::ClassOf(obj), name) : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(obj, f);
}

bool ResolveItemFields(void* item) {
    if (void* const cls = R::ClassOf(item); cls != g.itemCls) {
        g.itemCls = cls;
        g.itemData = R::FindPropertyOffset(cls, L"dataReference");
        g.itemOwner = R::FindPropertyOffset(cls, L"ownerComponent");
        g.itemIndex = R::FindPropertyOffset(cls, L"index");
    }
    return g.itemData >= 0 && g.itemOwner >= 0;
}

// The same item: what an entry stands for, not where it lies on its canvas.
bool SameItem(const II::Item& a, const II::Item& b) {
    return a.classPath == b.classPath && a.name == b.name && a.tag == b.tag && a.key == b.key &&
           a.attachKey == b.attachKey && a.actorData == b.actorData && a.amount == b.amount;
}

// Is `item` a widget holding `taken`'s item in a component other than `from`?
bool HoldsCopy(void* item, const II::Item& taken, void* from) {
    if (!item || !R::IsLive(item) || !ResolveItemFields(item) || PointerAt(item, g.itemOwner) == from) return false;
    II::Item held;
    return II::Read(static_cast<const uint8_t*>(item) + g.itemData, held) && SameItem(held, taken);
}

}  // namespace

void* Screen() { return ResolvedScreen(); }

void* Viewed() {
    void* const screen = ResolvedScreen();
    if (!Active(screen)) return nullptr;
    void* const owner = container_inventory::OwnerOf(PointerAt(screen, g.component));
    return owner && container_inventory::IsContainer(owner) ? owner : nullptr;
}

void* ViewedInventory() {
    void* const screen = ResolvedScreen();
    if (!Active(screen)) return nullptr;
    void* const comp = PointerAt(screen, g.component);
    return comp && R::IsLive(comp) ? comp : nullptr;
}

bool OpenInventory(void* component) {
    void* const screen = ResolvedScreen();
    void* const fn = screen && component ? R::FindDispatchFunctionCached(R::ClassOf(screen), L"begin") : nullptr;
    ParamFrame f(fn);
    return fn && f.valid() && f.Set<void*>(L"containerInventoryComponent", component) && Call(screen, f);
}

bool DropOnViewed(void* bagItem) {
    void* const screen = ResolvedScreen();
    if (!Active(screen) || !bagItem || g.dragging < 0) return false;
    int32_t hoverOff = -1;
    uint8_t hoverMask = 0;
    if (!R::FindBoolProperty(R::ClassOf(screen), L"hoverContainer", hoverOff, hoverMask) || hoverOff < 0 || hoverMask == 0)
        return false;
    uint8_t* const hover = static_cast<uint8_t*>(screen) + hoverOff;
    std::memcpy(static_cast<uint8_t*>(screen) + g.dragging, &bagItem, sizeof(bagItem));
    *hover = static_cast<uint8_t>(*hover | hoverMask);
    const bool ran = CallNoArgs(screen, L"enddrag");
    *hover = static_cast<uint8_t>(*hover & ~hoverMask);
    return ran;
}

bool CanClose() {
    void* const screen = ResolvedScreen();
    return Active(screen) && R::FindDispatchFunctionCached(R::ClassOf(screen), L"end");
}

bool Close() {
    void* const screen = ResolvedScreen();
    return Active(screen) && CallNoArgs(screen, L"end");
}

bool Open(void* container) { return CallNoArgs(container, L"openContainer"); }

void* PlayerComponent() {
    void* const screen = ResolvedScreen();
    void* const comp = screen ? PointerAt(screen, g.player) : nullptr;
    return comp && R::IsLive(comp) ? comp : nullptr;
}

bool ReadWidget(void* widget, WidgetEntry& out) {
    if (!widget || !R::IsLive(widget) || !ResolveItemFields(widget) || g.itemIndex < 0) return false;
    if (!II::Read(static_cast<const uint8_t*>(widget) + g.itemData, out.item)) return false;
    out.owner = PointerAt(widget, g.itemOwner);
    std::memcpy(&out.index, static_cast<const uint8_t*>(widget) + g.itemIndex, sizeof(out.index));
    return true;
}

void* SourceOf(const II::Item& item) { return CopyOf(item, nullptr); }

void* CopyOf(const II::Item& taken, void* from) {
    void* const screen = ResolvedScreen();
    if (!screen || II::IsEmpty(taken)) return nullptr;
    if (void* const dragged = PointerAt(screen, g.dragging); HoldsCopy(dragged, taken, from)) return dragged;
    if (g.massSelect < 0) return nullptr;
    const save_record::Arr selected = save_record::ReadArr(screen, g.massSelect);
    for (int32_t i = 0; selected.data && i < selected.num; ++i) {
        void* const item = PointerAt(selected.data, i * static_cast<int32_t>(sizeof(void*)));
        if (HoldsCopy(item, taken, from)) return item;
    }
    return nullptr;
}

bool Discard(void* copy) {
    if (!copy || !R::IsLive(copy)) return false;
    void* const screen = ResolvedScreen();
    const bool dragging = screen && PointerAt(screen, g.dragging) == copy;
    if (!CallNoArgs(copy, L"removeItem")) return false;
    if (dragging) CallNoArgs(screen, L"dropDragged");
    return true;
}

}  // namespace ue_wrap::container_view

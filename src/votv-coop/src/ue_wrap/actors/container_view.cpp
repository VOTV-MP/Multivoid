// ue_wrap/actors/container_view.cpp -- see container_view.h.
#include "ue_wrap/actors/container_view.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>

namespace ue_wrap::container_view {
namespace {

namespace R = reflection;

// The fields the view reads, resolved per class object: a class a world change replaced resolves again.
// Offsets only -- a class's layout is the same in every world; its functions go through the memoised
// lookup, which holds them by slot and serial.
struct Fields {
    void* gmCls = nullptr;
    int32_t propInventory = -1;    // mainGamemode_C.propInventory, the inventory screen
    int32_t mainPlayer = -1;       // mainGamemode_C.mainPlayer, the local player
    int32_t playerContainer = -1;  // mainGamemode_C.playerContainer, the player's own inventory
    void* playerCls = nullptr;
    int32_t activeInterface = -1;  // mainPlayer_C.activeInterface
    void* screenCls = nullptr;
    int32_t entered = -1;   // ui_playerInventory_C.entered, the container shown
    int32_t switcher = -1;  // ui_playerInventory_C.WidgetSwitcher_54, the screen's tabs
    void* ownerCls = nullptr;  // the class of a slot's owner screen
    int32_t asProp = -1;       // ui_playerInventory_C.as_prop, the container a slot takes from
    void* slotCls = nullptr;
    int32_t slotId = -1;     // uicomp_playerInvContainerSlot_C.id, the record index
    int32_t slotOwner = -1;  // uicomp_playerInvContainerSlot_C.owner, the screen
};
Fields g;

void* PointerAt(void* obj, int32_t off) {
    return obj && off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(obj) + off) : nullptr;
}

// The running world's gamemode, its fields resolved.
void* Gamemode() {
    void* const gm = world_singleton::Gamemode();
    if (!gm) return nullptr;
    if (void* const cls = R::ClassOf(gm); cls != g.gmCls) {
        g.gmCls = cls;
        g.propInventory = R::FindPropertyOffset(cls, L"propInventory");
        g.mainPlayer = R::FindPropertyOffset(cls, L"mainPlayer");
        g.playerContainer = R::FindPropertyOffset(cls, L"playerContainer");
    }
    return gm;
}

// The inventory screen, when it is the local player's active interface.
void* OpenScreen() {
    void* const gm = Gamemode();
    void* const screen = PointerAt(gm, g.propInventory);
    void* const player = PointerAt(gm, g.mainPlayer);
    if (!screen || !player) return nullptr;
    if (void* const cls = R::ClassOf(player); cls != g.playerCls) {
        g.playerCls = cls;
        g.activeInterface = R::FindPropertyOffset(cls, L"activeInterface");
    }
    if (PointerAt(player, g.activeInterface) != screen) return nullptr;
    if (void* const cls = R::ClassOf(screen); cls != g.screenCls) {
        g.screenCls = cls;
        g.entered = R::FindPropertyOffset(cls, L"entered");
        g.switcher = R::FindPropertyOffset(cls, L"WidgetSwitcher_54");
    }
    return screen;
}

void CallNoArgs(void* obj, void* fn) {
    ParamFrame f(fn);
    if (f.valid()) Call(obj, f);
}

}  // namespace

void* Viewed() {
    return PointerAt(OpenScreen(), g.entered);
}

bool IsOwnInventory(void* container) {
    if (!container) return false;
    if (container == PointerAt(Gamemode(), g.playerContainer)) return true;
    // The screen itself tells the player's own inventory apart by its class.
    void* ownCls = object_index::ClassByName(L"prop_inventoryContainer_player_C");
    void* const cls = R::ClassOf(container);
    return ownCls && cls && R::IsDescendantOfAny(cls, &ownCls, 1);
}

void* OwnInventory() {
    void* const own = PointerAt(Gamemode(), g.playerContainer);
    return own && R::IsLive(own) ? own : nullptr;
}

bool CanClose() {
    void* const screen = OpenScreen();
    return screen && R::FindDispatchFunctionCached(R::ClassOf(screen), L"exit");
}

bool Open(void* container) {
    void* const gm = Gamemode();
    if (!gm || !container) return false;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(gm), L"openPropInv");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"prop", container);
    return Call(gm, f);
}

bool Close() {
    void* const screen = OpenScreen();
    if (!screen) return false;
    void* const cls = R::ClassOf(screen);
    void* const exitFn = R::FindDispatchFunctionCached(cls, L"exit");
    if (!exitFn) return false;
    ParamFrame f(exitFn);
    if (!f.valid() || !Call(screen, f)) return false;
    // The rest of the game's own close: the first tab again, and the selection redrawn.
    static void* s_setIndexFn = nullptr;
    if (!s_setIndexFn) {
        if (void* switcherCls = R::FindClass(L"WidgetSwitcher"))
            s_setIndexFn = R::FindFunction(switcherCls, L"SetActiveWidgetIndex");
    }
    if (void* const switcher = PointerAt(screen, g.switcher); switcher && s_setIndexFn) {
        ParamFrame idx(s_setIndexFn);
        if (idx.valid()) {
            idx.Set<int32_t>(L"Index", 0);
            Call(switcher, idx);
        }
    }
    if (void* const upd = R::FindDispatchFunctionCached(cls, L"updSelect")) CallNoArgs(screen, upd);
    return true;
}

bool ReadSlot(void* obj, Slot& out) {
    if (!obj) return false;
    void* base = object_index::ClassByName(L"uicomp_playerInvContainerSlot_C");
    void* const cls = R::ClassOf(obj);
    if (!base || !cls || !R::IsDescendantOfAny(cls, &base, 1)) return false;
    if (cls != g.slotCls) {
        g.slotCls = cls;
        g.slotId = R::FindPropertyOffset(cls, L"id");
        g.slotOwner = R::FindPropertyOffset(cls, L"owner");
    }
    if (g.slotId < 0 || g.slotOwner < 0) return false;
    void* const screen = PointerAt(obj, g.slotOwner);
    if (!screen) return false;
    void* const screenCls = R::ClassOf(screen);
    if (!screenCls) return false;
    if (screenCls != g.ownerCls) {
        g.ownerCls = screenCls;
        g.asProp = R::FindPropertyOffset(screenCls, L"as_prop");
    }
    if (g.asProp < 0) return false;
    out.screen = screen;
    out.container = PointerAt(screen, g.asProp);
    out.id = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(obj) + g.slotId);
    return true;
}

bool PressSlot(void* slot) {
    if (!slot) return false;
    void* const cls = R::ClassOf(slot);
    void* const fn = cls ? R::FindDispatchFunctionCached(cls, L"pressButton") : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(slot, f);
}

bool Redraw(void* container) {
    void* const screen = OpenScreen();
    if (!container || !screen || PointerAt(screen, g.entered) != container) return false;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(screen), L"gen_player");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"enter", container);
    f.Set<bool>(L"double", true);
    return Call(screen, f);
}

}  // namespace ue_wrap::container_view

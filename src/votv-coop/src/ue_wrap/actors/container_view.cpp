// ue_wrap/actors/container_view.cpp -- see container_view.h.
#include "ue_wrap/actors/container_view.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/facts/function_row.h"
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
    void* putCls = nullptr;    // the screen class the put fields below were read from
    int32_t putAsProp = -1;    // ui_playerInventory_C.as_prop
    int32_t putHover = -1;     // ui_playerInventory_C.hoverContainerSlot
    int32_t putTake = -1;      // ui_playerInventory_C.Button_a_take
    void* buttonCls = nullptr;  // the class of the take button
    int32_t enabledOff = -1;    // the button's bIsEnabled bitfield: its byte and mask
    uint8_t enabledMask = 0;
    void* getIndFn = nullptr;   // ui_playerInventory_C.getInd of putCls
};
Fields g;

// The call whose result is the selected record's index; its row is the fingerprint table's.
UE_FACT_FN(kRowGetInd, "container_act", L"ui_playerInventory_C", L"getInd", Declared);

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

namespace {

// A UWidget's visibility answer, its own IsVisible. False when it does not resolve or the call fails.
bool WidgetVisible(void* widget, bool& visible) {
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(widget), L"IsVisible");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !Call(widget, f)) return false;
    visible = f.Get<bool>(L"ReturnValue");
    return true;
}

// The take button's bIsEnabled, a packed bitfield: read by its mask, the class asked again when it changes.
bool ButtonEnabled(void* button, bool& enabled) {
    void* const cls = R::ClassOf(button);
    if (cls != g.buttonCls) {
        g.buttonCls = cls;
        g.enabledOff = -1;
        g.enabledMask = 0;
        if (!R::FindBoolProperty(cls, L"bIsEnabled", g.enabledOff, g.enabledMask)) g.enabledOff = -1;
    }
    if (g.enabledOff < 0 || g.enabledMask == 0) return false;
    enabled = (*(static_cast<const uint8_t*>(button) + g.enabledOff) & g.enabledMask) != 0;
    return true;
}

}  // namespace

bool ReadPutState(void* screen, PutState& out, const char** failed) {
    const char* sink = "";
    const char*& field = failed ? *failed : sink;
    field = "";
    out = PutState{};
    out.index = -1;
    if (!screen || !R::IsLive(screen)) {
        field = "the screen";
        return false;
    }
    void* const cls = R::ClassOf(screen);
    if (cls != g.putCls) {
        g.putCls = cls;
        g.putAsProp = R::FindPropertyOffset(cls, L"as_prop");
        g.putHover = R::FindPropertyOffset(cls, L"hoverContainerSlot");
        g.putTake = R::FindPropertyOffset(cls, L"Button_a_take");
        g.getIndFn = facts::ResolveOn(kRowGetInd, cls).Fn();   // once per class, not once per press
    }
    if (g.putAsProp < 0 || g.putHover < 0 || g.putTake < 0) {
        field = "as_prop, hoverContainerSlot or Button_a_take";
        return false;
    }
    void* const button = PointerAt(screen, g.putTake);
    PutState s{};
    s.index = -1;
    if (!button || !ButtonEnabled(button, s.takeEnabled)) {
        field = "Button_a_take.bIsEnabled";
        return false;
    }
    s.container = PointerAt(screen, g.putAsProp);
    void* const hover = PointerAt(screen, g.putHover);
    s.hovered = hover && R::IsLive(hover);
    if (s.hovered && !WidgetVisible(hover, s.hoverVisible)) {
        field = "hoverContainerSlot.IsVisible";
        return false;
    }
    // The selected index is asked of the game only for a press that can go on to a put.
    if (!s.hovered && s.takeEnabled) {
        if (!g.getIndFn) {
            field = "getInd";
            return false;
        }
        ParamFrame f(g.getIndFn);
        if (!f.valid() || !Call(screen, f)) {
            field = "getInd";
            return false;
        }
        s.index = f.Get<int32_t>(L"ReturnValue");
    }
    out = s;
    return true;
}

bool RunPut(void* screen) {
    if (!screen || !R::IsLive(screen)) return false;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(screen), L"em_take");
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(screen, f);
}

void* PlayerContainer() {
    void* const c = PointerAt(Gamemode(), g.playerContainer);
    return c && R::IsLive(c) ? c : nullptr;
}

}  // namespace ue_wrap::container_view

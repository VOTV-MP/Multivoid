// ue_wrap/actors/container_view.cpp -- see container_view.h.
#include "ue_wrap/actors/container_view.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>

namespace ue_wrap::container_view {
namespace {

namespace R = reflection;

// The fields the view reads, resolved per class object: a class a world change replaced resolves again.
struct Fields {
    void* gmCls = nullptr;
    int32_t propInventory = -1;    // mainGamemode_C.propInventory, the inventory screen
    int32_t mainPlayer = -1;       // mainGamemode_C.mainPlayer, the local player
    int32_t playerContainer = -1;  // mainGamemode_C.playerContainer, the player's own inventory
    void* playerCls = nullptr;
    int32_t activeInterface = -1;  // mainPlayer_C.activeInterface
    void* screenCls = nullptr;
    int32_t entered = -1;  // ui_playerInventory_C.entered, the container shown
    void* exitFn = nullptr;
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
        g.exitFn = R::FindDispatchFunction(cls, L"exit", nullptr);
    }
    return screen;
}

}  // namespace

void* Viewed() {
    return PointerAt(OpenScreen(), g.entered);
}

void* OwnContainer() {
    return PointerAt(Gamemode(), g.playerContainer);
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
    if (!screen || !g.exitFn) return false;
    ParamFrame f(g.exitFn);
    return f.valid() && Call(screen, f);
}

}  // namespace ue_wrap::container_view

// coop/dev/physmods_drill.cpp -- see coop/dev/physmods_drill.h.

#include "coop/dev/physmods_drill.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/phys_mods.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"  // DestroyActor

#include <cstdint>
#include <cstring>

namespace coop::dev::physmods_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace PM = ue_wrap::phys_mods;
namespace CD = ue_wrap::console_desk;
namespace E  = ue_wrap::engine;

// Once per process (the host keeps its process across a client's rejoin).
uint8_t g_hostByte = 0;
uint8_t g_clientByte = 0;
bool    g_plugged = false;             // this peer plugged its module
uint8_t g_afterPlug[PM::kSlots] = {};  // the host's array right after its plug
// Per session.
bool    g_done = false;                // this peer's verdict is said
bool    g_armed = false;               // the client's array at its world-ready is taken
uint8_t g_armArray[PM::kSlots] = {};

bool Holds(const uint8_t arr[PM::kSlots], uint8_t b) {
    if (!b) return false;
    for (int i = 0; i < PM::kSlots; ++i)
        if (arr[i] == b) return true;
    return false;
}

// A byte of `arr` that `base` lacks and is not `skip`, or 0.
uint8_t NewByte(const uint8_t arr[PM::kSlots], const uint8_t base[PM::kSlots], uint8_t skip) {
    for (int i = 0; i < PM::kSlots; ++i)
        if (arr[i] && arr[i] != skip && !Holds(base, arr[i])) return arr[i];
    return 0;
}

bool ReadDeskBool(void* desk, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    return R::FindBoolProperty(R::ClassOf(desk), name, off, mask) &&
           (reinterpret_cast<const uint8_t*>(desk)[off] & mask) != 0;
}

void WriteDeskBool(void* desk, const wchar_t* name, bool on) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(desk), name, off, mask)) return;
    uint8_t& b = reinterpret_cast<uint8_t*>(desk)[off];
    b = on ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
}

// plugInModule explodes a hot plug: a console active with coldswap on.
bool WouldExplode(void* desk) {
    const bool active = ReadDeskBool(desk, L"active_console") || ReadDeskBool(desk, L"active_comp") ||
                        ReadDeskBool(desk, L"active_coords") || ReadDeskBool(desk, L"active_download") ||
                        ReadDeskBool(desk, L"active_play");
    return active && ReadDeskBool(desk, L"coldswapEnabled");
}

// The desk's slot component for array index `i` (physModSlots, a TArray of primitive components).
void* SlotComponent(void* desk, int i) {
    const int32_t off = R::FindPropertyOffset(R::ClassOf(desk), L"physModSlots");
    if (off < 0) return nullptr;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(desk) + off;
    void* const* data = *reinterpret_cast<void* const* const*>(base);
    const int32_t num = *reinterpret_cast<const int32_t*>(base + 8);
    return (data && i >= 0 && i < num) ? data[i] : nullptr;
}

// Plugs a module of the lowest byte not in the array (nor `avoid`) through the desk's plugInModule, spawned
// at the desk as a carried one arrives there; a byte the desk refuses (isModuleAllowed) is passed over and
// its module destroyed. A desk that would explode the hot plug has coldswap lifted for the drill's own
// call and put back in the same tick, so the plug is the one a cold desk takes. The plugged byte, or 0.
uint8_t PlugOneOnce(void* desk, void* pawn, uint8_t avoid);
uint8_t PlugOne(void* desk, void* pawn, uint8_t avoid) {
    const bool lift = WouldExplode(desk);
    if (lift) WriteDeskBool(desk, L"coldswapEnabled", false);
    const uint8_t byte = PlugOneOnce(desk, pawn, avoid);
    if (lift) WriteDeskBool(desk, L"coldswapEnabled", true);
    return byte;
}

uint8_t PlugOneOnce(void* desk, void* pawn, uint8_t avoid) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(desk), L"plugInModule");
    ue_wrap::FVector at{};
    if (!fn || !E::TryGetActorLocation(desk, at)) return 0;
    for (int b = 1; b < 64; ++b) {
        uint8_t arr[PM::kSlots];
        if (!PM::ReadArray(arr)) return 0;
        const uint8_t byte = static_cast<uint8_t>(b);
        if (byte == avoid || Holds(arr, byte)) continue;
        void* cls = PM::ClassForByte(byte);
        if (!cls) continue;
        int freeSlot = -1;
        for (int i = 0; i < PM::kSlots && freeSlot < 0; ++i)
            if (!arr[i]) freeSlot = i;
        void* slot = freeSlot >= 0 ? SlotComponent(desk, freeSlot) : nullptr;
        if (!slot) return 0;
        void* module = E::SpawnActor(cls, {at.X, at.Y, at.Z + 120.f});
        if (!module) continue;
        ue_wrap::ParamFrame f(fn);
        const bool called = f.valid() && f.Set<void*>(L"holdActor", module) && f.Set<void*>(L"slot", slot) &&
                            f.Set<void*>(L"player", pawn) && ue_wrap::Call(desk, f);
        uint8_t after[PM::kSlots];
        if (called && PM::ReadArray(after) && Holds(after, byte)) return byte;
        if (R::IsLive(module)) E::DestroyActor(module);  // refused: not left lying at the desk
    }
    return 0;
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::physmods_drill);
    return s;
}

void Tick(coop::net::Session* s) {
    if (!IsEnabled() || g_done || !s || !s->connected()) return;
    const bool host = s->role() == coop::net::Role::Host;
    if (host ? !s->IsSlotWorldReady(1) : !coop::net_pump::HasAnnouncedWorldReady()) return;
    if (!PM::EnsureResolved()) return;
    void* desk = CD::Instance();
    void* pawn = coop::players::Registry::Get().Local();
    uint8_t arr[PM::kSlots];
    if (!desk || !pawn || !PM::ReadArray(arr)) return;

    if (host) {
        if (!g_plugged) {
            g_plugged = true;
            g_hostByte = PlugOne(desk, pawn, 0);
            PM::ReadArray(g_afterPlug);
            UE_LOGI("[PHYSMODS-DRILL] host plugged byte=%u through plugInModule%s", g_hostByte,
                    g_hostByte ? "" : " -- NO module landed");
            if (!g_hostByte) g_done = true;
            return;
        }
        g_clientByte = NewByte(arr, g_afterPlug, 0);
        if (!g_clientByte) return;
        g_done = true;
        UE_LOGI("[PHYSMODS-DRILL] host DONE the array holds its byte=%u and the client's byte=%u -- %s",
                g_hostByte, g_clientByte, Holds(arr, g_hostByte) ? "PASS" : "FAIL (its own byte is gone)");
        return;
    }

    if (g_plugged) return;
    if (!g_armed) {
        std::memcpy(g_armArray, arr, PM::kSlots);
        g_armed = true;
        // The drill starts on an empty desk, so modules already here at the joiner's world-ready are a
        // rejoin into the drill's world, written by this peer's own load: it plugs nothing and says what
        // it loaded, and the host's log shows whether any op came of that load.
        int loaded = 0;
        for (int i = 0; i < PM::kSlots; ++i) loaded += arr[i] ? 1 : 0;
        if (loaded) {
            g_done = true;
            UE_LOGI("[PHYSMODS-DRILL] client REJOIN DONE its load wrote %d module(s) (first bytes %u, %u) and "
                    "this session plugged nothing", loaded, arr[0], arr[1]);
        }
        return;
    }
    g_hostByte = NewByte(arr, g_armArray, 0);  // the host's module arrives through its canonical
    if (!g_hostByte) return;
    g_plugged = true;
    g_done = true;
    g_clientByte = PlugOne(desk, pawn, g_hostByte);
    UE_LOGI("[PHYSMODS-DRILL] client saw the host's byte=%u and plugged byte=%u through plugInModule%s",
            g_hostByte, g_clientByte, g_clientByte ? "" : " -- NO module landed");
}

void OnDisconnect() {
    g_done = false;
    g_armed = false;
}

}  // namespace coop::dev::physmods_drill

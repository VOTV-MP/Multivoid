// ue_wrap/world/bad_sun.cpp -- see ue_wrap/world/bad_sun.h.

#include "ue_wrap/world/bad_sun.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/world/active_events.h"
#include "ue_wrap/world/world_instances.h"
#include "ue_wrap/world/world_singleton.h"

#include <cstdint>

namespace ue_wrap::bad_sun {
namespace {

namespace R = ue_wrap::reflection;

constexpr const wchar_t* kClassName = L"badSun_C";
constexpr const wchar_t* kSpawnFn = L"Spawn Bad Sun";  // on mainGamemode_C

int32_t g_offWidget = -1;  // badSun_C::widget (ui_badSun_C object pointer)
bool g_resolveFailed = false;

// Two members of mainGamemode_C, each found once off its class: the class's layout is the same in every world.
struct GmMember {
    const wchar_t* name;
    int32_t off = -1;
    bool failed = false;
};
GmMember g_gmCopy{L"badSun"};          // the badSun_C object pointer
GmMember g_gmCycle{L"daynightCycle"};  // the daynightCycle_C object pointer

bool EnsureGmMember(GmMember& m, void* gamemode) {
    if (m.off >= 0) return true;
    if (m.failed) return false;
    m.off = R::FindPropertyOffset(R::ClassOf(gamemode), m.name);
    if (m.off >= 0) {
        UE_LOGI("bad_sun: resolved (mainGamemode.%ls=0x%X)", m.name, m.off);
        return true;
    }
    m.failed = true;  // a member missing from a loaded class does not appear later
    UE_LOGW("bad_sun: mainGamemode_C.%ls did not resolve -- latched OFF; game version mismatch?", m.name);
    return false;
}

// The live object a gamemode member points at (null when unset or dead); false when the member does not read.
bool ReadGmObject(GmMember& m, void* gamemode, void*& obj) {
    obj = nullptr;
    void* gm = gamemode ? gamemode : world_singleton::Gamemode();
    if (!gm || !EnsureGmMember(m, gm)) return false;
    void* p = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(gm) + m.off);
    obj = p && R::IsLive(p) ? p : nullptr;
    return true;
}

}  // namespace

bool EnsureResolved() {
    if (g_offWidget >= 0) return true;
    if (g_resolveFailed) return false;
    void* cls = object_index::ClassByName(kClassName);
    if (!cls) return false;  // not loaded: no copy exists yet
    g_offWidget = R::FindPropertyOffset(cls, L"widget");
    if (g_offWidget >= 0) {
        UE_LOGI("bad_sun: resolved (widget=0x%X)", g_offWidget);
        return true;
    }
    g_resolveFailed = true;  // a member missing from a loaded class does not appear later
    UE_LOGW("bad_sun: badSun_C.widget did not resolve on a loaded class -- latched OFF; game version mismatch?");
    return false;
}

int CountLive(void** copies, int cap) {
    if (copies)
        for (int i = 0; i < cap; ++i) copies[i] = nullptr;
    if (object_index::DrainAll() == SIZE_MAX) return -1;
    void* out[kMaxLive] = {};
    const int32_t n = world_instances::Find(kClassName, out, kMaxLive);
    if (copies)
        for (int i = 0; i < cap && i < n; ++i) copies[i] = out[i];
    return static_cast<int>(n);
}

bool ReadActive(void* copy, bool& active) {
    active = false;
    if (!copy || !EnsureResolved()) return false;
    active = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(copy) + g_offWidget) != nullptr;
    return true;
}

bool ReadWidgetAlpha(void* copy, float& alpha) {
    static R::InstanceOffset s_alpha{L"a"};  // ui_badSun_C::a
    alpha = 0.f;
    if (!copy || !EnsureResolved()) return false;
    void* widget = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(copy) + g_offWidget);
    if (!widget || !R::IsLive(widget)) return false;
    const int32_t off = s_alpha.Of(widget);
    if (off < 0) return false;
    alpha = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(widget) + off);
    return true;
}

bool ReadIsSender(void* copy, bool& sender) {
    sender = false;
    if (!copy) return false;
    active_events::Senders s{};
    if (!active_events::EnsureResolved() || !active_events::ReadSenders(s)) return false;
    for (int32_t i = 0; i < s.num; ++i)
        if (s.data[i] == copy) sender = true;
    return true;
}

bool ReadGamemodeCopy(void* gamemode, bool& present) {
    void* copy = nullptr;
    const bool read = ReadGmObject(g_gmCopy, gamemode, copy);
    present = copy != nullptr;
    return read;
}

bool ReadHasCycle(bool& has) {
    has = false;
    void* gm = world_singleton::Gamemode();
    if (!gm) return true;  // no gamemode is a world with no cycle, not an unread member
    void* cycle = nullptr;
    if (!ReadGmObject(g_gmCycle, gm, cycle)) return false;
    has = cycle != nullptr;
    return true;
}

bool CallSpawn() {
    void* gm = world_singleton::Gamemode();
    void* fn = gm ? R::FindDispatchFunctionCached(R::ClassOf(gm), kSpawnFn) : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    return Call(gm, f);
}

bool DestroyInactive(void* copy) {
    bool active = false;
    if (!copy || !ReadActive(copy, active) || active) return false;
    return engine::DestroyActor(copy);
}

}  // namespace ue_wrap::bad_sun

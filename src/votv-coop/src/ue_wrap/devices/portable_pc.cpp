// ue_wrap/devices/portable_pc.cpp -- see ue_wrap/devices/portable_pc.h.

#include "ue_wrap/devices/portable_pc.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <unordered_map>

namespace ue_wrap::portable_pc {
namespace {

namespace R = reflection;

// ClassOf verdict cache: UClass* -> is-portable-pc. NameOf runs ONCE per
// distinct class, on a cache miss only; the class itself is
// buyable so a FindClass poll would walk GUObjectArray forever (device_screen
// measured trap) -- verdicts resolve lazily from live instances instead.
std::unordered_map<void*, bool> g_verdict;
void* g_cls = nullptr;             // latched on first positive verdict
int32_t g_offOpened = -1;
void* g_fnOpen = nullptr;

void ResolveMembers(void* cls) {
    g_cls = cls;
    g_offOpened = R::FindPropertyOffset(cls, L"opened");
    if (g_offOpened < 0) { UE_LOGW("portable_pc: 'opened' offset -- fallback 0x398"); g_offOpened = 0x398; }
    g_fnOpen = R::FindFunction(cls, L"Open");
    if (!g_fnOpen)
        UE_LOGW("portable_pc: Open() not found -- lid replay disabled");
    UE_LOGI("portable_pc: resolved (opened=0x%X Open=%p)", g_offOpened, g_fnOpen);
}

}  // namespace

bool IsPortablePcClass(void* cls) {
    if (!cls) return false;
    if (cls == g_cls) return true;
    auto it = g_verdict.find(cls);
    if (it != g_verdict.end()) return it->second;
    const bool is = R::NameEquals(R::NameOf(cls), L"prop_portablePc_C");
    g_verdict.emplace(cls, is);
    if (is && !g_cls) ResolveMembers(cls);
    return is;
}

bool ReadOpened(void* actor, bool& outOpened) {
    if (!actor || g_offOpened < 0) return false;
    outOpened = reinterpret_cast<const uint8_t*>(actor)[g_offOpened] != 0;
    return true;
}

bool CallOpen(void* actor, bool opened) {
    if (!actor || !g_fnOpen) return false;
    ParamFrame f(g_fnOpen);
    if (!f.valid()) return false;
    const uint8_t b = opened ? 1 : 0;
    if (!f.SetRaw(L"opened", &b, sizeof(b))) {
        UE_LOGW("portable_pc: Open 'opened' param not found -- lid replay declined");
        return false;
    }
    return Call(actor, f);
}

void ResetCache() {
    g_verdict.clear();
    g_cls = nullptr;
    g_offOpened = -1;
    g_fnOpen = nullptr;
}

}  // namespace ue_wrap::portable_pc

// ue_wrap/devices/portable_pc.cpp -- see ue_wrap/devices/portable_pc.h.

#include "ue_wrap/devices/portable_pc.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

namespace ue_wrap::portable_pc {
namespace {

namespace R = reflection;

// `opened` on the alpha 0.9.0-n cook, used only when the property lookup fails: without it a
// recook that renames the field silently disables the lid lane instead of degrading to the last
// known layout.
constexpr int32_t kOpenedFallback = 0x398;
int32_t g_offOpened = -1;  // the class's layout, read from its first object and kept

constexpr const wchar_t* kClassName = L"prop_portablePc_C";

// prop_portablePc_C as this world holds it: kept only while its slot and serial still hold it, looked up by
// name again after a world gave the class a new object. Game thread.
CachedObjRef g_clsRef;

void ResolveMembers(void* cls) {
    g_offOpened = R::FindPropertyOffset(cls, L"opened");
    if (g_offOpened < 0) {
        UE_LOGW("portable_pc: 'opened' not found by name -- using the measured fallback 0x%X",
                kOpenedFallback);
        g_offOpened = kOpenedFallback;
    }
    UE_LOGI("portable_pc: resolved (opened=0x%X)", g_offOpened);
}

void Keep(void* cls) {
    g_clsRef.Set(cls);
    if (g_offOpened < 0) ResolveMembers(cls);
}

void* PcClass() {
    if (void* c = g_clsRef.Get()) return c;
    void* c = object_index::ClassByName(kClassName);
    if (c) Keep(c);
    return c;
}

}  // namespace

bool IsPortablePc(void* actor) {
    if (!actor) return false;
    void* const actorCls = R::ClassOf(actor);
    if (void* cls = PcClass()) return actorCls == cls;
    // The index drains a tick or more behind a load and has not listed the class yet: the actor's own
    // class answers by name, and is kept, so a PC's line is never taken for another prop's.
    if (!actorCls || !R::NameEquals(R::NameOf(actorCls), kClassName)) return false;
    Keep(actorCls);
    return true;
}

size_t ForEachPc(PcFn fn, void* ctx) {
    void* cls = PcClass();
    if (!cls || !fn) return 0;
    struct Forward { PcFn fn; void* ctx; } fwd{fn, ctx};
    return object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        const auto* f = static_cast<const Forward*>(c);
        f->fn(f->ctx, obj);
    }, &fwd);
}

bool ReadOpened(void* actor, bool& outOpened) {
    if (!actor || g_offOpened < 0) return false;
    outOpened = reinterpret_cast<const uint8_t*>(actor)[g_offOpened] != 0;
    return true;
}

bool CallOpen(void* actor, bool opened) {
    // Per call from the instance's class, by slot and serial: the class loads with its first PC and
    // can be a new object in the next world, where a kept UFunction would be the old world's.
    void* fn = actor ? R::FindDispatchFunctionCached(R::ClassOf(actor), L"open") : nullptr;
    if (!fn) {
        UE_LOGW("portable_pc: open() not found on the PC's class -- lid replay declined");
        return false;
    }
    ParamFrame f(fn);
    if (!f.valid()) return false;
    const uint8_t b = opened ? 1 : 0;
    if (!f.SetRaw(L"opened", &b, sizeof(b))) {
        UE_LOGW("portable_pc: Open 'opened' param not found -- lid replay declined");
        return false;
    }
    return Call(actor, f);
}

}  // namespace ue_wrap::portable_pc

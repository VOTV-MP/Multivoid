// ue_wrap/actors/swinger.cpp -- see ue_wrap/actors/swinger.h. Engine access for VOTV container
// lids (Aprop_swinger_C). The `opened` offset and the two verbs are resolved from the live class by
// name; a class they do not resolve on leaves the lane off, said once. The Key is read through
// ue_wrap::prop (a swinger IS an Aprop_C).

#include "ue_wrap/actors/swinger.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::swinger {
namespace {

namespace R = reflection;

std::atomic<bool> g_resolved{false};

int32_t g_openedOff  = -1;       // Aprop_swinger_C::opened
int32_t g_lockableOff = -1;      // Aprop_swinger_C::isLockable (optional: only a drill reads it)

// A swinger class whose `opened` or verbs did not resolve: said once, asked again only for another
// class object.
void* g_failedCls = nullptr;

// prop_swinger_C as this world holds it: kept only while its slot and serial still hold it, looked up by
// name again after a world gave the class a new object. Game thread.
ue_wrap::CachedObjRef g_swingerClsRef;
void* SwingerClass() {
    if (void* c = g_swingerClsRef.Get()) return c;
    void* c = ue_wrap::object_index::ClassByName(L"prop_swinger_C");
    if (c) g_swingerClsRef.Set(c);
    return c;
}

// The verb THIS swinger runs (Open(bool Damage), Close()). IsSwinger admits any descendant, and a
// UFunction handed to ProcessEvent is the body that runs -- the engine does not re-resolve it by name --
// so a base-class pointer would run the plain door's body on a fire door, which overrides both. The
// resolve is cached by the reflection layer, keyed on the class and revalidated.
void* VerbFor(void* swinger, const wchar_t* name) {
    void* cls = swinger ? R::ClassOf(swinger) : nullptr;
    return cls ? R::FindDispatchFunctionCached(cls, name) : nullptr;
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    // One object-index lookup a call until the class loads, which costs nothing while it has not.
    void* cls = SwingerClass();
    if (!cls || cls == g_failedCls) return false;

    const int32_t openedOff = R::FindPropertyOffset(cls, L"opened");
    void* openFn  = R::FindFunction(cls, L"Open");
    void* closeFn = R::FindFunction(cls, L"Close");
    if (openedOff < 0 || !openFn || !closeFn) {
        g_failedCls = cls;
        UE_LOGE("swinger: prop_swinger_C did not resolve by name (opened@%d Open=%p Close=%p) -- the container "
                "lane stays off for this class", openedOff, openFn, closeFn);
        return false;
    }

    g_openedOff  = openedOff;
    g_lockableOff = R::FindPropertyOffset(cls, L"isLockable");
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("swinger: resolved prop_swinger_C=%p opened@0x%04X Open=%p Close=%p",
            cls, openedOff, openFn, closeFn);
    return true;
}

bool IsSwinger(void* obj) {
    if (!obj || !g_resolved.load(std::memory_order_acquire)) return false;
    void* swingerCls = SwingerClass();
    void* cls = R::ClassOf(obj);
    return swingerCls && cls && R::IsDescendantOfAny(cls, &swingerCls, 1);
}

bool TryReadLockable(void* swinger, bool& lockable) {
    if (!swinger || g_lockableOff < 0) return false;
    lockable = *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(swinger) + g_lockableOff);
    return true;
}

bool TryReadOpen(void* swinger, bool& on) {
    if (!swinger || g_openedOff < 0) return false;
    on = *reinterpret_cast<const bool*>(
        reinterpret_cast<const char*>(swinger) + g_openedOff);
    return true;
}

bool CallOpen(void* swinger, bool damage) {
    void* const fn = VerbFor(swinger, L"Open");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<bool>(L"Damage", damage);
    return Call(swinger, f);
}

bool CallClose(void* swinger) {
    void* const fn = VerbFor(swinger, L"Close");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    return Call(swinger, f);
}

}  // namespace ue_wrap::swinger

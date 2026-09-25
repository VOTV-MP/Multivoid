// ue_wrap/actors/container_openers.cpp -- see container_openers.h.
#include "ue_wrap/actors/container_openers.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>

namespace ue_wrap::container_openers {
namespace {

namespace R = ue_wrap::reflection;

// One kind of opener: its class and the field naming the container it opens. The class is looked up
// on every call (one index lookup) and the field's offset kept per class object.
struct Opener {
    const wchar_t* cls;
    const wchar_t* field;
    void* resolvedCls = nullptr;
    int32_t off = -1;
};
Opener g_openers[] = {
    {L"drone_C", L"container"},
    {L"prop_dronesack_C", L"container"},
    {L"ATV_C", L"spawnedContainer"},
};

// The field's offset on `cls`, the kind's class as this world holds it.
int32_t OffsetOn(Opener& o, void* cls) {
    if (cls != o.resolvedCls) {
        o.resolvedCls = cls;
        o.off = R::FindPropertyOffset(cls, o.field);
        if (o.off < 0)
            UE_LOGW("container_openers: %ls has no '%ls' -- a container it opens is reached only where it "
                    "stands", o.cls, o.field);
    }
    return o.off;
}

void* FieldAt(void* obj, int32_t off) {
    return *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(obj) + off);
}

struct Walk {
    void* container;
    int32_t off;
    OpenerFn fn;
    void* ctx;
    bool stop;
};

}  // namespace

void ForEach(void* container, OpenerFn fn, void* ctx) {
    if (!container || !fn) return;
    for (Opener& o : g_openers) {
        void* const cls = object_index::ClassByName(o.cls);
        if (!cls) continue;  // not loaded in this world: nothing of it can open anything
        const int32_t off = OffsetOn(o, cls);
        if (off < 0) continue;
        Walk w{container, off, fn, ctx, false};
        object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t index) {
            Walk& walk = *static_cast<Walk*>(c);
            if (walk.stop) return;
            if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
            if (FieldAt(obj, walk.off) != walk.container) return;
            if (!walk.fn(walk.ctx, obj)) walk.stop = true;
        }, &w);
        if (w.stop) return;
    }
}

void* Opens(void* opener) {
    if (!opener) return nullptr;
    void* const objCls = R::ClassOf(opener);
    for (Opener& o : g_openers) {
        void* cls = object_index::ClassByName(o.cls);
        if (!cls || !objCls || !R::IsDescendantOfAny(objCls, &cls, 1)) continue;
        const int32_t off = OffsetOn(o, cls);
        return off < 0 ? nullptr : FieldAt(opener, off);
    }
    return nullptr;
}

}  // namespace ue_wrap::container_openers

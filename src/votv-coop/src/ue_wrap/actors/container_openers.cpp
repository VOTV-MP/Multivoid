// ue_wrap/actors/container_openers.cpp -- see container_openers.h.
#include "ue_wrap/actors/container_openers.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>
#include <iterator>
#include <vector>

namespace ue_wrap::container_openers {
namespace {

namespace R = ue_wrap::reflection;

// One kind of opener: its class and the field naming the container it opens, which its subclasses
// inherit at the same offset -- the ATV has five (ATV_Child_C, car1_Child_C, car1_Child1_C,
// car1_conduire_C, car1_witch_C), each opening its container through the ATV's own verb. The kind's
// class is looked up on every call (one index lookup) and the field's offset kept per class object.
struct Opener {
    const wchar_t* cls;
    const wchar_t* field;
    void* resolvedCls = nullptr;
    int32_t off = -1;
    void* builtFrom = nullptr;   // the kind's class object when `classes` was built
    std::vector<void*> classes;  // the kind's class and its descendants that have live instances
};
Opener g_openers[] = {
    {L"drone_C", L"container"},
    {L"prop_dronesack_C", L"container"},
    {L"ATV_C", L"spawnedContainer"},
};
constexpr size_t kKinds = std::size(g_openers);
uint64_t g_classSetSeen = UINT64_MAX;

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

// Each kind's live classes, rebuilt from the index's class set when that set has changed since the
// last call -- a class gaining its first instance or losing its last -- or when a kind's class is not
// the object the list was built from: a list built while the kind's class was still loading held
// nothing of it, and its finishing moves no class-set number.
void RefreshClasses() {
    const uint64_t version = object_index::ClassSetVersion();
    struct Kinds {
        void* cls[kKinds];
    } kinds{};
    bool stale = version != g_classSetSeen;
    for (size_t i = 0; i < kKinds; ++i) {
        kinds.cls[i] = object_index::ClassByName(g_openers[i].cls);
        if (kinds.cls[i] != g_openers[i].builtFrom) stale = true;
    }
    if (!stale) return;
    g_classSetSeen = version;
    for (size_t i = 0; i < kKinds; ++i) {
        g_openers[i].classes.clear();
        g_openers[i].builtFrom = kinds.cls[i];
    }
    object_index::ForEachClass([](void* ctx, void* cls, void*) {
        const Kinds& k = *static_cast<const Kinds*>(ctx);
        for (size_t i = 0; i < kKinds; ++i) {
            void* kind = k.cls[i];
            if (kind && R::IsDescendantOfAny(cls, &kind, 1)) {
                g_openers[i].classes.push_back(cls);
                return;
            }
        }
    }, &kinds);
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
    RefreshClasses();
    for (Opener& o : g_openers) {
        void* const cls = object_index::ClassByName(o.cls);
        if (!cls) continue;  // not loaded in this world: nothing of it can open anything
        const int32_t off = OffsetOn(o, cls);
        if (off < 0) continue;
        Walk w{container, off, fn, ctx, false};
        for (void* c : o.classes) {
            object_index::ForEachInstance(c, [](void* wctx, void* obj, int32_t index) {
                Walk& walk = *static_cast<Walk*>(wctx);
                if (walk.stop) return;
                if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
                if (FieldAt(obj, walk.off) != walk.container) return;
                if (!walk.fn(walk.ctx, obj)) walk.stop = true;
            }, &w);
            if (w.stop) return;
        }
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

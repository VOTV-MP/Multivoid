// ue_wrap/world/explosion.cpp -- see ue_wrap/world/explosion.h.

#include "ue_wrap/world/explosion.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/world_identity.h"

namespace ue_wrap::explosion {
namespace {

namespace R = reflection;

constexpr int kCount = static_cast<int>(Cls::Count);

constexpr const wchar_t* kNames[kCount] = {L"explosion_C", L"explosion_pc_C", L"explosion_saltrocket_C"};
constexpr const wchar_t* kPaths[kCount] = {
    L"/Game/objects/misc/explosion.explosion_C",
    L"/Game/objects/misc/explosion_pc.explosion_pc_C",
    L"/Game/objects/misc/explosion_saltrocket.explosion_saltrocket_C",
};

void*    g_cls[kCount]       = {};
bool     g_loadTried[kCount] = {};
uint64_t g_unlistVersion     = ~0ull;

Cls At(int i) { return static_cast<Cls>(i); }

void Store(int i, void* cls) {
    g_cls[i] = cls;
    UE_LOGI("[EXPLOSION] class %ls resolved", ClassName(At(i)));
}

// The one load by path this world allows the class; null when it was tried already or fails.
void* LoadOnce(int i) {
    if (g_loadTried[i]) return nullptr;
    g_loadTried[i] = true;
    void* cls = asset_load::LoadObjectByPath(kPaths[i]);
    if (!cls) UE_LOGW("[EXPLOSION] class %ls did not load from %ls; not asked again this world", ClassName(At(i)), kPaths[i]);
    return cls;
}

bool AllResolved() {
    for (int i = 0; i < kCount; ++i)
        if (!Resolved(At(i))) return false;
    return true;
}

}  // namespace

bool Resolve() {
    if (AllResolved()) return true;
    if (object_index::Backlog() != 0) return false;
    const bool gameplay = world_identity::CurrentWorldKind() == world_identity::WorldKind::Gameplay;
    for (int i = 0; i < kCount; ++i) {
        if (Resolved(At(i))) continue;
        if (gameplay) {
            ResolveNow(At(i));
        } else if (void* cls = object_index::ClassByName(ClassName(At(i)))) {
            Store(i, cls);
        }
    }
    return AllResolved();
}

void* ResolveNow(Cls c) {
    const int i = static_cast<int>(c);
    if (i < 0 || i >= kCount) return nullptr;
    if (g_cls[i]) return g_cls[i];
    void* cls = object_index::ClassByName(kNames[i]);
    if (!cls) cls = LoadOnce(i);
    if (cls) Store(i, cls);
    return cls;
}

void Reset() {
    for (int i = 0; i < kCount; ++i) {
        g_cls[i]       = nullptr;
        g_loadTried[i] = false;
    }
}

bool Resolved(Cls c) { return ClassPtr(c) != nullptr; }

void* ClassPtr(Cls c) {
    const int i = static_cast<int>(c);
    return (i >= 0 && i < kCount) ? g_cls[i] : nullptr;
}

int ClassIdOf(void* cls, bool& byName) {
    byName = false;
    if (!cls) return -1;
    const uint64_t version = object_index::ClassUnlistVersion();
    if (version != g_unlistVersion) {
        g_unlistVersion = version;
        for (int i = 0; i < kCount; ++i) g_cls[i] = nullptr;
    }
    for (int i = 0; i < kCount; ++i) {
        if (void* known = ClassPtr(At(i))) {
            if (known == cls) return i;
        } else if (R::NameEquals(R::NameOf(cls), ClassName(At(i)))) {
            byName = true;
            return i;
        }
    }
    void* up = cls;
    for (int hop = 0; hop < 2; ++hop) {
        up = R::SuperStructOf(up);
        if (!up) break;
        for (int i = 0; i < kCount; ++i)
            if (ClassPtr(At(i)) == up) return i;
    }
    return -1;
}

const wchar_t* ClassName(Cls c) {
    const int i = static_cast<int>(c);
    return (i >= 0 && i < kCount) ? kNames[i] : L"";
}

bool NarrowForceToPawn(void* explosion) {
    if (!explosion) return false;
    static R::InstanceOffset sForce{L"RadialForce"};
    const int32_t off = sForce.Of(explosion);
    if (off < 0) return false;
    void* comp = *reinterpret_cast<void**>(static_cast<uint8_t*>(explosion) + off);
    if (!comp || !R::IsLive(comp)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(comp), L"RemoveObjectTypeToAffect");
    if (!fn) return false;
    for (const uint8_t type : {uint8_t{1}, uint8_t{3}}) {
        ParamFrame f(fn);
        if (!f.Set<uint8_t>(L"ObjectType", type) || !Call(comp, f)) return false;
    }
    return true;
}

}  // namespace ue_wrap::explosion

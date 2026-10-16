// ue_wrap/world/explosion.cpp -- see ue_wrap/world/explosion.h.

#include "ue_wrap/world/explosion.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/field_io.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/sdk_profile_names.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>

namespace ue_wrap::explosion {
namespace {

namespace R = reflection;
namespace P = profile;

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

// Where each written property sits in an explosion of one class: found once per class object, before anything is
// spawned, so a property that does not resolve fails the spawn whole.
struct Layout {
    void*   cls = nullptr;
    bool    ok = false;
    int32_t damage = -1, radius = -1, force = -1, fireStrength = -1, fireFuel = -1, innerShake = -1, outerShake = -1;
    int32_t objType = -1, ignores = -1;
    int32_t ignoreWallsOff = -1, skipSettingOff = -1;
    uint8_t ignoreWallsMask = 0, skipSettingMask = 0;
};
Layout g_layout[kCount];

const Layout* LayoutFor(int i, void* cls) {
    Layout& l = g_layout[i];
    if (l.cls == cls) return l.ok ? &l : nullptr;
    l = Layout{};
    l.cls = cls;
    l.damage = R::FindPropertyOffset(cls, L"damage");
    l.radius = R::FindPropertyOffset(cls, L"radius");
    l.force = R::FindPropertyOffset(cls, L"force");
    l.fireStrength = R::FindPropertyOffset(cls, L"fireStrength");
    l.fireFuel = R::FindPropertyOffset(cls, L"fireFuel");
    l.innerShake = R::FindPropertyOffset(cls, L"innerShake");
    l.outerShake = R::FindPropertyOffset(cls, L"outerShake");
    l.objType = R::FindPropertyOffset(cls, L"objType");
    l.ignores = R::FindPropertyOffset(cls, L"ignores");
    const bool bools = R::FindBoolProperty(cls, L"ignoreWalls", l.ignoreWallsOff, l.ignoreWallsMask) &&
                       R::FindBoolProperty(cls, L"Skip Setting", l.skipSettingOff, l.skipSettingMask);
    l.ok = bools && l.damage >= 0 && l.radius >= 0 && l.force >= 0 && l.fireStrength >= 0 && l.fireFuel >= 0 &&
           l.innerShake >= 0 && l.outerShake >= 0 && l.objType >= 0 && l.ignores >= 0;
    if (!l.ok)
        UE_LOGE("[EXPLOSION] class %ls: a property to write did not resolve (damage=%d radius=%d force=%d fire=%d/%d "
                "shake=%d/%d objType=%d ignores=%d bools=%d); no copy of it is spawned",
                ClassName(At(i)), l.damage, l.radius, l.force, l.fireStrength, l.fireFuel, l.innerShake,
                l.outerShake, l.objType, l.ignores, bools ? 1 : 0);
    return l.ok ? &l : nullptr;
}

// GameplayStatics and the two spawn functions, resolved once.
void* g_gsCdo = nullptr;
void* g_beginFn = nullptr;
void* g_finishFn = nullptr;

bool ResolveSpawnFns() {
    if (!g_gsCdo) g_gsCdo = R::FindClassDefaultObject(P::name::GameplayStaticsClass);
    if (g_gsCdo) {
        void* gs = R::ClassOf(g_gsCdo);
        if (gs && !g_beginFn) g_beginFn = R::FindFunction(gs, P::name::BeginDeferredSpawnFn);
        if (gs && !g_finishFn) g_finishFn = R::FindFunction(gs, P::name::FinishSpawningActorFn);
    }
    return g_gsCdo && g_beginFn && g_finishFn;
}

// The layout for an explosion actor of one of the three classes (a child of one included); null otherwise.
const Layout* LayoutOf(void* explosion) {
    if (!explosion) return nullptr;
    void* cls = R::ClassOf(explosion);
    bool byName = false;
    const int i = ClassIdOf(cls, byName);
    return i < 0 ? nullptr : LayoutFor(i, cls);
}

float GetFloat(const uint8_t* base, int32_t off) { return *reinterpret_cast<const float*>(base + off); }

void PutFloat(uint8_t* base, int32_t off, float v) { *reinterpret_cast<float*>(base + off) = v; }

void PutBool(uint8_t* base, int32_t off, uint8_t mask, bool v) {
    if (v) base[off] |= mask;
    else base[off] &= static_cast<uint8_t>(~mask);
}

}  // namespace

bool Resolve() {
    if (AllResolved()) return true;
    if (object_index::Backlog() != 0) return false;
    for (int i = 0; i < kCount; ++i) {
        if (Resolved(At(i))) continue;
        if (void* cls = object_index::ClassByName(ClassName(At(i)))) Store(i, cls);
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

namespace {

// The explosion's RadialForce member when it is a live component, else null.
void* RadialForceOf(void* explosion) {
    if (!explosion) return nullptr;
    static R::InstanceOffset sForce{L"RadialForce"};
    const int32_t off = sForce.Of(explosion);
    if (off < 0) return nullptr;
    void* comp = *reinterpret_cast<void**>(static_cast<uint8_t*>(explosion) + off);
    return (comp && R::IsLive(comp)) ? comp : nullptr;
}

}  // namespace

bool ReadForceRadius(void* explosion, float& out) {
    void* comp = RadialForceOf(explosion);
    if (!comp) return false;
    static R::InstanceOffset sRadius{L"Radius"};
    const int32_t off = sRadius.Of(comp);
    if (off < 0) return false;
    out = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(comp) + off);
    return std::isfinite(out) && out >= 0.f;
}

bool NarrowForceToPawn(void* explosion) {
    void* comp = RadialForceOf(explosion);
    if (!comp) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(comp), L"RemoveObjectTypeToAffect");
    if (!fn) return false;
    for (const uint8_t type : {uint8_t{1}, uint8_t{3}}) {
        ParamFrame f(fn);
        if (!f.Set<uint8_t>(L"ObjectType", type) || !Call(comp, f)) return false;
    }
    return true;
}

void* SpawnDeferred(Cls c, const float loc[3], const float rot[3], const float scale[3], const Params& p,
                    void* ignoreActor) {
    const int i = static_cast<int>(c);
    void* cls = ClassPtr(c);
    if (!cls || !loc || !rot || !scale) return nullptr;
    const Layout* l = LayoutFor(i, cls);
    if (!l) return nullptr;
    if (!ResolveSpawnFns()) {
        UE_LOGE("[EXPLOSION] the spawn path on GameplayStatics did not resolve (cdo=%p begin=%p finish=%p)", g_gsCdo,
                g_beginFn, g_finishFn);
        return nullptr;
    }
    FTransform xform{};
    engine::RotatorToQuat(rot[0], rot[1], rot[2], xform.RotX, xform.RotY, xform.RotZ, xform.RotW);
    xform.TX = loc[0]; xform.TY = loc[1]; xform.TZ = loc[2];
    xform.SX = scale[0]; xform.SY = scale[1]; xform.SZ = scale[2];

    ParamFrame begin(g_beginFn);
    begin.Set<void*>(L"WorldContextObject", engine::GetWorldContext());
    begin.Set<void*>(L"ActorClass", cls);
    begin.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
    begin.Set<uint8_t>(L"CollisionHandlingOverride", 1);  // AlwaysSpawn
    begin.Set<void*>(L"Owner", nullptr);
    if (!Call(g_gsCdo, begin)) {
        UE_LOGE("[EXPLOSION] BeginDeferredActorSpawnFromClass call failed");
        return nullptr;
    }
    void* actor = begin.Get<void*>(L"ReturnValue");
    if (!actor) {
        UE_LOGW("[EXPLOSION] BeginDeferredActorSpawnFromClass returned null for class %ls", ClassName(c));
        return nullptr;
    }

    uint8_t* base = static_cast<uint8_t*>(actor);
    PutFloat(base, l->damage, p.damage);
    PutFloat(base, l->radius, p.radius);
    PutFloat(base, l->force, p.force);
    PutFloat(base, l->fireStrength, p.fireStrength);
    PutFloat(base, l->fireFuel, p.fireFuel);
    PutFloat(base, l->innerShake, p.innerShake);
    PutFloat(base, l->outerShake, p.outerShake);
    *reinterpret_cast<int32_t*>(base + l->objType) = p.objType;
    PutBool(base, l->ignoreWallsOff, l->ignoreWallsMask, p.ignoreWalls);
    PutBool(base, l->skipSettingOff, l->skipSettingMask, p.skipSetting);
    if (ignoreActor) {
        void** one = static_cast<void**>(R::EngineAlloc(sizeof(void*)));
        if (!one) {
            UE_LOGE("[EXPLOSION] the engine allocator gave no buffer for the ignores array; the copy is dropped");
            engine::DestroyActor(actor);
            return nullptr;
        }
        *one = ignoreActor;
        auto* view = reinterpret_cast<field_io::TArrayView*>(base + l->ignores);
        if (view->data) R::EngineFree(view->data);
        view->data = reinterpret_cast<uint8_t*>(one);
        view->num = 1;
        view->max = 1;
    }

    ParamFrame finish(g_finishFn);
    finish.Set<void*>(L"Actor", actor);
    finish.SetRaw(L"SpawnTransform", &xform, sizeof(xform));
    if (!Call(g_gsCdo, finish)) {
        UE_LOGE("[EXPLOSION] FinishSpawningActor call failed");
        return nullptr;
    }
    void* finished = finish.Get<void*>(L"ReturnValue");
    return finished ? finished : actor;
}

bool ReadParams(void* explosion, Params& out) {
    const Layout* l = LayoutOf(explosion);
    if (!l) return false;
    const uint8_t* base = static_cast<const uint8_t*>(explosion);
    out.damage = GetFloat(base, l->damage);
    out.radius = GetFloat(base, l->radius);
    out.force = GetFloat(base, l->force);
    out.fireStrength = GetFloat(base, l->fireStrength);
    out.fireFuel = GetFloat(base, l->fireFuel);
    out.innerShake = GetFloat(base, l->innerShake);
    out.outerShake = GetFloat(base, l->outerShake);
    out.objType = *reinterpret_cast<const int32_t*>(base + l->objType);
    out.ignoreWalls = (base[l->ignoreWallsOff] & l->ignoreWallsMask) != 0;
    out.skipSetting = (base[l->skipSettingOff] & l->skipSettingMask) != 0;
    return true;
}

bool ReadTransform(void* explosion, float loc[3], float rot[3], float scale[3]) {
    if (!explosion || !loc || !rot || !scale) return false;
    FVector at{};
    FRotator facing{};
    if (!engine::TryGetActorLocation(explosion, at) || !engine::TryGetActorRotation(explosion, facing)) return false;
    const FVector s = engine::GetActorScale3D(explosion);
    loc[0] = at.X; loc[1] = at.Y; loc[2] = at.Z;
    rot[0] = facing.Pitch; rot[1] = facing.Yaw; rot[2] = facing.Roll;
    scale[0] = s.X; scale[1] = s.Y; scale[2] = s.Z;
    return true;
}

void* FirstIgnore(void* explosion) {
    const Layout* l = LayoutOf(explosion);
    if (!l) return nullptr;
    const auto* view = reinterpret_cast<const field_io::TArrayView*>(static_cast<const uint8_t*>(explosion) + l->ignores);
    return (view->data && view->num > 0) ? *reinterpret_cast<void* const*>(view->data) : nullptr;
}

}  // namespace ue_wrap::explosion

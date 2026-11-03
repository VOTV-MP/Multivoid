// ue_wrap/actors/grime_spill.cpp -- see ue_wrap/actors/grime_spill.h.

#include "ue_wrap/actors/grime_spill.h"

#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/grime.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <cmath>
#include <cstring>

namespace ue_wrap::grime_spill {
namespace {

namespace E = engine;
namespace G = grime;
namespace R = reflection;

std::atomic<void*> g_projectileCls{nullptr};
std::atomic<void*> g_grimeCls{nullptr};
std::atomic<void*> g_bottleCls{nullptr};
std::atomic<int32_t> g_sizeOff{-1};

template <class T>
bool WriteField(void* obj, const wchar_t* name, const T& v) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    std::memcpy(static_cast<uint8_t*>(obj) + off, &v, sizeof(v));
    return true;
}

void* DecalComponent(void* decal) {
    const int32_t off = decal ? R::FindPropertyOffset(R::ClassOf(decal), L"decal") : -1;
    if (off < 0) return nullptr;
    void* comp = nullptr;
    std::memcpy(&comp, static_cast<const uint8_t*>(decal) + off, sizeof(comp));
    return comp && R::IsLive(comp) ? comp : nullptr;
}

bool Default(void* obj) { return R::NameStartsWith(R::NameOf(obj), L"Default__"); }

}  // namespace

bool EnsureResolved() {
    if (!g_projectileCls.load()) g_projectileCls.store(object_index::ClassByName(L"grimeProjectile_C"));
    if (!g_grimeCls.load()) g_grimeCls.store(object_index::ClassByName(L"grime_C"));
    if (!g_bottleCls.load()) g_bottleCls.store(object_index::ClassByName(L"prop_snack_beer_C"));
    if (g_sizeOff.load() < 0 && g_projectileCls.load())
        g_sizeOff.store(R::FindPropertyOffset(g_projectileCls.load(), L"size"));
    G::EnsureResolved();
    return g_projectileCls.load() && g_grimeCls.load();
}

bool IsProjectile(void* actor) {
    void* cls = g_projectileCls.load();
    return cls && actor && R::ClassOf(actor) == cls;
}

bool IsBottle(void* actor) {
    void* base = g_bottleCls.load();
    return base && actor && R::IsDescendantOfAny(R::ClassOf(actor), &base, 1);
}

bool ReadProjectileSize(void* projectile, float& out) {
    const int32_t off = g_sizeOff.load();
    if (!projectile || off < 0) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(projectile) + off, sizeof(out));
    return true;
}

float DefaultProjectileSize() {
    float v = -1.f;
    void* cdo = R::FindClassDefaultObject(L"grimeProjectile_C");
    return cdo && ReadProjectileSize(cdo, v) ? v : -1.f;
}

bool IsDecal(void* actor) { return actor && G::IsGrime(actor); }

bool IsDecalClass(void* cls) {
    void* base = g_grimeCls.load();
    return cls && base && R::IsDescendantOfAny(cls, &base, 1);
}

bool ReadSpill(void* decal, Spill& out) {
    if (!IsDecal(decal) || !R::IsLive(decal)) return false;
    out.className = R::ClassNameOf(decal);
    if (!E::TryGetActorLocation(decal, out.location) || !E::TryGetActorRotation(decal, out.rotation)) return false;
    out.scale = E::GetActorScale3D(decal);
    if (void* comp = DecalComponent(decal)) E::TryGetComponentWorldRotation(comp, out.decalRotation);
    G::ReadProcess(decal, out.process);
    G::ReadType(decal, out.type);
    return true;
}

void* FindSpill(const std::wstring& className, const FVector& location, float toleranceCm) {
    void* cls = object_index::ClassByName(className.c_str());
    if (!cls) return nullptr;
    struct Find {
        FVector at;
        float tolSq;
        void* found;
    } find{location, toleranceCm * toleranceCm, nullptr};
    object_index::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        auto* f = static_cast<Find*>(ctx);
        FVector l{};
        if (f->found || !R::IsLive(obj) || Default(obj) || !E::TryGetActorLocation(obj, l)) return;
        const float dx = l.X - f->at.X, dy = l.Y - f->at.Y, dz = l.Z - f->at.Z;
        if (dx * dx + dy * dy + dz * dz <= f->tolSq) f->found = obj;
    }, &find);
    return find.found;
}

void* SpawnSpill(const Spill& s) {
    void* cls = object_index::ClassByName(s.className.c_str());
    if (!IsDecalClass(cls)) return nullptr;
    void* decal = E::BeginDeferredSpawn(cls, s.location, s.rotation);
    if (!decal) return nullptr;
    E::SetActorScale3D(decal, s.scale);
    WriteField(decal, L"process", s.process);
    WriteField(decal, L"Type", s.type);
    if (!E::FinishDeferredSpawn(decal, s.location, s.rotation)) {
        E::DestroyActor(decal);
        return nullptr;
    }
    E::SetActorScale3D(decal, s.scale);
    if (void* comp = DecalComponent(decal)) E::SetComponentWorldRotation(comp, s.decalRotation);
    return decal;
}

}  // namespace ue_wrap::grime_spill

// ue_wrap/prop.cpp -- Aprop_C accessors (Stage 2 implementation).

#include "ue_wrap/prop.h"

#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <cmath>
#include <cstdint>

namespace ue_wrap::prop {
namespace {

namespace P = profile;
namespace R = reflection;

// Cached prop_C UClass -- one-shot reflection lookup, re-validated via IsLive
// each call. A full level unload + reload could either (a) keep the same
// UClass live (typical for cooked-content classes), (b) destroy it and
// re-create with a different address, or (c) destroy it and reuse the address
// for another object. The IsLive check covers (b) and partially (c): when the
// slot's InternalIndex no longer matches the cached pointer, IsLive returns
// false and we re-resolve. NOT thread-safe to WRITE; readers run on the game
// thread (the only write site is PropBaseClass itself, called only on the
// game thread by the observer / autonomous-test paths).
void* g_propBaseCls = nullptr;

void* PropBaseClass() {
    if (g_propBaseCls && R::IsLive(g_propBaseCls)) return g_propBaseCls;
    g_propBaseCls = R::FindClass(P::name::PropClass);
    return g_propBaseCls;
}

// Read raw bytes at offset; small helpers so the field offsets are the only
// thing the code references (matches the existing engine.cpp pattern).
template <typename T>
inline T ReadField(void* base, size_t off) {
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off);
}

}  // namespace

bool IsDescendantOfProp(void* obj) {
    if (!obj) return false;
    void* base = PropBaseClass();
    if (!base) return false;
    void* cls = R::ClassOf(obj);
    // Walk SuperStruct chain. ~16 hops covers the deepest VOTV BP class
    // chain; UE4's own SCENE_COMPONENT_BASE inheritance is shallower.
    for (int hops = 0; hops < 16 && cls; ++hops) {
        if (cls == base) return true;
        cls = *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(cls) + P::off::UStruct_SuperStruct);
    }
    return false;
}

R::FName GetKey(void* prop) {
    if (!prop) return R::FName{0, 0};
    return ReadField<R::FName>(prop, P::off::Aprop_Key);
}

std::wstring GetKeyString(void* prop) {
    if (!prop) return {};
    const R::FName key = GetKey(prop);
    return R::ToString(key);
}

bool IsHeavy(void* prop) {
    if (!prop) return false;
    return ReadField<bool>(prop, P::off::Aprop_propData_heavy);
}

bool IsStatic(void* prop) {
    if (!prop) return false;
    return ReadField<bool>(prop, P::off::Aprop_Static);
}

bool IsFrozen(void* prop) {
    if (!prop) return false;
    return ReadField<bool>(prop, P::off::Aprop_frozen);
}

void* GetStaticMesh(void* prop) {
    if (!prop) return nullptr;
    return ReadField<void*>(prop, P::off::Aprop_StaticMesh);
}

std::wstring GetClassName(void* prop) {
    if (!prop) return {};
    return R::ClassNameOf(prop);
}

NearestResult FindNearest(const FVector& anchor, bool wantHeavy, ScanStats* outStats) {
    NearestResult best;
    ScanStats stats;
    void* base = PropBaseClass();
    if (!base) {
        if (outStats) *outStats = stats;
        return best;
    }
    float bestD2 = 1e18f;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        ++stats.totalScanned;
        // Fast filter FIRST: super-chain walk is a few pointer compares with
        // no allocation. Most of GUObjectArray (>99% of ~237k entries) isn't
        // an Aprop_C derivative and shouldn't pay for a wstring allocation.
        if (!IsDescendantOfProp(obj)) continue;
        // Now safe to allocate: skip CDOs (Default__<Class>) -- they have no
        // world location and aren't grabbable instances. With the filter
        // order above, this allocates only ~candidate count (~2k) wstrings
        // instead of ~237k.
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;
        ++stats.candidates;
        const bool heavy = IsHeavy(obj);
        if (heavy) ++stats.totalHeavy;
        if (wantHeavy && !heavy) continue;
        // Read world location via the BP-callable K2_GetActorLocation (the
        // generic AActor path; works for any subclass without per-prop
        // logic).
        const FVector loc = engine::GetActorLocation(obj);
        const float dx = loc.X - anchor.X;
        const float dy = loc.Y - anchor.Y;
        const float dz = loc.Z - anchor.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) {
            bestD2 = d2;
            best.prop = obj;
            best.className = R::ClassNameOf(obj);
            best.heavy = heavy;
            best.isStatic = IsStatic(obj);
            best.isFrozen = IsFrozen(obj);
        }
    }
    if (best.prop) {
        best.mesh = GetStaticMesh(best.prop);
        best.keyString = GetKeyString(best.prop);
        best.dist = std::sqrt(bestD2);
    }
    if (outStats) *outStats = stats;
    return best;
}

void* FindByKeyString(const std::wstring& keyString) {
    if (keyString.empty()) return nullptr;
    void* base = PropBaseClass();
    if (!base) return nullptr;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        // Fast filter: descendant check before the string compare (string
        // compare is more expensive than pointer chain walk).
        if (!IsDescendantOfProp(obj)) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) continue;
        if (GetKeyString(obj) != keyString) continue;
        // Liveness gate: reject PendingKill / Unreachable matches. UE4 keeps
        // the dying actor in its GUObjectArray slot until GC purge -- without
        // this check we can return the OLD instance after a level reload
        // when the same Key is re-spawned on the fresh actor (same wire
        // string, fresh memory). Caller wants the LIVE one only.
        if (!R::IsLive(obj)) continue;
        return obj;
    }
    return nullptr;
}

}  // namespace ue_wrap::prop

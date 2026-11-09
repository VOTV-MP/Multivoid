// ue_wrap/actors/dig_tool.cpp -- see ue_wrap/actors/dig_tool.h.

#include "ue_wrap/actors/dig_tool.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <string>

namespace ue_wrap::dig_tool {
namespace {

namespace R = ue_wrap::reflection;

namespace E = ue_wrap::engine;
namespace OI = ue_wrap::object_index;

constexpr const wchar_t* kDigTools[] = {L"prop_shovel_C", L"prop_rake_C"};
constexpr const wchar_t* kShovel[] = {L"prop_shovel_C"};
constexpr const wchar_t* kScrapFn = L"turnIntoScrap";
constexpr const wchar_t* kHoleClass = L"dirthole_C";

template <size_t N>
bool IsOneOf(void* obj, const wchar_t* const (&names)[N]) {
    if (!obj || !R::IsLive(obj)) return false;
    void* cls = R::ClassOf(obj);
    for (int hops = 0; hops < 16 && cls; ++hops) {
        const std::wstring name = R::ToString(R::NameOf(cls));
        for (const wchar_t* n : names)
            if (name == n) return true;
        cls = R::SuperStructOf(cls);
    }
    return false;
}

void* HoleClass() {
    void* cls = OI::ClassByName(kHoleClass);
    return cls ? cls : R::FindClass(kHoleClass);
}

int32_t MatTypeOffset(void* cls) {
    static void* s_cls = nullptr;
    static int32_t s_off = -1;
    if (cls != s_cls) {
        s_cls = cls;
        s_off = cls ? R::FindPropertyOffset(cls, L"matType") : -1;
    }
    return s_off;
}

}  // namespace

bool IsDigTool(void* obj) { return IsOneOf(obj, kDigTools); }

bool IsShovel(void* obj) { return IsOneOf(obj, kShovel); }

bool IsDugHole(void* obj) {
    if (!obj || !R::IsLive(obj)) return false;
    void* cls = HoleClass();
    return cls && R::ClassOf(obj) == cls && !E::IsChildActor(obj);
}

bool ReadHoleGround(void* hole, uint8_t& matType) {
    if (!hole || !R::IsLive(hole)) return false;
    const int32_t off = MatTypeOffset(R::ClassOf(hole));
    if (off < 0) return false;
    const int32_t v = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(hole) + off);
    matType = static_cast<uint8_t>(v < 0 || v > 2 ? 0 : v);
    return true;
}

void* SpawnDugHole(const FVector& at, const FRotator& rot, uint8_t matType) {
    void* cls = HoleClass();
    if (!cls) return nullptr;
    void* hole = E::BeginDeferredSpawn(cls, at, rot);
    if (!hole) return nullptr;
    const int32_t off = MatTypeOffset(cls);
    if (off >= 0) *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(hole) + off) = matType > 2 ? 0 : matType;
    return E::FinishDeferredSpawn(hole, at, rot) ? hole : nullptr;
}

namespace {
struct NearScan {
    FVector at;
    float   r2;
    int     count = 0;
    float   best = -1.f;
    void*   nearest = nullptr;
};
void ScanOne(void* ctx, void* obj, int32_t /*index*/) {
    auto* s = static_cast<NearScan*>(ctx);
    if (!IsDugHole(obj)) return;
    FVector l{};
    if (!E::TryGetActorLocation(obj, l)) return;
    const float dx = l.X - s->at.X, dy = l.Y - s->at.Y, dz = l.Z - s->at.Z;
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > s->r2) return;
    ++s->count;
    if (s->best < 0.f || d2 < s->best) {
        s->best = d2;
        s->nearest = obj;
    }
}
}  // namespace

int CountDugHolesNear(const FVector& at, float radiusUU, void** nearest) {
    NearScan s;
    s.at = at;
    s.r2 = radiusUU * radiusUU;
    if (void* cls = HoleClass()) OI::ForEachInstance(cls, &ScanOne, &s);
    if (nearest) *nearest = s.nearest;
    return s.count;
}

bool DestroyHole(void* hole) {
    if (!hole || !R::IsLive(hole)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(hole), L"K2_DestroyActor");
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(hole, f);
}

bool TurnIntoScrap(void* pile) {
    if (!pile || !R::IsLive(pile) || !ue_wrap::prop::IsChipPile(pile)) return false;
    // Revalidated on every call, so a world unload cannot hand back a freed function.
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(pile), kScrapFn);
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && ue_wrap::Call(pile, f);
}

}  // namespace ue_wrap::dig_tool

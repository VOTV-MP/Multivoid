// ue_wrap/core/trace.cpp -- see ue_wrap/core/trace.h.

#include "ue_wrap/core/trace.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace ue_wrap::trace {
namespace {

namespace R = reflection;

// Resolved once; the KismetSystemLibrary CDO + UFunction are process-stable native
// objects -- never GC'd, latch-safe (same latch shape as kerfur.cpp's KSL latch).
void* g_kslCdo  = nullptr;  // Default__KismetSystemLibrary (the static-call dispatch object)
void* g_traceFn = nullptr;  // LineTraceSingleForObjects

// The object set: {WorldStatic, WorldDynamic} (EObjectTypeQuery1=0, 2=1;
// TEnumAsByte = 1 byte each). Static storage: the engine only READS an in-param
// TArray (const ref), it never reallocs/frees it.
uint8_t g_traceObjTypes[2] = {0, 1};
#pragma pack(push, 4)
struct TArrayControl { void* data; int32_t num; int32_t max; };
#pragma pack(pop)

// The trace, run into `f`. False when unresolvable.
bool Run(ParamFrame& f, void* worldCtx, const FVector& start, const FVector& end) {
    if (!f.valid()) return false;
    f.Set<void*>(L"WorldContextObject", worldCtx);
    f.Set<ue_wrap::FVector>(L"Start", start);
    f.Set<ue_wrap::FVector>(L"End", end);
    TArrayControl objTypes{g_traceObjTypes, 2, 2};
    f.SetRaw(L"ObjectTypes", &objTypes, sizeof(objTypes));
    f.Set<bool>(L"bTraceComplex", false);
    // ActorsToIgnore stays the zero-initialized empty TArray (frame is zeroed);
    // DrawDebugType 0 = None; OutHit is an in-frame zeroed FHitResult (POD members
    // only -- FName/floats/weak ptrs -- so no destructor concerns on our frame).
    f.Set<bool>(L"bIgnoreSelf", true);
    return ue_wrap::Call(g_kslCdo, f);
}

bool Resolved() {
    if (!g_kslCdo) g_kslCdo = R::FindClassDefaultObject(L"KismetSystemLibrary");
    if (!g_traceFn) {
        if (void* kc = R::FindClass(L"KismetSystemLibrary"))
            g_traceFn = R::FindFunction(kc, L"LineTraceSingleForObjects");
    }
    return g_traceFn && g_kslCdo;
}

// FHitResult's members, by the engine struct's own reflection, resolved once.
struct HitLayout {
    bool    tried = false, ok = false;
    int32_t size = 0, point = -1, normal = -1, actor = -1, component = -1;
};
const HitLayout& Layout() {
    static HitLayout l;
    if (l.tried) return l;
    l.tried = true;
    void* hr = R::FindObject(L"HitResult", L"ScriptStruct");
    l.size = R::FindParamSize(g_traceFn, L"OutHit");
    if (!hr || l.size <= 0) return l;
    l.point = R::FindPropertyOffset(hr, L"ImpactPoint");
    l.normal = R::FindPropertyOffset(hr, L"ImpactNormal");
    l.actor = R::FindPropertyOffset(hr, L"Actor");
    l.component = R::FindPropertyOffset(hr, L"Component");
    const auto fits = [&](int32_t off, int32_t n) { return off >= 0 && off + n <= l.size; };
    l.ok = fits(l.point, 12) && fits(l.normal, 12) && fits(l.actor, 8) && fits(l.component, 8);
    return l;
}

void* Weak(const unsigned char* at) {
    int32_t idx = 0, serial = 0;
    std::memcpy(&idx, at, 4);
    std::memcpy(&serial, at + 4, 4);
    return idx < 0 ? nullptr : R::ResolveWeakObject(idx, serial);
}

}  // namespace

int LineBlockedStatDyn(void* worldCtx, const FVector& start, const FVector& end) {
    if (!worldCtx || !Resolved()) return -1;
    ParamFrame f(g_traceFn);
    if (!Run(f, worldCtx, start, end)) return -1;
    return f.Get<bool>(L"ReturnValue") ? 1 : 0;
}

bool LineHitStatDyn(void* worldCtx, const FVector& start, const FVector& end, Hit* out) {
    if (!out || !worldCtx || !Resolved()) return false;
    const HitLayout& l = Layout();
    if (!l.ok) return false;
    ParamFrame f(g_traceFn);
    if (!Run(f, worldCtx, start, end)) return false;
    *out = Hit{};
    out->blocked = f.Get<bool>(L"ReturnValue");
    if (!out->blocked) return true;
    std::vector<unsigned char> hit(static_cast<size_t>(l.size));
    if (!f.GetRaw(L"OutHit", hit.data(), l.size)) return false;
    std::memcpy(&out->point, hit.data() + l.point, 12);
    std::memcpy(&out->normal, hit.data() + l.normal, 12);
    out->actor = Weak(hit.data() + l.actor);
    out->component = Weak(hit.data() + l.component);
    return true;
}

}  // namespace ue_wrap::trace

// ue_wrap/core/trace.cpp -- see ue_wrap/core/trace.h.

#include "ue_wrap/core/trace.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/hit_result.h"

#include <cstdint>

namespace ue_wrap::trace {
namespace {

namespace R = reflection;

// Resolved once; the KismetSystemLibrary CDO + UFunction are process-stable native
// objects -- never GC'd, latch-safe (same latch shape as kerfur.cpp's KSL latch).
void* g_kslCdo  = nullptr;  // Default__KismetSystemLibrary (the static-call dispatch object)
void* g_traceFn = nullptr;  // LineTraceSingleForObjects

// The set {WorldStatic, WorldDynamic} (EObjectTypeQuery1=0, 2=1; TEnumAsByte = 1 byte each).
constexpr uint8_t kStatDyn[2] = {0, 1};

// An in-param TArray the engine only READS (const ref): it never reallocs or frees the buffer, so the
// caller's own array backs it.
#pragma pack(push, 4)
struct TArrayControl { void* data; int32_t num; int32_t max; };
#pragma pack(pop)

// The FHitResult read buffer is on the stack, its size read from the live OutHit parameter (0x88 in 4.27).
constexpr int32_t kHitBytes = 256;

bool Resolved() {
    if (!g_kslCdo) g_kslCdo = R::FindClassDefaultObject(L"KismetSystemLibrary");
    if (!g_traceFn) {
        if (void* kc = R::FindClass(L"KismetSystemLibrary"))
            g_traceFn = R::FindFunction(kc, L"LineTraceSingleForObjects");
    }
    return g_traceFn && g_kslCdo;
}

// The trace over `types`, ignoring `ignore`, run into `f`. False when unresolvable.
bool Run(ParamFrame& f, void* worldCtx, const FVector& start, const FVector& end, const uint8_t* types,
         int32_t nTypes, void* const* ignore, int32_t nIgnore) {
    if (!f.valid()) return false;
    f.Set<void*>(L"WorldContextObject", worldCtx);
    f.Set<ue_wrap::FVector>(L"Start", start);
    f.Set<ue_wrap::FVector>(L"End", end);
    TArrayControl objTypes{const_cast<uint8_t*>(types), nTypes, nTypes};
    f.SetRaw(L"ObjectTypes", &objTypes, sizeof(objTypes));
    f.Set<bool>(L"bTraceComplex", false);
    // ActorsToIgnore stays the zero-initialized empty TArray (frame is zeroed) when nothing is named;
    // DrawDebugType 0 = None; OutHit is an in-frame zeroed FHitResult (POD members
    // only -- FName/floats/weak ptrs -- so no destructor concerns on our frame).
    if (nIgnore > 0) {
        TArrayControl ignored{const_cast<void**>(ignore), nIgnore, nIgnore};
        f.SetRaw(L"ActorsToIgnore", &ignored, sizeof(ignored));
    }
    f.Set<bool>(L"bIgnoreSelf", true);
    return ue_wrap::Call(g_kslCdo, f);
}

// The trace: 1 blocked, 0 clear, -1 unresolvable (also a bad argument, or a blocking hit whose FHitResult did
// not read). A non-null `out` is filled with the first blocking hit, and needs FHitResult's layout resolved
// before the trace runs: a hit that cannot be read answers -1, never a clear line.
int Cast(void* worldCtx, const FVector& start, const FVector& end, const uint8_t* types, int32_t nTypes,
         void* const* ignore, int32_t nIgnore, Hit* out) {
    if (!worldCtx || !types || nTypes <= 0 || nIgnore < 0 || (nIgnore > 0 && !ignore) || !Resolved()) return -1;
    static int32_t s_outHit = 0;  // the OutHit parameter's size, read once
    if (out && s_outHit <= 0) s_outHit = R::FindParamSize(g_traceFn, L"OutHit");
    const int32_t size = s_outHit;
    if (out && (size <= 0 || size > kHitBytes || !hit_result::Readable())) return -1;
    ParamFrame f(g_traceFn);
    if (!Run(f, worldCtx, start, end, types, nTypes, ignore, nIgnore)) return -1;
    const bool blocked = f.Get<bool>(L"ReturnValue");
    if (!out) return blocked ? 1 : 0;
    *out = Hit{};
    out->blocked = blocked;
    if (!blocked) return 0;
    unsigned char raw[kHitBytes];
    hit_result::Hit hit;
    if (!f.GetRaw(L"OutHit", raw, size) || !hit_result::Parse(raw, size, &hit)) return -1;
    out->point = hit.location;
    out->normal = hit.normal;
    out->actor = hit.actor;
    out->component = hit.component;
    // The engine must not report an actor it was told to ignore (or the context actor, which bIgnoreSelf ignores). One
    // reported means ActorsToIgnore did not take effect and every verdict drawn from this trace is wrong: said once.
    static bool sSaid = false;
    bool ignored = out->actor && out->actor == worldCtx;
    for (int32_t i = 0; i < nIgnore && !ignored; ++i) ignored = out->actor && out->actor == ignore[i];
    if (ignored && !sSaid) {
        sSaid = true;
        UE_LOGE("trace: the engine reported an actor on the ignore list (%p) as the first blocking hit -- "
                "ActorsToIgnore did not take effect for this trace", out->actor);
    }
    return 1;
}

}  // namespace

bool LineHitForObjects(void* worldCtx, const FVector& start, const FVector& end, const uint8_t* objectTypes,
                       int32_t nTypes, void* const* ignore, int32_t nIgnore, Hit* out) {
    return out && Cast(worldCtx, start, end, objectTypes, nTypes, ignore, nIgnore, out) >= 0;
}

int LineBlockedStatDyn(void* worldCtx, const FVector& start, const FVector& end) {
    return Cast(worldCtx, start, end, kStatDyn, 2, nullptr, 0, nullptr);
}

bool LineHitStatDyn(void* worldCtx, const FVector& start, const FVector& end, Hit* out) {
    return LineHitForObjects(worldCtx, start, end, kStatDyn, 2, nullptr, 0, out);
}

}  // namespace ue_wrap::trace

// ue_wrap/trace.cpp -- see ue_wrap/trace.h.

#include "ue_wrap/core/trace.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/reflection.h"

#include <cstdint>

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

}  // namespace

int LineBlockedStatDyn(void* worldCtx, const FVector& start, const FVector& end) {
    if (!worldCtx) return -1;
    if (!g_traceFn || !g_kslCdo) {
        if (!g_kslCdo) g_kslCdo = R::FindClassDefaultObject(L"KismetSystemLibrary");
        if (!g_traceFn) {
            if (void* kc = R::FindClass(L"KismetSystemLibrary"))
                g_traceFn = R::FindFunction(kc, L"LineTraceSingleForObjects");
        }
        if (!g_traceFn || !g_kslCdo) return -1;
    }
    ParamFrame f(g_traceFn);
    if (!f.valid()) return -1;
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
    if (!ue_wrap::Call(g_kslCdo, f)) return -1;
    return f.Get<bool>(L"ReturnValue") ? 1 : 0;
}

}  // namespace ue_wrap::trace

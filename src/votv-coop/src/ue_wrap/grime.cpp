// ue_wrap/grime.cpp -- see ue_wrap/grime.h. Engine access for VOTV surface grime (Agrime_C).
//
// Offsets are resolved from the live class via reflection (FindPropertyOffset) with the
// documented Alpha 0.9.0-n CXX-dump values as a logged fallback (version-tagging rule).

#include "ue_wrap/grime.h"

#include "ue_wrap/call.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::grime {
namespace {

namespace R = reflection;

// Resolved once at EnsureResolved, then read-only. Published via the g_resolved
// release-store / acquire-load (game-thread writes; poll/apply reads).
std::atomic<bool> g_resolved{false};

void*   g_grimeCls       = nullptr;  // grime_C UClass
int32_t g_processOff     = -1;       // Agrime_C::process    (Alpha 0.9.0-n: 0x0250)
int32_t g_typeOff        = -1;       // Agrime_C::Type       (Alpha 0.9.0-n: 0x024C)
void*   g_applyMaterialFn = nullptr; // Agrime_C::applyMaterial() -- repaint the decal at process/maxProcess

// Documented Alpha 0.9.0-n fallbacks (CXXHeaderDump/grime.hpp).
constexpr int32_t kProcessOffFallback = 0x0250;
constexpr int32_t kTypeOffFallback    = 0x024C;

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* grimeCls = R::FindClass(L"grime_C");
    if (!grimeCls) return false;  // BP class not loaded yet -- caller retries

    int32_t processOff = R::FindPropertyOffset(grimeCls, L"process");
    if (processOff < 0) {
        UE_LOGW("grime: reflected process offset not found -- using fallback 0x%04X", kProcessOffFallback);
        processOff = kProcessOffFallback;
    }
    int32_t typeOff = R::FindPropertyOffset(grimeCls, L"Type");
    if (typeOff < 0) {
        UE_LOGW("grime: reflected Type offset not found -- using fallback 0x%04X", kTypeOffFallback);
        typeOff = kTypeOffFallback;
    }
    void* applyMaterialFn = R::FindFunction(grimeCls, L"applyMaterial");
    if (!applyMaterialFn)
        UE_LOGW("grime: applyMaterial UFunction not found -- a mirrored process won't repaint the decal");

    g_grimeCls        = grimeCls;
    g_processOff      = processOff;
    g_typeOff         = typeOff;
    g_applyMaterialFn = applyMaterialFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("grime: resolved grime_C=%p process@0x%04X Type@0x%04X applyMaterial=%p",
            grimeCls, processOff, typeOff, applyMaterialFn);
    return true;
}

void* GrimeClass() { return g_grimeCls; }

bool IsGrime(void* obj) {
    if (!obj || !g_grimeCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_grimeCls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

bool ReadProcess(void* grime, float& out) {
    if (!grime || g_processOff < 0) return false;
    out = *reinterpret_cast<const float*>(
        reinterpret_cast<const char*>(grime) + g_processOff);
    return true;
}

bool ReadType(void* grime, int32_t& out) {
    if (!grime || g_typeOff < 0) return false;
    out = *reinterpret_cast<const int32_t*>(
        reinterpret_cast<const char*>(grime) + g_typeOff);
    return true;
}

bool WriteProcessAndApply(void* grime, float process) {
    if (!grime || !g_resolved.load(std::memory_order_acquire) || g_processOff < 0) return false;
    *reinterpret_cast<float*>(reinterpret_cast<char*>(grime) + g_processOff) = process;
    // applyMaterial() reads this->process + this->maxProcess and repaints the decal
    // (dynmat.SetScalarParameterValue(cleanParameter, process/maxProcess)). The field write
    // above must precede it. NOTE: it re-creates the dynamic material each call; if hands-on
    // shows that is too heavy on a fast wipe, switch to a direct dynmat SetScalarParameterValue
    // thunk (process/maxProcess) -- the lighter update clean() itself uses for the visual.
    if (g_applyMaterialFn) {
        ParamFrame f(g_applyMaterialFn);
        if (f.valid()) Call(grime, f);
    }
    return true;
}

}  // namespace ue_wrap::grime

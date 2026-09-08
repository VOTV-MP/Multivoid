// ue_wrap/grime.cpp -- see ue_wrap/grime.h. Engine access for VOTV surface grime (Agrime_C).
//
// Offsets are resolved from the live class via reflection (FindPropertyOffset) with the
// documented Alpha 0.9.0-n CXX-dump values as a logged fallback (version-tagging rule).

#include "ue_wrap/devices/grime.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

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
void*   g_applyMaterialFn = nullptr; // Agrime_C::applyMaterial() -- builds the dynamic material
int32_t g_dynmatOff      = -1;       // Agrime_C::dynmat (the decal's material instance)
int32_t g_cleanParamOff  = -1;       // Agrime_C::cleanParameter (the scalar's name, default "alpha")
int32_t g_maxProcessOff  = -1;       // Agrime_C::maxProcess (the ratio's denominator)
void*   g_setScalarFn    = nullptr;  // UMaterialInstanceDynamic::SetScalarParameterValue

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
        UE_LOGW("grime: applyMaterial UFunction not found -- a decal with no material yet cannot "
                "be given one");

    // The repaint path, which is what a mirrored wipe actually needs: the decal's own dynamic
    // material and the scalar the game drives on it.
    const int32_t dynmatOff     = R::FindPropertyOffset(grimeCls, L"dynmat");
    const int32_t cleanParamOff = R::FindPropertyOffset(grimeCls, L"cleanParameter");
    const int32_t maxProcessOff = R::FindPropertyOffset(grimeCls, L"maxProcess");
    void* setScalarFn = nullptr;
    if (void* midCls = R::FindClass(L"MaterialInstanceDynamic"))
        setScalarFn = R::FindFunction(midCls, L"SetScalarParameterValue");
    if (dynmatOff < 0 || cleanParamOff < 0 || maxProcessOff < 0 || !setScalarFn)
        UE_LOGW("grime: repaint path unresolved (dynmat=%d cleanParameter=%d maxProcess=%d "
                "SetScalarParameterValue=%p) -- mirrored wipes fall back to applyMaterial",
                dynmatOff, cleanParamOff, maxProcessOff, setScalarFn);

    g_grimeCls        = grimeCls;
    g_processOff      = processOff;
    g_typeOff         = typeOff;
    g_applyMaterialFn = applyMaterialFn;
    g_dynmatOff       = dynmatOff;
    g_cleanParamOff   = cleanParamOff;
    g_maxProcessOff   = maxProcessOff;
    g_setScalarFn     = setScalarFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("grime: resolved grime_C=%p process@0x%04X Type@0x%04X applyMaterial=%p "
            "dynmat@0x%04X cleanParameter@0x%04X maxProcess@0x%04X setScalar=%p",
            grimeCls, processOff, typeOff, applyMaterialFn,
            dynmatOff, cleanParamOff, maxProcessOff, setScalarFn);
    return true;
}

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

    // Repaint the way the game's own clean() does: set the scalar on the decal's existing
    // dynamic material. applyMaterial() is the SETUP verb, not the wipe verb -- it rebuilds the
    // material and then, whenever randomOrientation is set (the class default), takes the
    // random-rotation branch and RETURNS before it ever writes the ratio. Driving a mirrored wipe
    // through it would spin the decal to a new random angle and leave the dirt unchanged.
    if (g_dynmatOff >= 0 && g_cleanParamOff >= 0 && g_maxProcessOff >= 0 && g_setScalarFn) {
        auto* base = reinterpret_cast<char*>(grime);
        void* dynmat = *reinterpret_cast<void**>(base + g_dynmatOff);
        if (dynmat && R::IsLive(dynmat)) {
            const float maxProcess = *reinterpret_cast<const float*>(base + g_maxProcessOff);
            const R::FName param = *reinterpret_cast<const R::FName*>(base + g_cleanParamOff);
            ParamFrame f(g_setScalarFn);
            if (f.valid()) {
                f.Set<R::FName>(L"ParameterName", param);
                f.Set<float>(L"Value", maxProcess != 0.f ? process / maxProcess : 0.f);
                Call(dynmat, f);
                return true;
            }
        }
    }
    // No material on this decal yet: applyMaterial is the verb that builds one, and on that
    // path its rotation branch is the game's own intended behaviour.
    if (g_applyMaterialFn) {
        ParamFrame f(g_applyMaterialFn);
        if (f.valid()) Call(grime, f);
    }
    return true;
}

}  // namespace ue_wrap::grime

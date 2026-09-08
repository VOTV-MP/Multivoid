// ue_wrap/grime.cpp -- see ue_wrap/grime.h. Engine access for VOTV surface grime (Agrime_C).
//
// Offsets are resolved from the live class via reflection (FindPropertyOffset) with the
// documented Alpha 0.9.0-n CXX-dump values as a logged fallback (version-tagging rule).

#include "ue_wrap/devices/grime.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine_component.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::grime {
namespace {

namespace R = reflection;
namespace E = ue_wrap::engine;

// Resolved once at EnsureResolved, then read-only. Published via the g_resolved
// release-store / acquire-load (game-thread writes; poll/apply reads).
std::atomic<bool> g_resolved{false};

void*   g_grimeCls       = nullptr;  // grime_C UClass
int32_t g_processOff     = -1;       // Agrime_C::process    (Alpha 0.9.0-n: 0x0250)
int32_t g_typeOff        = -1;       // Agrime_C::Type       (Alpha 0.9.0-n: 0x024C)
void*   g_applyMaterialFn = nullptr; // Agrime_C::applyMaterial() -- builds the dynamic material
int32_t g_dynmatOff      = -1;       // Agrime_C::dynmat (the decal's material instance)
int32_t g_cleanParamOff  = -1;       // Agrime_C::cleanParameter (the scalar's name; varies by subclass)

// Fallbacks for the current game build, used only if the reflected lookup misses. grime_C is a
// blueprint class, so it is in no header dump; these came from the live class layout.
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
    // material and the name of the scalar the game drives on it. cleanParameter is read per
    // instance because subclasses disagree -- "alpha" on grime_C, "opacity" on the liquids.
    const int32_t dynmatOff     = R::FindPropertyOffset(grimeCls, L"dynmat");
    const int32_t cleanParamOff = R::FindPropertyOffset(grimeCls, L"cleanParameter");
    if (dynmatOff < 0 || cleanParamOff < 0)
        UE_LOGW("grime: repaint path unresolved (dynmat=%d cleanParameter=%d) -- a mirrored wipe "
                "cannot repaint and will report failure", dynmatOff, cleanParamOff);

    g_grimeCls        = grimeCls;
    g_processOff      = processOff;
    g_typeOff         = typeOff;
    g_applyMaterialFn = applyMaterialFn;
    g_dynmatOff       = dynmatOff;
    g_cleanParamOff   = cleanParamOff;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("grime: resolved grime_C=%p process@0x%04X Type@0x%04X applyMaterial=%p "
            "dynmat@0x%04X cleanParameter@0x%04X",
            grimeCls, processOff, typeOff, applyMaterialFn, dynmatOff, cleanParamOff);
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

// Paint the decal's dynamic material at the ratio the WIPING peer is looking at.
//
// The denominator is the literal 100, not maxProcess, because the game itself is inconsistent
// and this lane's job is parity with the author's screen: clean() -- the verb a sponge runs --
// sets the scalar to `process / 100`, while applyMaterial sets it to `process / maxProcess`.
// Those agree only while maxProcess keeps its default of 100, and two subclasses do not --
// grime_explosionScorch_C and grime_poo_C set it to 50 -- so dividing by the field would paint
// those mirrors at twice the wiper's ratio, with nothing to pull them back: the poll sees no
// further `process` delta.
bool Repaint(void* grime, float process) {
    auto* base = reinterpret_cast<char*>(grime);
    void* dynmat = *reinterpret_cast<void**>(base + g_dynmatOff);
    if (!dynmat || !R::IsLive(dynmat)) return false;
    const R::FName param = *reinterpret_cast<const R::FName*>(base + g_cleanParamOff);
    return E::SetScalarParameterValue(dynmat, param, process / 100.f);
}

bool WriteProcessAndApply(void* grime, float process) {
    if (!grime || !g_resolved.load(std::memory_order_acquire) || g_processOff < 0) return false;
    if (g_dynmatOff < 0 || g_cleanParamOff < 0) return false;  // logged once at resolve
    *reinterpret_cast<float*>(reinterpret_cast<char*>(grime) + g_processOff) = process;

    // Repaint the way the game's own clean() does, by setting the scalar on the decal's existing
    // dynamic material. applyMaterial() is the SETUP verb, not the wipe verb: it rebuilds the
    // material and then, whenever randomOrientation is set (the class default), takes the
    // random-rotation branch and RETURNS before it ever writes the ratio. Driving a mirrored wipe
    // through it spins the decal to a new random angle and leaves the dirt unchanged, so it is
    // never the last thing this function does.
    if (Repaint(grime, process)) return true;

    // No live material on this decal yet. applyMaterial is the verb that builds one -- and only
    // builds it, on the branch above -- so build, then paint. Failing to paint here would drop
    // this wipe silently: the caller records the target as applied and never retries.
    if (!g_applyMaterialFn) return false;
    ParamFrame f(g_applyMaterialFn);
    if (!f.valid() || !Call(grime, f)) return false;
    return Repaint(grime, process);
}

}  // namespace ue_wrap::grime

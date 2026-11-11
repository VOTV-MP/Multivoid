// ue_wrap/devices/windturbine.cpp -- see ue_wrap/devices/windturbine.h.

#include "ue_wrap/devices/windturbine.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>

namespace ue_wrap::windturbine {
namespace {

namespace R = reflection;

// Offsets are resolved from the class by member name. A member the name does not find leaves the
// wrapper unresolved (said once), so the turbine lane stays off rather than touching a guessed offset.
std::atomic<bool> g_resolved{false};
bool g_missing = false;             // a member is not on the class: said once, never retried

void*   g_cls = nullptr;            // windturbine_C UClass
int32_t g_offHeadRotation  = -1;    // float headRotation
int32_t g_offTargetRot     = -1;    // float targetRot
int32_t g_offRot           = -1;    // float rot (servo integrator)
int32_t g_offAlphaBlades   = -1;    // float alpha_blades
int32_t g_offBladesMomentum= -1;    // float bladesMomentum
int32_t g_offMult          = -1;    // float mult

inline float ReadF(const void* obj, int32_t off) {
    return *reinterpret_cast<const float*>(reinterpret_cast<const char*>(obj) + off);
}
inline void WriteF(void* obj, int32_t off, float v) {
    *reinterpret_cast<float*>(reinterpret_cast<char*>(obj) + off) = v;
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;
    if (g_missing) return false;
    void* cls = R::FindClass(L"windturbine_C");
    if (!cls) return false;  // BP class not loaded yet -- retry

    g_offHeadRotation   = R::FindPropertyOffset(cls, L"headRotation");
    g_offTargetRot      = R::FindPropertyOffset(cls, L"targetRot");
    g_offRot            = R::FindPropertyOffset(cls, L"rot");
    g_offAlphaBlades    = R::FindPropertyOffset(cls, L"alpha_blades");
    g_offBladesMomentum = R::FindPropertyOffset(cls, L"bladesMomentum");
    g_offMult           = R::FindPropertyOffset(cls, L"mult");
    if (g_offHeadRotation < 0 || g_offTargetRot < 0 || g_offRot < 0 || g_offAlphaBlades < 0 ||
        g_offBladesMomentum < 0 || g_offMult < 0) {
        g_missing = true;
        UE_LOGE("windturbine: members not all found by name (headRotation=%d targetRot=%d rot=%d alpha_blades=%d "
                "bladesMomentum=%d mult=%d) -- the turbine lane stays off",
                g_offHeadRotation, g_offTargetRot, g_offRot, g_offAlphaBlades, g_offBladesMomentum, g_offMult);
        return false;
    }
    g_cls = cls;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("windturbine: resolved windturbine_C=%p headRotation@0x%04X targetRot@0x%04X "
            "rot@0x%04X alpha_blades@0x%04X bladesMomentum@0x%04X mult@0x%04X",
            cls, g_offHeadRotation, g_offTargetRot, g_offRot, g_offAlphaBlades,
            g_offBladesMomentum, g_offMult);
    return true;
}

bool IsTurbine(void* obj) {
    if (!obj || !g_cls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_cls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

bool ReadState(void* turbine, State& out) {
    if (!turbine || !g_resolved.load(std::memory_order_acquire)) return false;
    out.headRotation   = ReadF(turbine, g_offHeadRotation);
    out.targetRot      = ReadF(turbine, g_offTargetRot);
    out.rot            = ReadF(turbine, g_offRot);
    out.alphaBlades    = ReadF(turbine, g_offAlphaBlades);
    out.bladesMomentum = ReadF(turbine, g_offBladesMomentum);
    out.mult           = ReadF(turbine, g_offMult);
    return true;
}

bool WriteState(void* turbine, const State& st) {
    if (!turbine || !g_resolved.load(std::memory_order_acquire)) return false;
    WriteF(turbine, g_offHeadRotation,   st.headRotation);
    WriteF(turbine, g_offTargetRot,      st.targetRot);
    WriteF(turbine, g_offRot,            st.rot);
    WriteF(turbine, g_offAlphaBlades,    st.alphaBlades);
    WriteF(turbine, g_offBladesMomentum, st.bladesMomentum);
    WriteF(turbine, g_offMult,           st.mult);
    return true;
}

}  // namespace ue_wrap::windturbine

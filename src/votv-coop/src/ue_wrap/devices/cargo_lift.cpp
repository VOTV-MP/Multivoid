// ue_wrap/devices/cargo_lift.cpp -- see ue_wrap/devices/cargo_lift.h.

#include "ue_wrap/devices/cargo_lift.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine_component.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::cargo_lift {
namespace {

namespace R = reflection;

std::atomic<bool> g_resolved{false};
int32_t g_dirOff = -1, g_movingOff = -1, g_ignoreOff = -1, g_compOff = -1;
void* g_failedCls = nullptr;

// cargoLift_C as this world holds it, looked up by name again after a world gave the class a new object.
CachedObjRef g_clsRef;
void* LiftClass() {
    if (void* c = g_clsRef.Get()) return c;
    void* c = object_index::ClassByName(L"cargoLift_C");
    if (c) g_clsRef.Set(c);
    return c;
}

bool ReadBool(void* lift, int32_t off, bool& out) {
    if (!lift || off < 0 || !IsCargoLift(lift)) return false;
    out = *(static_cast<const uint8_t*>(lift) + off) != 0;
    return true;
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;
    void* cls = LiftClass();
    if (!cls || cls == g_failedCls) return false;
    const int32_t dirOff = R::FindPropertyOffset(cls, L"dir");
    const int32_t movingOff = R::FindPropertyOffset(cls, L"moving");
    if (dirOff < 0 || movingOff < 0 || !R::FindFunction(cls, L"actionOptionIndex")) {
        g_failedCls = cls;
        UE_LOGE("cargo_lift: cargoLift_C did not resolve (dir@%d moving@%d) -- the cargo lift lane stays off for this "
                "class", dirOff, movingOff);
        return false;
    }
    g_dirOff = dirOff;
    g_movingOff = movingOff;
    g_ignoreOff = R::FindPropertyOffset(cls, L"ignoreCheck");
    g_compOff = R::FindPropertyOffset(cls, L"comp");
    if (g_ignoreOff < 0 || g_compOff < 0)
        UE_LOGW("cargo_lift: ignoreCheck@%d comp@%d -- a remote rider can still block the lift", g_ignoreOff, g_compOff);
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("cargo_lift: resolved cargoLift_C=%p dir@0x%04X moving@0x%04X (identity=level-export FName)", cls, dirOff,
            movingOff);
    return true;
}

bool IsCargoLift(void* obj) {
    if (!obj || !g_resolved.load(std::memory_order_acquire)) return false;
    void* liftCls = LiftClass();
    void* cls = R::ClassOf(obj);
    return liftCls && cls && R::IsDescendantOfAny(cls, &liftCls, 1);
}

std::wstring GetNameKey(void* lift) {
    if (!lift) return std::wstring();
    return R::ToString(R::NameOf(lift));
}

bool ReadDir(void* lift, bool& up) { return ReadBool(lift, g_dirOff, up); }

bool WriteIgnoreCheck(void* lift, bool on) {
    if (!lift || g_ignoreOff < 0 || !IsCargoLift(lift)) return false;
    *(static_cast<uint8_t*>(lift) + g_ignoreOff) = on ? 1 : 0;
    return true;
}

bool ReadIgnoreCheck(void* lift, bool& on) { return ReadBool(lift, g_ignoreOff, on); }

bool PlatformLocation(void* lift, FVector& out) {
    if (!lift || g_compOff < 0 || !IsCargoLift(lift)) return false;
    void* const comp = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(lift) + g_compOff);
    return comp && R::IsLive(comp) && ue_wrap::engine::TryGetComponentLocation(comp, out);
}

bool ReadMoving(void* lift, bool& moving) { return ReadBool(lift, g_movingOff, moving); }

bool WriteDir(void* lift, bool up) {
    if (!lift || g_dirOff < 0 || !IsCargoLift(lift)) return false;
    *(static_cast<uint8_t*>(lift) + g_dirOff) = up ? 1 : 0;
    return true;
}

bool Press(void* lift) {
    if (!lift || !IsCargoLift(lift)) return false;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(lift), L"actionOptionIndex");
    ParamFrame pf(fn);
    return fn && pf.valid() && pf.Set<uint8_t>(L"action", kPressAction) && Call(lift, pf);
}

}  // namespace ue_wrap::cargo_lift

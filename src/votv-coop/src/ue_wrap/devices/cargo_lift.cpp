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
int32_t g_dirOff = -1, g_movingOff = -1;
// The measure fields, optional: -1 when the class lacks one (its reader then returns false).
int32_t g_attachOff = -1, g_compOff = -1, g_ignoreOff = -1, g_speedOff = -1;
uint8_t g_ignoreMask = 0;
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

void* ReadObj(void* lift, int32_t off) {
    if (!lift || off < 0 || !IsCargoLift(lift)) return nullptr;
    void* const o = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(lift) + off);
    return o && R::IsLive(o) ? o : nullptr;
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
    g_attachOff = R::FindPropertyOffset(cls, L"attach");
    g_compOff = R::FindPropertyOffset(cls, L"comp");
    g_speedOff = R::FindPropertyOffset(cls, L"speed");
    int32_t ignoreOff = -1;
    uint8_t ignoreMask = 0;
    if (R::FindBoolProperty(cls, L"ignoreCheck", ignoreOff, ignoreMask) && ignoreOff >= 0 && ignoreMask != 0) {
        g_ignoreOff = ignoreOff;
        g_ignoreMask = ignoreMask;
    }
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("cargo_lift: resolved cargoLift_C=%p dir@0x%04X moving@0x%04X (identity=level-export FName); measure fields "
            "attach@%d comp@%d ignoreCheck@%d mask 0x%02X", cls, dirOff, movingOff, g_attachOff, g_compOff, g_ignoreOff,
            g_ignoreMask);
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

bool ReadIgnoreCheck(void* lift, bool& ignore) {
    if (!lift || g_ignoreOff < 0 || !IsCargoLift(lift)) return false;
    ignore = (*(static_cast<const uint8_t*>(lift) + g_ignoreOff) & g_ignoreMask) != 0;
    return true;
}

void* PlatformComponent(void* lift) { return ReadObj(lift, g_compOff); }

void* PlatformActor(void* lift) {
    void* const comp = PlatformComponent(lift);
    void* const owner = comp ? R::OuterOf(comp) : nullptr;
    return owner && R::IsLive(owner) ? owner : nullptr;
}

bool ReadAttachZ(void* lift, float& z) {
    FVector at{};
    void* const attach = ReadObj(lift, g_attachOff);
    if (!attach || !engine::TryGetComponentLocation(attach, at)) return false;
    z = at.Z;
    return true;
}

bool ReadStopsZ(void* lift, float& lowZ, float& highZ) {
    ue_wrap::FVector at{};
    void* const attach = ReadObj(lift, g_attachOff);
    if (!attach || !engine::TryGetComponentLocation(attach, at)) return false;
    lowZ = at.Z - engine::GetComponentRelativeLocation(attach).Z;
    highZ = lowZ + kTravelCm;
    return true;
}

bool ReadMaxSpeedCmS(void* lift, float& cmPerSec) {
    if (!lift || g_speedOff < 0 || !IsCargoLift(lift)) return false;
    const float speed = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(lift) + g_speedOff);
    if (!(speed > 0.f)) return false;
    cmPerSec = 2.f * kTravelCm / speed;
    return true;
}

bool ReadPlatformZ(void* lift, float& z) {
    FVector at{};
    void* const comp = PlatformComponent(lift);
    if (!comp || !engine::TryGetComponentLocation(comp, at)) return false;
    z = at.Z;
    return true;
}

}  // namespace ue_wrap::cargo_lift

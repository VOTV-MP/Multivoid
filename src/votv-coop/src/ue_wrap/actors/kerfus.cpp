// ue_wrap/actors/kerfus.cpp -- see ue_wrap/actors/kerfus.h.

#include "ue_wrap/actors/kerfus.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

namespace ue_wrap::kerfus {
namespace {

namespace OI = ue_wrap::object_index;
namespace R  = ue_wrap::reflection;

// A group's latch: 0 not tried, 1 resolved, -1 a field missing (said once).
struct State {
    int latch = 0;
};

// On, charging, energy: what the state sync reads and writes.
struct StateFields {
    State   s;
    int32_t activeOff = -1, chargingOff = -1, energyOff = -1;
    uint8_t activeMask = 0, chargingMask = 0;
};
// The task target and the possess point: what the path goal's answer reads.
struct TaskFields {
    State   s;
    int32_t moveToOff = -1, possessOff = -1;
};
StateFields g_state;
TaskFields  g_task;

// Latches `s` on the result and names a missing field once.
bool Settle(State& s, bool ok, const char* missing, const char* group) {
    s.latch = ok ? 1 : -1;
    if (!ok)
        UE_LOGW("kerfus: the Kerfus's `%s` did not resolve -- its %s are not read or written this process", missing,
                group);
    return ok;
}

bool ResolveState(void* k) {
    if (g_state.s.latch != 0) return g_state.s.latch > 0;
    if (!k || !IsKerfus(k)) return false;  // not tried: a Kerfus names the layout
    void* cls = R::ClassOf(k);
    StateFields f;
    const char* missing = nullptr;
    if (!R::FindBoolProperty(cls, L"active", f.activeOff, f.activeMask)) missing = "active";
    else if (!R::FindBoolProperty(cls, L"charging", f.chargingOff, f.chargingMask)) missing = "charging";
    else if ((f.energyOff = R::FindPropertyOffset(cls, L"energy")) < 0) missing = "energy";
    f.s = g_state.s;
    g_state = f;
    return Settle(g_state.s, missing == nullptr, missing, "on, charging and energy");
}

bool ResolveTask(void* k) {
    if (g_task.s.latch != 0) return g_task.s.latch > 0;
    if (!k || !IsKerfus(k)) return false;
    void* cls = R::ClassOf(k);
    TaskFields f;
    const char* missing = nullptr;
    if ((f.moveToOff = R::FindPropertyOffset(cls, L"moveTo")) < 0) missing = "moveTo";
    else if ((f.possessOff = R::FindPropertyOffset(cls, L"possessLoc")) < 0) missing = "possessLoc";
    f.s = g_task.s;
    g_task = f;
    return Settle(g_task.s, missing == nullptr, missing, "task and possess point");
}

uint8_t* At(void* k, int32_t off) { return static_cast<uint8_t*>(k) + off; }

bool ReadBit(void* k, int32_t off, uint8_t mask, bool& v) {
    v = (*At(k, off) & mask) != 0;
    return true;
}

bool WriteBit(void* k, int32_t off, uint8_t mask, bool v) {
    uint8_t& b = *At(k, off);
    b = static_cast<uint8_t>(v ? (b | mask) : (b & ~mask));
    return true;
}

}  // namespace

bool IsKerfus(void* obj) {
    void* kerfus = OI::ClassByName(kClassName);
    void* cls = obj ? R::ClassOf(obj) : nullptr;
    return kerfus && cls && R::IsDescendantOfAny(cls, &kerfus, 1);
}

bool ReadActive(void* k, bool& on) {
    return ResolveState(k) && ReadBit(k, g_state.activeOff, g_state.activeMask, on);
}
bool WriteActive(void* k, bool on) {
    return ResolveState(k) && WriteBit(k, g_state.activeOff, g_state.activeMask, on);
}
bool ReadCharging(void* k, bool& on) {
    return ResolveState(k) && ReadBit(k, g_state.chargingOff, g_state.chargingMask, on);
}
bool WriteCharging(void* k, bool on) {
    return ResolveState(k) && WriteBit(k, g_state.chargingOff, g_state.chargingMask, on);
}

bool ReadEnergy(void* k, float& energy) {
    if (!ResolveState(k)) return false;
    energy = *reinterpret_cast<const float*>(At(k, g_state.energyOff));
    return true;
}

bool WriteEnergy(void* k, float energy) {
    if (!ResolveState(k)) return false;
    *reinterpret_cast<float*>(At(k, g_state.energyOff)) = energy;
    return true;
}

void* ValidTask(void* k) {
    if (!ResolveTask(k)) return nullptr;
    void* t = *reinterpret_cast<void* const*>(At(k, g_task.moveToOff));
    return (t && R::IsLive(t)) ? t : nullptr;
}

bool ReadPossessLoc(void* k, FVector& at) {
    if (!ResolveTask(k)) return false;
    at = *reinterpret_cast<const FVector*>(At(k, g_task.possessOff));
    return true;
}

int32_t TargetActorPossessOut(void* fn) {
    static void*   sFn = nullptr;
    static int32_t sOff = -1;
    if (fn != sFn) {
        sFn = fn;
        sOff = fn ? R::FindParamOffset(fn, L"possessLoc") : -1;
    }
    return sOff;
}

bool RunUpd(void* k, bool skipFace) {
    void* fn = k ? R::FindDispatchFunctionCached(R::ClassOf(k), L"upd") : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<bool>(L"skipFace", skipFace) && ue_wrap::Call(k, f);
}

bool RunActionOptionIndex(void* k, void* player, uint8_t action) {
    void* fn = k ? R::FindDispatchFunctionCached(R::ClassOf(k), L"actionOptionIndex") : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<void*>(L"player", player) && f.Set<uint8_t>(L"action", action) &&
           ue_wrap::Call(k, f);
}

bool RunPossess(void* k, const FVector& at) {
    void* fn = k ? R::FindDispatchFunctionCached(R::ClassOf(k), L"possess") : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    return f.valid() && f.Set<FVector>(L"possessLoc", at) && ue_wrap::Call(k, f);
}

}  // namespace ue_wrap::kerfus

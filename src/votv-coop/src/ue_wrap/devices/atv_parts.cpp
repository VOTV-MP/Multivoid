// ue_wrap/devices/atv_parts.cpp -- see ue_wrap/devices/atv_parts.h.

#include "ue_wrap/devices/atv_parts.h"

#include "ue_wrap/actors/item_spawn.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

namespace ue_wrap::atv_parts {
namespace {

namespace R = ue_wrap::reflection;

bool g_resolved = false;
bool g_tried = false;

// ATV_C.
int32_t g_offLookTire = -1, g_offLookSocket = -1, g_offLookSpare = -1, g_offLookContainer = -1;
void* g_fnUnscrewPanel = nullptr;
void* g_fnGetUnscrewSpawn = nullptr;
void* g_fnResetUnscrew = nullptr;

// prop_atvWheel_C, an import of ATV_C: held through a CachedObjRef, which finds the class again once its slot dies.
constexpr wchar_t kWheelClass[] = L"prop_atvWheel_C";
CachedObjRef g_wheelCls;
int32_t g_offWheelDur = -1, g_offWheelDirt = -1, g_offWheelFixes = -1;

// The wheel's class for an identity test: the object index only, never a load.
void* WheelClass() {
    if (!g_wheelCls.Alive()) g_wheelCls.Set(ue_wrap::object_index::ClassByName(kWheelClass));
    return g_wheelCls.Raw();
}

template <class T>
T& Field(void* obj, int32_t off) {
    return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(obj) + off);
}

bool IsOfClass(void* actor, void* cls) {
    void* c = (actor && cls) ? R::ClassOf(actor) : nullptr;
    return c && (c == cls || R::IsDescendantOfAny(c, &cls, 1));
}

void StampWheel(void* ctx, void* actor) {
    const Wheel& w = *static_cast<const Wheel*>(ctx);
    Field<float>(actor, g_offWheelDur) = w.durability;
    Field<float>(actor, g_offWheelDirt) = w.dirt;
    Field<int32_t>(actor, g_offWheelFixes) = w.fixes;
}

}  // namespace

namespace {

// `loadWheel` loads the wheel's class when this process has not (only the host's refund needs that); otherwise the
// class is the object index's answer and a missing one is asked for again.
bool ResolveImpl(bool loadWheel) {
    if (g_resolved) return true;
    if (g_tried) return false;
    // A class not loaded yet is asked for again; a loaded one that lacks a member will not grow it.
    void* atv = ue_wrap::object_index::ClassByName(L"ATV_C");
    if (!atv) return false;
    if (loadWheel && !g_wheelCls.Alive()) g_wheelCls.Set(ue_wrap::item_spawn::LoadedClass(kWheelClass));
    void* const wheelCls = WheelClass();
    if (!wheelCls) return false;
    g_tried = true;
    g_offLookTire      = R::FindPropertyOffset(atv, L"lookAtTire");
    g_offLookSocket    = R::FindPropertyOffset(atv, L"lookAtTireSocket");
    g_offLookSpare     = R::FindPropertyOffset(atv, L"lookAtSpareTireBox");
    g_offLookContainer = R::FindPropertyOffset(atv, L"lookAtContainer");
    g_fnUnscrewPanel    = R::FindFunction(atv, L"unscrewPanel");
    g_fnGetUnscrewSpawn = R::FindFunction(atv, L"getUnscrewSpawn");
    g_fnResetUnscrew    = R::FindFunction(atv, L"resetUnscrew");
    g_offWheelDur      = R::FindPropertyOffset(wheelCls, L"durability");
    g_offWheelDirt     = R::FindPropertyOffset(wheelCls, L"dirt");
    g_offWheelFixes    = R::FindPropertyOffset(wheelCls, L"fixes");
    const bool atvOk = g_offLookTire >= 0 && g_offLookSocket >= 0 && g_offLookSpare >= 0 &&
                       g_offLookContainer >= 0 && g_fnUnscrewPanel && g_fnGetUnscrewSpawn && g_fnResetUnscrew;
    const bool partsOk = g_offWheelDur >= 0 && g_offWheelDirt >= 0 && g_offWheelFixes >= 0;
    g_resolved = atvOk && partsOk;
    if (!g_resolved)
        UE_LOGE("atv_parts: resolve INCOMPLETE (ATV_C ok=%d, part props ok=%d) -- a client's wheel "
                "work stays local this session", atvOk ? 1 : 0, partsOk ? 1 : 0);
    return g_resolved;
}

}  // namespace

bool Resolve() { return ResolveImpl(false); }

bool ReadLook(void* atv, Look& out) {
    if (!g_resolved || !atv) return false;
    out.tire = Field<int32_t>(atv, g_offLookTire);
    out.socket = Field<int32_t>(atv, g_offLookSocket);
    out.spareBox = Field<uint8_t>(atv, g_offLookSpare) != 0;
    out.container = Field<uint8_t>(atv, g_offLookContainer) != 0;
    return true;
}

bool IsWheel(void* actor) { return g_resolved && IsOfClass(actor, WheelClass()); }

bool ReadWheel(void* wheel, Wheel& out) {
    if (!IsWheel(wheel)) return false;
    out.durability = Field<float>(wheel, g_offWheelDur);
    out.dirt = Field<float>(wheel, g_offWheelDirt);
    out.fixes = Field<int32_t>(wheel, g_offWheelFixes);
    return true;
}

bool RunUnscrew(void* atv, int tire) {
    if (!g_resolved || !atv) return false;
    const int32_t tireWas = Field<int32_t>(atv, g_offLookTire);
    const int32_t socketWas = Field<int32_t>(atv, g_offLookSocket);
    Field<int32_t>(atv, g_offLookTire) = tire;
    Field<int32_t>(atv, g_offLookSocket) = -1;
    bool ok = false;
    ue_wrap::ParamFrame spawn(g_fnGetUnscrewSpawn);
    if (spawn.valid() && ue_wrap::Call(atv, spawn)) {
        void* const at = spawn.Get<void*>(L"ReturnValue");
        ue_wrap::ParamFrame panel(g_fnUnscrewPanel);
        ok = panel.valid() && panel.Set<void*>(L"spawnAt", at) && ue_wrap::Call(atv, panel);
    }
    Field<int32_t>(atv, g_offLookTire) = tireWas;
    Field<int32_t>(atv, g_offLookSocket) = socketWas;
    return ok;
}

bool ResetUnscrew(void* atv) {
    if (!g_resolved || !atv) return false;
    ue_wrap::ParamFrame f(g_fnResetUnscrew);
    return f.valid() && ue_wrap::Call(atv, f);
}

void* SpawnWheel(const FVector& at, const Wheel& w) {
    // The refund spawns a wheel the host may never have held, so its class is loaded here, and Resolve is finished
    // with it; a client's hooks only look the class up.
    if (!g_resolved && !ResolveImpl(true)) return nullptr;
    Wheel copy = w;
    return ue_wrap::item_spawn::Spawn(kWheelClass, at, &StampWheel, &copy);
}

}  // namespace ue_wrap::atv_parts

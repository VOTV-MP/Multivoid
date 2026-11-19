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
StateFields g_state;

// The navigation pawn: its `pawn`.
struct PawnField {
    State   s;
    int32_t pawnOff = -1;
};
PawnField g_pawn;

// The wheel: the actor (`wheelObject`), its body (`wheelComponent`) and the child-actor slot that spawned it (`wheel`).
struct WheelFields {
    State   s;
    int32_t objectOff = -1, compOff = -1, slotOff = -1;
};
WheelFields g_wheel;

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

bool ResolvePawn(void* k) {
    if (g_pawn.s.latch != 0) return g_pawn.s.latch > 0;
    if (!k || !IsKerfus(k)) return false;  // not tried: a Kerfus names the layout
    g_pawn.pawnOff = R::FindPropertyOffset(R::ClassOf(k), L"pawn");
    return Settle(g_pawn.s, g_pawn.pawnOff >= 0, "pawn", "navigation pawn");
}

bool ResolveWheel(void* k) {
    if (g_wheel.s.latch != 0) return g_wheel.s.latch > 0;
    if (!k || !IsKerfus(k)) return false;  // not tried: a Kerfus names the layout
    void* const cls = R::ClassOf(k);
    const char* missing = nullptr;
    if ((g_wheel.objectOff = R::FindPropertyOffset(cls, L"wheelObject")) < 0) missing = "wheelObject";
    else if ((g_wheel.compOff = R::FindPropertyOffset(cls, L"wheelComponent")) < 0) missing = "wheelComponent";
    else if ((g_wheel.slotOff = R::FindPropertyOffset(cls, L"wheel")) < 0) missing = "wheel";
    return Settle(g_wheel.s, missing == nullptr, missing, "wheel parts");
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

void* FindLive(bool (*accept)(void* obj, const void* arg), const void* arg) {
    // The Kerfus and its colour variants, each an exact class to the object index.
    static constexpr const wchar_t* kClasses[] = {
        L"p_kerfus_C", L"p_kerfus_p_C", L"p_kerfus_r_C", L"p_kerfus_y_C", L"p_kerfus_col_C", L"p_kerfus_col_gamer_C",
    };
    for (const wchar_t* name : kClasses) {
        void* cls = OI::ClassByName(name);
        if (!cls) continue;
        struct Ctx { bool (*accept)(void*, const void*); const void* arg; void* found; } ctx{accept, arg, nullptr};
        OI::ForEachInstance(cls, [](void* p, void* obj, int32_t index) {
            auto* c = static_cast<Ctx*>(p);
            if (c->found || !obj) return;
            if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
            if (!c->accept || c->accept(obj, c->arg)) c->found = obj;
        }, &ctx);
        if (ctx.found) return ctx.found;
    }
    return nullptr;
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

void* NavPawn(void* k) {
    if (!ResolvePawn(k)) return nullptr;
    void* pawn = *reinterpret_cast<void* const*>(At(k, g_pawn.pawnOff));
    return (pawn && R::IsLive(pawn)) ? pawn : nullptr;
}

bool ReadWheel(void* k, Wheel& out) {
    out = Wheel{};
    if (!ResolveWheel(k)) return false;
    auto live = [](void* p) { return (p && R::IsLive(p)) ? p : nullptr; };
    out.actor = live(*reinterpret_cast<void* const*>(At(k, g_wheel.objectOff)));
    out.body = live(*reinterpret_cast<void* const*>(At(k, g_wheel.compOff)));
    out.slot = live(*reinterpret_cast<void* const*>(At(k, g_wheel.slotOff)));
    return out.actor && out.body && out.slot;
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

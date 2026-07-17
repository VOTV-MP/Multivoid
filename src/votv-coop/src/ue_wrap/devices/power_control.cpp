// ue_wrap/power_control.cpp -- see ue_wrap/power_control.h. Engine access for the base POWER
// PANEL (ApowerControl_C). Offsets/verbs resolved from the live class via reflection
// (version-portable); the Alpha 0.9.0-n values are logged fallbacks.
//
// The APPLY follows the RE doc's "Recipe A" (votv-powerControl-panel-sync-RE-2026-06-08.md):
// mirror the panel's OWN visual (lever positions + LED particles) and nothing downstream. The
// native buttonsVisibility() that the BP calls on a real press ALSO fans out to servers /
// lightRoots / blackout-doors / wall-cords + gamemode.setPower -- all of which are synced by
// their OWN coop channels (ApplianceState serverBox, LightState lightRoots, DoorState doors).
// Re-running that fan-out on a remote peer would double-drive + fight those channels, so we do
// NOT call buttonsVisibility()/powerChanged()/sendPower()/setPowered(); instead we call the
// visual-only moveLevers() (levers) and drive the eff_*_on/off particle visibility directly
// (the LED half of buttonsVisibility @116-1346), achieving the panel mirror with ZERO fan-out.

#include "ue_wrap/devices/power_control.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"        // SetSceneComponentVisibility (the LED particles)
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::power_control {
namespace {

namespace R = reflection;

// One descriptor per breaker subsystem. `bit` is the PowerPanelPayload mask bit (FIELD/offset
// order, 0x0380..0x0384) -- NOT the powerChanged() arg order (we never call powerChanged). The
// `eff_*_on/off` are the LED particle components (visibility = the lit indicator); note the BP
// typo "eef_calc_*" (not "eff") for the calc subsystem.
struct Sys {
    int            bit;
    const wchar_t* pressName;  int32_t pressOffFallback;
    const wchar_t* effOnName;  int32_t effOnOffFallback;
    const wchar_t* effOffName; int32_t effOffOffFallback;
    // resolved lazily (game-thread serial):
    int32_t pressOff;
    int32_t effOnOff;
    int32_t effOffOff;
};

Sys g_sys[] = {
    { 0, L"press_coord", 0x0380, L"eff_coords_on", 0x02E8, L"eff_coords_off", 0x0310, -1, -1, -1 },
    { 1, L"press_downl", 0x0381, L"eff_downl_on",  0x02E0, L"eff_downl_off",  0x0308, -1, -1, -1 },
    { 2, L"press_play",  0x0382, L"eff_play_on",   0x02F0, L"eff_play_off",   0x0318, -1, -1, -1 },
    { 3, L"press_calc",  0x0383, L"eef_calc_on",   0x02D8, L"eef_calc_off",   0x0300, -1, -1, -1 },
    { 4, L"press_light", 0x0384, L"eff_light_on",  0x02D0, L"eff_light_off",  0x02F8, -1, -1, -1 },
};

std::atomic<bool> g_resolved{false};
void*   g_cls = nullptr;          // powerControl_C UClass
int32_t g_keyOff = -1;            // AtriggerBase_C::Key (Alpha 0.9.0-n: 0x0260)
void*   g_moveLeversFn = nullptr; // moveLevers() -- visual-only lever animation

constexpr int32_t kKeyOffFallback = 0x0260;

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* cls = R::FindClass(L"powerControl_C");
    if (!cls) return false;

    // Key lives on the AtriggerBase_C base; FindPropertyOffset does NOT climb to a super, so
    // resolve it against triggerBase_C directly (same gotcha garage/appliance handle).
    int32_t keyOff = -1;
    if (void* trig = R::FindClass(L"triggerBase_C")) keyOff = R::FindPropertyOffset(trig, L"Key");
    if (keyOff < 0) {
        UE_LOGW("power: reflected Key offset not found -- using fallback 0x%04X", kKeyOffFallback);
        keyOff = kKeyOffFallback;
    }

    for (auto& s : g_sys) {
        s.pressOff = R::FindPropertyOffset(cls, s.pressName);
        if (s.pressOff < 0) {
            UE_LOGW("power: %ls offset not found -- fallback 0x%04X", s.pressName, s.pressOffFallback);
            s.pressOff = s.pressOffFallback;
        }
        s.effOnOff = R::FindPropertyOffset(cls, s.effOnName);
        if (s.effOnOff < 0) {
            UE_LOGW("power: %ls offset not found -- fallback 0x%04X", s.effOnName, s.effOnOffFallback);
            s.effOnOff = s.effOnOffFallback;
        }
        s.effOffOff = R::FindPropertyOffset(cls, s.effOffName);
        if (s.effOffOff < 0) {
            UE_LOGW("power: %ls offset not found -- fallback 0x%04X", s.effOffName, s.effOffOffFallback);
            s.effOffOff = s.effOffOffFallback;
        }
    }

    void* moveLevers = R::FindFunction(cls, L"moveLevers");
    if (!moveLevers)
        UE_LOGW("power: moveLevers UFunction not found -- mirror levers won't animate (LEDs still mirror)");

    g_cls = cls;
    g_keyOff = keyOff;
    g_moveLeversFn = moveLevers;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("power: resolved powerControl_C=%p Key@0x%04X moveLevers=%p", cls, keyOff, moveLevers);
    return true;
}

bool IsPowerControl(void* obj) {
    if (!obj || !g_cls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_cls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

std::wstring GetKeyString(void* p) {
    if (!p || g_keyOff < 0) return std::wstring();
    const R::FName& key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const char*>(p) + g_keyOff);
    return R::ToString(key);
}

bool ReadPress(void* p, uint8_t& mask) {
    if (!p || !g_resolved.load(std::memory_order_acquire)) return false;
    uint8_t m = 0;
    for (auto& s : g_sys) {
        if (s.pressOff < 0) continue;
        if (*reinterpret_cast<const bool*>(reinterpret_cast<const char*>(p) + s.pressOff))
            m |= static_cast<uint8_t>(1u << s.bit);
    }
    mask = m;
    return true;
}

bool ApplyPress(void* p, uint8_t mask) {
    if (!p || !g_resolved.load(std::memory_order_acquire)) return false;

    // 1. Write the 5 latched press_ bools (the authoritative state we poll + mirror).
    for (auto& s : g_sys) {
        if (s.pressOff < 0) continue;
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(p) + s.pressOff) =
            (mask & (1u << s.bit)) != 0;
    }

    // 2. moveLevers() -- animate lever_0..4 from press_ (visual-only; no field writes, no
    //    fan-out -- RE @4074/@1201). Safe to call on the mirror.
    if (g_moveLeversFn) {
        ParamFrame f(g_moveLeversFn);
        if (f.valid()) Call(p, f);
    }

    // 3. LEDs: set the eff_<sys>_on/off particle visibility DIRECTLY (replicating the visual half
    //    of buttonsVisibility @116-1346) -- NOT buttonsVisibility() itself, which would fan out
    //    to servers/lightRoots/blackout-doors/cords + gamemode.setPower (all synced by their own
    //    channels). This drives ONLY the panel's own LED indicators. (The powerblock face-material
    //    bulbs + isOn scalar are a documented visual-polish follow-up; the particles + levers are
    //    the primary indicator.)
    for (auto& s : g_sys) {
        const bool on = (mask & (1u << s.bit)) != 0;
        if (s.effOnOff >= 0)
            if (void* c = *reinterpret_cast<void**>(reinterpret_cast<char*>(p) + s.effOnOff))
                engine::SetSceneComponentVisibility(c, on, false);
        if (s.effOffOff >= 0)
            if (void* c = *reinterpret_cast<void**>(reinterpret_cast<char*>(p) + s.effOffOff))
                engine::SetSceneComponentVisibility(c, !on, false);
    }
    return true;
}

}  // namespace ue_wrap::power_control

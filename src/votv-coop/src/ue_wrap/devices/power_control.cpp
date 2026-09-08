// ue_wrap/power_control.cpp -- see ue_wrap/power_control.h. Engine access for the base POWER
// PANEL (ApowerControl_C). Offsets and verbs are resolved from the live class by reflection
// (version-portable); the Alpha 0.9.0-n values are logged fallbacks.
//
// The apply mirrors the panel's OWN visual -- the lever positions and the LED particles -- and
// nothing downstream. The native buttonsVisibility() the blueprint calls on a real press also
// fans out to servers, lightRoots, blackout doors and wall cords plus gamemode.setPower, all of
// which ride their own coop channels (ApplianceState for the serverBox, LightState for the
// lightRoots, DoorState for the doors). Re-running that fan-out on a remote peer would
// double-drive and fight those channels, so we never call buttonsVisibility(), powerChanged(),
// sendPower() or setPowered(): we call the visual-only moveLevers() and drive the eff_*_on/off
// particle visibility directly, which is the LED half of buttonsVisibility, for a panel mirror
// with zero fan-out.

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

// One descriptor per breaker subsystem. `bit` is the PowerPanelPayload mask bit, in field order --
// NOT the powerChanged() arg order, since we never call powerChanged. The `eff_*_on/off` are the
// LED particle components, whose visibility is the lit indicator; note the blueprint's typo
// "eef_calc_*" for the calc subsystem.
struct Sys {
    int            bit;
    const wchar_t* pressName;
    const wchar_t* effOnName;
    const wchar_t* effOffName;
    // resolved lazily (game-thread serial):
    int32_t pressOff;
    int32_t effOnOff;
    int32_t effOffOff;
};

Sys g_sys[] = {
    { 0, L"press_coord", L"eff_coords_on", L"eff_coords_off", -1, -1, -1 },
    { 1, L"press_downl", L"eff_downl_on",  L"eff_downl_off",  -1, -1, -1 },
    { 2, L"press_play",  L"eff_play_on",   L"eff_play_off",   -1, -1, -1 },
    { 3, L"press_calc",  L"eef_calc_on",   L"eef_calc_off",   -1, -1, -1 },
    { 4, L"press_light", L"eff_light_on",  L"eff_light_off",  -1, -1, -1 },
};

std::atomic<bool> g_resolved{false};
bool    g_layoutRefused = false;  // the class loaded but a field did not: permanent for this build
void*   g_cls = nullptr;          // powerControl_C UClass
int32_t g_keyOff = -1;            // AtriggerBase_C::Key
void*   g_moveLeversFn = nullptr; // moveLevers() -- visual-only lever animation

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;
    if (g_layoutRefused) return false;

    void* cls = R::FindClass(L"powerControl_C");
    if (!cls) return false;  // the blueprint has not streamed in yet; retried next tick

    // Every offset is resolved BY NAME, and a miss refuses the lane rather than falling back on
    // the address this build happens to use. The apply writes a bool and dereferences a component
    // pointer at these offsets, so a recook that moves a field would turn a fallback into a blind
    // write into whatever now lives there. The class IS loaded by this point, so a miss is a fact
    // about this game build and not a timing race: it is latched, and reported once.
    auto refuse = [&](const wchar_t* what) {
        g_layoutRefused = true;
        UE_LOGE("power: %ls did not resolve on a loaded powerControl_C -- the base power-panel lane "
                "is OFF for this game build (no blind writes at a stale offset)", what);
        return false;
    };

    // Key lives on the AtriggerBase_C base; FindPropertyOffset does NOT climb to a super, so
    // resolve it against triggerBase_C directly (same gotcha garage/appliance handle).
    int32_t keyOff = -1;
    if (void* trig = R::FindClass(L"triggerBase_C")) keyOff = R::FindPropertyOffset(trig, L"Key");
    if (keyOff < 0) return refuse(L"triggerBase_C::Key");

    for (auto& s : g_sys) {
        s.pressOff = R::FindPropertyOffset(cls, s.pressName);
        if (s.pressOff < 0) return refuse(s.pressName);
        s.effOnOff = R::FindPropertyOffset(cls, s.effOnName);
        if (s.effOnOff < 0) return refuse(s.effOnName);
        s.effOffOff = R::FindPropertyOffset(cls, s.effOffName);
        if (s.effOffOff < 0) return refuse(s.effOffName);
    }

    // The one best-effort resolve: moveLevers is a VERB, not an address. Without it the mirror
    // still drives the LEDs, so its absence degrades the visual instead of corrupting the object.
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

    // 3. LEDs: set the eff_<sys>_on/off particle visibility DIRECTLY -- the visual half of
    // buttonsVisibility -- and never buttonsVisibility() itself, which would fan out to servers,
    // lightRoots, blackout doors and cords plus gamemode.setPower, all of them synced by their own
    // channels. This drives ONLY the panel's own LED indicators. The powerblock face-material bulbs
    // and the isOn scalar are still a visual-polish follow-up; the particles and levers are the
    // primary indicator.
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

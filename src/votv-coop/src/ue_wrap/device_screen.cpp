// ue_wrap/device_screen.cpp -- see ue_wrap/device_screen.h.

#include "ue_wrap/device_screen.h"

#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflected_offset.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"
#include "ue_wrap/types.h"

#include <chrono>
#include <cmath>
#include <cstdint>

namespace ue_wrap::device_screen {
namespace {

namespace R  = ue_wrap::reflection;
namespace RO = ue_wrap::reflected_offset;
namespace P  = ue_wrap::profile;

// FHitResult sub-offsets (ENGINE struct, 4.27-stable -- not BP-recook-volatile,
// so a local constant per the reflected_offset.h doctrine; single consumer).
// FHitResult is 0x88; `TWeakObjectPtr<AActor> Actor` sits at +0x68 as
// {int32 ObjectIndex, int32 ObjectSerialNumber}. The canonical NULL weakptr is
// ObjectIndex=-1 / Serial=0 -> 0x00000000FFFFFFFF little-endian. (RE doc U4:
// layout consistent with the dumped FHitResult size 0x88.)
constexpr int32_t kFHitResult_Actor = 0x68;
constexpr uint64_t kNullWeakPtr = 0x00000000FFFFFFFFull;

// Cached UClass pointers. BP classes load with their content (several only
// when their sublevel/asset streams in), so each slot lazily fills on the
// throttled resolve pass below and stays null until then. A null slot simply
// never matches -- correct: a device whose class hasn't loaded locally cannot
// be aimed at locally either. Class identity is an EXACT ClassOf match (none
// of the 8 device BPs nor the 7 widgets are subclassed -- RE census).
struct ClassSlot { const wchar_t* name; void* cls; };

// Widget classes (the activeInterface side). Fixed singleton keys except the
// last two (per-instance widgets -> owner-resolved position keys).
ClassSlot g_widgets[] = {
    { L"ui_consolesAtlas_C",            nullptr },  // -> "desk"
    { L"ui_console_C",                  nullptr },  // -> "sat"
    { L"ui_radar_C",                    nullptr },  // -> "radar"
    { L"ui_reactor_C",                  nullptr },  // -> "reactor"
    { L"ui_laptop_C",                   nullptr },  // -> "laptop"
    { L"uiwindow_transformerScreens_C", nullptr },  // -> "tfmr:<posKey>"
    { L"ui_arcade_invaders_C",          nullptr },  // -> "arcade:<posKey>"
};
const wchar_t* const g_widgetKeys[] = {
    L"desk", L"sat", L"radar", L"reactor", L"laptop", nullptr, nullptr };

// Device actor classes (the aim/deny side).
ClassSlot g_devices[] = {
    { L"analogDScreenTest_C",  nullptr },  // -> "desk"
    { L"panel_SATconsole_C",   nullptr },  // -> "sat"
    { L"panel_radar_C",        nullptr },  // -> "radar"
    { L"panel_reactor_C",      nullptr },  // -> "reactor"
    { L"laptop_C",             nullptr },  // -> "laptop"
    { L"prop_portablePc_C",    nullptr },  // -> "laptop" (same shared widget)
    { L"transformerMGPanel_C", nullptr },  // -> "tfmr:<posKey>"
    { L"prop_arcade_C",        nullptr },  // -> "arcade:<posKey>"
};
const wchar_t* const g_deviceKeys[] = {
    L"desk", L"sat", L"radar", L"reactor", L"laptop", L"laptop", nullptr, nullptr };

void* g_playerCls = nullptr;
void* g_setActiveInterfaceFn = nullptr;
// Per-instance widget backref fields (FindPropertyOffset, recook-robust).
int32_t g_offTfmrWidgetInst = -1;   // transformerMGPanel_C::widgetInst @0x02A8
int32_t g_offArcadeScrWidge = -1;   // prop_arcade_C::scrWidge

std::chrono::steady_clock::time_point g_nextResolve{};
bool g_coreResolved = false;

// CORE resolve only (player class + setActiveInterface fn + the activeInterface/lookAt/HitResult
// offsets). Throttled to 2 s, and STOPS the moment the core latches (g_coreResolved). The device +
// widget CLASS pointers are NOT resolved here any more -- they resolve lazily at the interaction edge
// (FillDeviceSlotFromClass / FillWidgetSlotFromClass below).
//
// WHY (L5 device_occupancy hitch root, 2026-06-24): the old per-2 s
// `for (d : g_devices) if (!d.cls) d.cls = FindClass(d.name)` loop name-walked all ~237k UObjects EVERY
// 2 s for any class that had not loaded -- and the BUYABLE props (prop_arcade_C / prop_portablePc_C) only
// load when PURCHASED, so an un-bought one walked FOREVER (~30 ms/2 s, the measured device_occupancy half
// of the steady hitch). A settle-bound poll-stop would strand a genuinely-late buy; resolving from a live
// instance's ClassOf at the edge is walk-free AND late-buy-safe by construction (you cannot aim at / enter
// a device that does not exist -- the instant it does, ClassOf resolves it inline, before the claim check
// in the SAME dispatch). [[lesson-periodic-hitch-not-the-walk-by-period-coincidence]] cousin: it WAS a
// real walk here, just a class-resolution walk, not an instance-index walk -> the fix is edge-resolve, not
// IncrementalObjectScan.
void ResolvePass() {
    if (g_coreResolved) return;  // core latched -- nothing left to poll (classes are edge-lazy now)
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextResolve) return;
    g_nextResolve = now + std::chrono::seconds(2);

    if (!g_playerCls) g_playerCls = R::FindClass(P::name::MainPlayerClass);
    if (g_playerCls && !g_setActiveInterfaceFn) {
        g_setActiveInterfaceFn = R::FindFunction(g_playerCls, L"setActiveInterface");
        if (g_setActiveInterfaceFn)
            UE_LOGI("device_screen: setActiveInterface resolved (force-exit ready)");
    }
    const bool core = g_playerCls && g_setActiveInterfaceFn &&
                      RO::MainPlayer_activeInterface() >= 0 &&
                      RO::MainPlayer_lookAtActor() >= 0 &&
                      RO::MainPlayer_HitResult() >= 0;
    if (core) {
        g_coreResolved = true;
        UE_LOGI("device_screen: core resolved (activeInterface=0x%X lookAt=0x%X hit=0x%X); device/widget "
                "classes now resolve lazily at the interaction edge (per-2s FindClass poll removed)",
                RO::MainPlayer_activeInterface(), RO::MainPlayer_lookAtActor(),
                RO::MainPlayer_HitResult());
    }
}

// ---- Edge-lazy class resolution (beta, 2026-06-24) ------------------------
// Fill a device/widget cache slot DIRECTLY from a live instance's ClassOf -- the aimed/entered object IS
// an instance of its class, so ClassOf(obj) is the exact UClass with NO GUObjectArray walk. Exact-name
// match into the fixed slot list; idempotent (early-out if already cached). Exact match is correct: none
// of the 8 device / 7 widget BPs is subclassed (RE census + reflection-dump subclass scan 2026-06-24),
// and this is behaviour-IDENTICAL to the prior exact-ClassOf comparison the deny gate already relied on.
// Called only at the interaction EDGE (E-press deny / activeInterface rising edge), never per-tick.
void FillDeviceSlotFromClass(void* cls) {
    if (!cls) return;
    for (auto& d : g_devices) if (d.cls == cls) return;  // already cached
    const std::wstring nm = R::ToString(R::NameOf(cls));
    for (size_t i = 0; i < 8; ++i) {
        if (g_devices[i].cls || nm != g_devices[i].name) continue;
        g_devices[i].cls = cls;
        if (i == 6) g_offTfmrWidgetInst = R::FindPropertyOffset(cls, L"widgetInst");
        if (i == 7) g_offArcadeScrWidge = R::FindPropertyOffset(cls, L"scrWidge");
        UE_LOGI("device_screen: device class '%ls' resolved at edge (lazy, no walk)", nm.c_str());
        return;
    }
}
void FillWidgetSlotFromClass(void* cls) {
    if (!cls) return;
    for (auto& w : g_widgets) if (w.cls == cls) return;  // already cached
    const std::wstring nm = R::ToString(R::NameOf(cls));
    for (auto& w : g_widgets) {
        if (w.cls || nm != w.name) continue;
        w.cls = cls;
        UE_LOGI("device_screen: widget class '%ls' resolved at edge (lazy, no walk)", nm.c_str());
        return;
    }
}
// The per-instance device classes (transformer [6] / arcade [7]) map a per-instance widget back to its
// owning actor via FindOwnerByWidgetField, which needs the backref OFFSET (-> needs the device class). The
// E-press deny path normally resolves this first (OnUseInputPre -> ClassifyDeviceActorClaimKey), but to
// guarantee NO bypass (Check 2) -- any enter that skips the deny path -- resolve it here too: ONE edge-rate
// FindClass on actual enter (the device provably exists, so it resolves in a single walk + caches). This is
// NOT the removed per-2s poll; it is a once-ever resolve on real interaction.
void EnsurePerInstanceDevice(size_t i) {
    if (g_devices[i].cls) return;
    g_devices[i].cls = R::FindClass(g_devices[i].name);
    if (!g_devices[i].cls) return;
    if (i == 6) g_offTfmrWidgetInst = R::FindPropertyOffset(g_devices[i].cls, L"widgetInst");
    if (i == 7) g_offArcadeScrWidge = R::FindPropertyOffset(g_devices[i].cls, L"scrWidge");
    UE_LOGI("device_screen: per-instance device '%ls' resolved at enter edge", g_devices[i].name);
}

// Quantized-position identity for the per-instance devices (the turbine
// PosKey shape; both transformer panels and arcade props sit still in play --
// the arcade is a physics prop, but its key need only be stable while someone
// is INSIDE it, which freezes the player and leaves the cabinet parked).
// FORMAT BUDGET (audit CRIT-2): the key must fit WireKey.data (31 chars) BY
// CONSTRUCTION -- WireKeyFromString truncates silently, and a truncated wire
// key would mismatch the receiver's locally-computed full key, silently
// disabling the deny gate for that device. Prefixes are 4 chars ("arc_" /
// "tfm_", no "t_" infix): worst case 4 + 3x(7-digit signed coord) + 2
// separators = 27 chars at +/-100 km world coords -- beyond any UE4 float
// world. The defensive WARN below catches the impossible anyway.
constexpr double kPosGrid = 10.0;  // cm
std::wstring PosKey(const wchar_t* prefix, void* actor) {
    const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(actor);
    auto q = [](float v) -> long { return std::lround(static_cast<double>(v) / kPosGrid); };
    std::wstring k = prefix;
    k += std::to_wstring(q(loc.X)); k += L'_';
    k += std::to_wstring(q(loc.Y)); k += L'_';
    k += std::to_wstring(q(loc.Z));
    if (k.size() > 31) {
        static bool sWarned = false;
        if (!sWarned) {
            sWarned = true;
            UE_LOGW("device_screen: PosKey '%ls' exceeds the 31-char WireKey budget -- "
                    "claims for this device will not match across peers", k.c_str());
        }
    }
    return k;
}

// Resolve the owning actor of a per-instance widget by matching the device's
// widget backref field over the (few) live instances. Edge-rate only (claim
// classification), never per-tick.
void* FindOwnerByWidgetField(const wchar_t* className, int32_t fieldOff, void* widget) {
    if (fieldOff < 0) return nullptr;
    for (void* obj : R::FindObjectsByClass(className)) {
        if (!obj || !R::IsLive(obj)) continue;
        void* w = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + fieldOff);
        if (w == widget) return obj;
    }
    return nullptr;
}

// The one-slot PRE/POST aim save (door Active-gate shape; GT-only by
// dispatch construction).
struct SavedAim {
    void*    player = nullptr;
    void*    lookAtActor = nullptr;
    uint64_t hitActorWeak = 0;
};
SavedAim g_saved;

}  // namespace

bool EnsureResolved() {
    ResolvePass();
    return g_coreResolved;
}

void* ReadActiveInterface(void* player) {
    if (!player) return nullptr;
    const int32_t off = RO::MainPlayer_activeInterface();
    if (off < 0) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(player) + off);
}

std::wstring ClassifyWidgetClaimKey(void* widget) {
    if (!widget || !R::IsLive(widget)) return {};
    void* cls = R::ClassOf(widget);
    if (!cls) return {};
    FillWidgetSlotFromClass(cls);  // beta: resolve this widget's class from its own ClassOf (late-buy safe), inline before the match
    for (size_t i = 0; i < 5; ++i)
        if (g_widgets[i].cls && cls == g_widgets[i].cls) return g_widgetKeys[i];
    if (g_widgets[5].cls && cls == g_widgets[5].cls) {
        EnsurePerInstanceDevice(6);  // transformer device class + widgetInst offset (no-bypass guarantee)
        if (void* owner = FindOwnerByWidgetField(L"transformerMGPanel_C",
                                                 g_offTfmrWidgetInst, widget))
            return PosKey(L"tfm_", owner);
        UE_LOGW("device_screen: transformer widget %p has no owning panel (widgetInst miss)", widget);
        return {};
    }
    if (g_widgets[6].cls && cls == g_widgets[6].cls) {
        EnsurePerInstanceDevice(7);  // arcade device class + scrWidge offset (no-bypass guarantee)
        if (void* owner = FindOwnerByWidgetField(L"prop_arcade_C",
                                                 g_offArcadeScrWidge, widget))
            return PosKey(L"arc_", owner);
        UE_LOGW("device_screen: arcade widget %p has no owning cabinet (scrWidge miss)", widget);
        return {};
    }
    return {};  // clipboard / prop inventory / paper / picker -- not devices
}

std::wstring ClassifyDeviceActorClaimKey(void* actor) {
    if (!actor) return {};
    void* cls = R::ClassOf(actor);
    if (!cls) return {};
    FillDeviceSlotFromClass(cls);  // beta: resolve this device's class from its own ClassOf (late-buy safe), inline before the match
    for (size_t i = 0; i < 6; ++i)
        if (g_devices[i].cls && cls == g_devices[i].cls) return g_deviceKeys[i];
    if (g_devices[6].cls && cls == g_devices[6].cls) return PosKey(L"tfm_", actor);
    if (g_devices[7].cls && cls == g_devices[7].cls) return PosKey(L"arc_", actor);
    return {};
}

bool ClearAimForDispatch(void* player) {
    if (!player) return false;
    const int32_t offLook = RO::MainPlayer_lookAtActor();
    const int32_t offHit  = RO::MainPlayer_HitResult();
    if (offLook < 0 || offHit < 0) return false;
    auto* pLook = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(player) + offLook);
    auto* pWeak = reinterpret_cast<uint64_t*>(
        reinterpret_cast<uint8_t*>(player) + offHit + kFHitResult_Actor);
    g_saved.player = player;
    g_saved.lookAtActor = *pLook;
    g_saved.hitActorWeak = *pWeak;
    *pLook = nullptr;
    *pWeak = kNullWeakPtr;  // canonical null: every icast in the enter chain fails
    return true;
}

void RestoreAim() {
    if (!g_saved.player) return;
    const int32_t offLook = RO::MainPlayer_lookAtActor();
    const int32_t offHit  = RO::MainPlayer_HitResult();
    if (offLook >= 0 && offHit >= 0 && R::IsLive(g_saved.player)) {
        *reinterpret_cast<void**>(
            reinterpret_cast<uint8_t*>(g_saved.player) + offLook) = g_saved.lookAtActor;
        *reinterpret_cast<uint64_t*>(
            reinterpret_cast<uint8_t*>(g_saved.player) + offHit + kFHitResult_Actor) =
            g_saved.hitActorWeak;
    }
    g_saved = {};
}

bool HasClearedAim() { return g_saved.player != nullptr; }

bool ForceExitInterface(void* player) {
    if (!player || !R::IsLive(player) || !g_setActiveInterfaceFn) return false;
    // setActiveInterface(activeInterface=null, inString="", Zoom=false,
    // sentBy=null, ignoreZoom=false, isActiveINterface3D=false,
    // showInterface=false, &return). The null-widget branch (@1405-1531) is
    // the game's own forced-exit path (ragdollMode rides it): GameOnly input,
    // cursor off, default FOV, movement restored, exitInterface BROADCAST.
    // An all-zero frame IS that call -- the bool params only matter on the
    // non-null enter branch.
    ue_wrap::ParamFrame f(g_setActiveInterfaceFn);
    if (!f.valid()) return false;
    return ue_wrap::Call(player, f);
}

}  // namespace ue_wrap::device_screen

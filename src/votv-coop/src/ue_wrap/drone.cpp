// ue_wrap/drone.cpp -- see ue_wrap/drone.h. Engine access for the delivery drone (Adrone_C).
// Offsets resolved from the live class via reflection (version-portable); the Alpha 0.9.0-n value
// is a logged fallback. Transform reads/writes go through ue_wrap::engine at the actor level (the
// drone moves the actor via its BP ReceiveTick, no physics body). Find() mirrors the skysphere/
// daynightcycle cached-singleton + throttled-scan shape.

#include "ue_wrap/drone.h"

#include "ue_wrap/call.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/fname_utils.h"
#include "ue_wrap/log.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace ue_wrap::drone {
namespace {

namespace R = reflection;
namespace E = engine;

std::atomic<bool> g_resolved{false};
void*   g_cls       = nullptr;  // drone_C UClass
int32_t g_activeOff = -1;       // Adrone_C::Active (Alpha 0.9.0-n: 0x0370, bool)

constexpr int32_t kActiveOffFallback = 0x0370;

void*   g_cache    = nullptr;  // cached singleton drone actor (GT-only)
int32_t g_cacheIdx = -1;       // its GUObjectArray slot -- IsLiveByIndex is safe vs a freed pointer
                               // (the project rule; IsLive(ptr) derefs the maybe-freed object itself)

// ---- FX mirroring (Phase 2) -- resolved lazily (separate from the pose path) ----
bool    g_fxResolved   = false;
int32_t g_canTakeOffOff = -1;  // Adrone_C::canTakeOff @0x0500 (arrival edge)
int32_t g_dustOff       = -1;  // Adrone_C::eff_droneDust @0x0278 (UParticleSystemComponent*)
int32_t g_audioAlarmOff = -1;  // Adrone_C::audio_alarm  @0x0230 (UAudioComponent*)
void*   g_setActiveFn   = nullptr;  // UActorComponent::SetActive(bool bNewActive, bool bReset)
void*   g_activateFn    = nullptr;  // UActorComponent::Activate(bool bReset)
void*   g_isActiveFn    = nullptr;  // UActorComponent::IsActive() -> bool
int32_t g_lightAlarmOff = -1;       // Adrone_C::light_alarm @0x0240 (UPointLightComponent*) -- arrival signal light
void*   g_setFloatParamFn = nullptr;// UParticleSystemComponent::SetFloatParameter(FName, float) -- dust intensity
void*   g_setVisFn        = nullptr;// USceneComponent::SetVisibility(bool, bool) -- the signal light
R::FName g_dustFName{0, 0};         // the 'dust' particle intensity param name (lazy; needs Kismet up)
constexpr int32_t kCanTakeOffFallback = 0x0500;
constexpr int32_t kDustFallback       = 0x0278;
constexpr int32_t kAudioAlarmFallback = 0x0230;
constexpr int32_t kLightAlarmFallback = 0x0240;
constexpr float   kDustIntensity      = 1.0f;  // full intensity on the mirror (the host fades by ground
                                               // distance; the mirror lacks the raycast -> fixed -- MVP)
// Interaction-gate fields (RE 2026-06-09): the client mirror must carry these so a PARKED drone is
// interactable (the suppressed tick never sets them). canTakeOff@0x0500 IS the gate (true=parked);
// hasSack@0x0501 gates the action options (open-inv / drop-sack); container@0x04F8 is the inventory
// actor openPropInv opens (a keyed Aprop_C the prop pipeline already mirrors).
int32_t g_hasSackOff   = -1;   // Adrone_C::hasSack   @0x0501
int32_t g_containerOff = -1;   // Adrone_C::container @0x04F8 (Aprop_inventoryContainer_drone_C*)
constexpr int32_t kHasSackFallback   = 0x0501;
constexpr int32_t kContainerFallback = 0x04F8;

bool EnsureFxResolved() {
    if (g_fxResolved) return true;
    if (!EnsureResolved() || !g_cls) return false;
    g_canTakeOffOff = R::FindPropertyOffset(g_cls, L"canTakeOff");
    if (g_canTakeOffOff < 0) g_canTakeOffOff = kCanTakeOffFallback;
    g_dustOff = R::FindPropertyOffset(g_cls, L"eff_droneDust");
    if (g_dustOff < 0) g_dustOff = kDustFallback;
    g_audioAlarmOff = R::FindPropertyOffset(g_cls, L"audio_alarm");
    if (g_audioAlarmOff < 0) g_audioAlarmOff = kAudioAlarmFallback;
    g_lightAlarmOff = R::FindPropertyOffset(g_cls, L"light_alarm");
    if (g_lightAlarmOff < 0) g_lightAlarmOff = kLightAlarmFallback;
    g_hasSackOff = R::FindPropertyOffset(g_cls, L"hasSack");
    if (g_hasSackOff < 0) g_hasSackOff = kHasSackFallback;
    g_containerOff = R::FindPropertyOffset(g_cls, L"container");
    if (g_containerOff < 0) g_containerOff = kContainerFallback;
    // SetActive/Activate/IsActive are declared on UActorComponent (the components' base); resolve
    // them there and dispatch on the concrete component instance (ProcessEvent resolves the impl).
    if (void* acCls = R::FindClass(L"ActorComponent")) {
        g_setActiveFn = R::FindFunction(acCls, L"SetActive");
        g_activateFn  = R::FindFunction(acCls, L"Activate");
        g_isActiveFn  = R::FindFunction(acCls, L"IsActive");
    }
    if (!g_setActiveFn || !g_activateFn || !g_isActiveFn) {
        UE_LOGW("drone: FX component UFunctions incomplete (SetActive=%p Activate=%p IsActive=%p) -- "
                "FX mirror disabled", g_setActiveFn, g_activateFn, g_isActiveFn);
        return false;
    }
    // Best-effort FX-polish UFunctions (dust intensity + the signal light) -- not required for the
    // core mirror, so they don't gate g_fxResolved; logged if absent.
    // SetFloatParameter is declared on UFXSystemComponent (the PARENT of UParticleSystemComponent),
    // NOT on UParticleSystemComponent itself -- and FindFunction does NOT climb the super chain, so
    // it MUST be resolved on the owning class (the same gotcha SetActive->ActorComponent handles
    // above + engine_widget's SetVisibility->Widget). Resolving on ParticleSystemComponent returns
    // null -> the 'dust' param is never set -> the emitter stays at rate 0 -> NO dust (the bug).
    if (void* fxCls = R::FindClass(L"FXSystemComponent"))
        g_setFloatParamFn = R::FindFunction(fxCls, L"SetFloatParameter");
    if (void* scCls = R::FindClass(profile::name::SceneComponentClass))
        g_setVisFn = R::FindFunction(scCls, profile::name::SetVisibilityFn);
    if (!g_setFloatParamFn || !g_setVisFn)
        UE_LOGW("drone: FX polish UFunctions partial (SetFloatParameter=%p SetVisibility=%p) -- "
                "dust intensity / signal light may be limited", g_setFloatParamFn, g_setVisFn);
    g_fxResolved = true;
    UE_LOGI("drone: FX resolved canTakeOff@0x%04X eff_droneDust@0x%04X audio_alarm@0x%04X "
            "light_alarm@0x%04X hasSack@0x%04X container@0x%04X",
            g_canTakeOffOff, g_dustOff, g_audioAlarmOff, g_lightAlarmOff, g_hasSackOff, g_containerOff);
    return true;
}

void* ReadComp(void* drone, int32_t off) {
    if (!drone || off < 0) return nullptr;
    void* c = *reinterpret_cast<void**>(reinterpret_cast<char*>(drone) + off);
    return (c && R::IsLive(c)) ? c : nullptr;
}

bool ComponentIsActive(void* comp) {
    if (!comp || !g_isActiveFn) return false;
    ParamFrame f(g_isActiveFn);
    if (!f.valid() || !Call(comp, f)) return false;
    return f.Get<bool>(L"ReturnValue");
}

void SetComponentActive(void* comp, bool on) {
    if (!comp || !g_setActiveFn) return;
    ParamFrame f(g_setActiveFn);
    if (!f.valid()) return;
    f.Set<bool>(L"bNewActive", on);
    f.Set<bool>(L"bReset", false);
    Call(comp, f);
}

void ActivateComponent(void* comp) {
    if (!comp || !g_activateFn) return;
    ParamFrame f(g_activateFn);
    if (!f.valid()) return;
    f.Set<bool>(L"bReset", true);  // restart the cue from the top each arrival
    Call(comp, f);
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;
    void* cls = R::FindClass(L"drone_C");
    if (!cls) return false;  // not loaded yet -- caller retries

    int32_t activeOff = R::FindPropertyOffset(cls, L"Active");
    if (activeOff < 0) {
        UE_LOGW("drone: reflected Active offset not found -- using fallback 0x%04X", kActiveOffFallback);
        activeOff = kActiveOffFallback;
    }

    g_cls = cls;
    g_activeOff = activeOff;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("drone: resolved drone_C=%p Active@0x%04X", cls, activeOff);
    return true;
}

void* Find() {
    if (g_cache && R::IsLiveByIndex(g_cache, g_cacheIdx)) return g_cache;  // steady-state: index check
    if (!EnsureResolved()) return nullptr;
    // The delivery drone is a singleton; once found it stays live. THROTTLE the scan to once/sec
    // so a transient miss can never become a per-frame FindObjectByClass walk (the standing ban).
    // Game-thread-only -> the static is unguarded.
    static std::chrono::steady_clock::time_point s_lastScan{};
    const auto now = std::chrono::steady_clock::now();
    if (g_cache == nullptr || now - s_lastScan >= std::chrono::seconds(1)) {
        s_lastScan = now;
        g_cache = R::FindObjectByClass(L"drone_C");
        g_cacheIdx = g_cache ? R::InternalIndexOf(g_cache) : -1;
    }
    return (g_cache && R::IsLiveByIndex(g_cache, g_cacheIdx)) ? g_cache : nullptr;
}

bool IsActive(void* drone) {
    if (!drone || g_activeOff < 0) return false;
    return *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(drone) + g_activeOff);
}

bool GetTransform(void* drone, FVector& loc, FRotator& rot) {
    if (!drone) return false;
    loc = E::GetActorLocation(drone);
    rot = E::GetActorRotation(drone);
    return true;
}

bool DriveMirror(void* drone, const FVector& loc, const FRotator& rot) {
    if (!drone) return false;
    bool ok = E::SetActorLocation(drone, loc);
    ok = E::SetActorRotation(drone, rot) && ok;
    return ok;
}

void SuppressTick(void* drone) {
    if (!drone) return;
    E::SetActorTickEnabled(drone, false);  // stop the flight integrator -- the client drone is a mirror
}

void RestoreTick(void* drone) {
    if (!drone) return;
    E::SetActorTickEnabled(drone, true);
}

uint8_t ReadFxBits(void* drone) {
    if (!drone || !EnsureFxResolved()) return 0;
    uint8_t bits = 0;
    if (void* dust = ReadComp(drone, g_dustOff))
        if (ComponentIsActive(dust)) bits |= kFxDust;
    auto rb = [drone](int32_t off) -> bool {
        return off >= 0 && *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(drone) + off);
    };
    if (rb(g_canTakeOffOff)) bits |= kFxArrived;   // parked/arrived -> the interaction gate
    if (rb(g_hasSackOff))    bits |= kFxHasSack;   // cargo aboard -> the action-option prerequisite
    return bits;
}

void SetDust(void* drone, bool on) {
    if (!drone || !EnsureFxResolved()) return;
    void* dust = ReadComp(drone, g_dustOff);
    if (!dust) return;
    SetComponentActive(dust, on);
    // The emitter's spawn rate is driven by the 'dust' float param (intensity); SetActive(true)
    // alone leaves it at the mirror's default (0 -> emits NOTHING -- the "no dust on peers" bug).
    // Set it so the dust is visible. (Lazy-resolve the FName: it needs Kismet up, which post-boot it is.)
    if (on && g_setFloatParamFn) {
        if (g_dustFName.ComparisonIndex == 0 && g_dustFName.Number == 0)
            g_dustFName = ue_wrap::fname_utils::StringToFName(L"dust");
        if (g_dustFName.ComparisonIndex != 0 || g_dustFName.Number != 0) {
            ParamFrame f(g_setFloatParamFn);
            if (f.valid()) {
                f.SetRaw(L"ParameterName", &g_dustFName, sizeof(g_dustFName));
                f.Set<float>(L"Param", kDustIntensity);
                Call(dust, f);
            }
        }
    }
}

void PlayArrivalCue(void* drone) {
    if (!drone || !EnsureFxResolved()) return;
    // audio_alarm.Activate(true) = the "items ready" ding. Fire-and-forget on the arrival edge.
    if (void* audio = ReadComp(drone, g_audioAlarmOff)) ActivateComponent(audio);
}

void SetSignalLight(void* drone, bool on) {
    if (!drone || !EnsureFxResolved() || !g_setVisFn) return;
    // light_alarm is the visible arrival "signal" (the host blinks it; the mirror shows it steady
    // while the drone is parked-with-cargo, off otherwise -- the visual half of "items ready").
    if (void* light = ReadComp(drone, g_lightAlarmOff)) {
        ParamFrame f(g_setVisFn);
        if (!f.valid()) return;
        f.Set<bool>(L"bNewVisibility", on);
        f.Set<bool>(L"bPropagateToChildren", false);
        Call(light, f);
    }
}

void WriteGateFields(void* drone, bool canTakeOff, bool hasSack) {
    if (!drone || !EnsureFxResolved()) return;
    // The client's suppressed tick never sets these, so a parked mirror drone reads "in motion" +
    // shows no action options. Mirror the host's values so a parked drone is interactable (RE: the
    // actionOptionIndex gate is `IFNOT(canTakeOff) -> "drone is in motion"`; getActionOptions needs
    // hasSack for the open-inv/drop options to appear).
    if (g_canTakeOffOff >= 0)
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(drone) + g_canTakeOffOff) = canTakeOff;
    if (g_hasSackOff >= 0)
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(drone) + g_hasSackOff) = hasSack;
}

void RepointContainer(void* drone) {
    if (!drone || !EnsureFxResolved() || g_containerOff < 0) return;
    // openPropInv(container) reads the mirror drone's OWN container@0x04F8, which the suppressed tick
    // never populates. The container actor itself (Aprop_inventoryContainer_drone_C, keyed
    // 'droneContainer') is already mirrored by the prop pipeline -- find it + point the field at it so
    // the inventory opens. Idempotent: only writes when the field is null + the actor exists.
    void** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(drone) + g_containerOff);
    if (*slot && R::IsLive(*slot)) return;  // already pointed at a live container
    if (void* c = R::FindObjectByClass(L"prop_inventoryContainer_drone_C")) {
        *slot = c;
        UE_LOGI("drone: repointed mirror container @0x%04X -> %p (prop-mirrored 'droneContainer')",
                g_containerOff, c);
    }
}

}  // namespace ue_wrap::drone

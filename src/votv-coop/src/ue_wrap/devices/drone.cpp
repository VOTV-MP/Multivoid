// ue_wrap/devices/drone.cpp -- see ue_wrap/devices/drone.h. Engine access for the delivery drone
// (Adrone_C). Offsets resolved from the live class via reflection; a field that does not resolve leaves
// its feature off, never a guessed offset. The pose and the state bits are read from memory (the root
// component's relative transform, the dust component's bIsActive bit), writes go through
// ue_wrap::engine at the actor level: the drone flies by forces on its root sphere (`coll`, pushed by
// AddForce and AddTorqueInDegrees in its ReceiveTick), which carries the actor. Find() is the world
// singleton's drone_C.

#include "ue_wrap/devices/drone.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/world/world_singleton.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace ue_wrap::drone {
namespace {

namespace R = reflection;
namespace E = engine;

std::atomic<bool> g_resolved{false};
void*   g_cls       = nullptr;  // drone_C UClass
int32_t g_activeOff = -1;       // Adrone_C::Active
int32_t g_leaveAfter5minOff = -1; // Adrone_C::leaveAfter5min


                               // (the project rule; IsLive(ptr) derefs the maybe-freed object itself)

// ---- FX mirroring -- resolved lazily (separate from the pose path) ----
bool    g_fxResolved   = false;
bool    g_fxTried      = false;  // one attempt once the class is loaded: what it misses stays missing
int32_t g_canTakeOffOff = -1;  // Adrone_C::canTakeOff (arrival edge)
int32_t g_dustOff       = -1;  // Adrone_C::eff_droneDust (UParticleSystemComponent*)
int32_t g_audioAlarmOff = -1;  // Adrone_C::audio_alarm (UAudioComponent*)
void*   g_setActiveFn   = nullptr;  // UActorComponent::SetActive(bool bNewActive, bool bReset)
void*   g_activateFn    = nullptr;  // UActorComponent::Activate(bool bReset)
int32_t g_activeByteOff  = -1;      // UActorComponent::bIsActive, a bitfield byte (what IsActive returns)
uint8_t g_activeMask     = 0;
int32_t g_lightAlarmOff = -1;       // Adrone_C::light_alarm (UPointLightComponent*) -- arrival signal light
void*   g_setFloatParamFn = nullptr;// UFXSystemComponent::SetFloatParameter(FName, float) -- dust intensity
void*   g_setVisFn        = nullptr;// USceneComponent::SetVisibility(bool, bool) -- the signal light
void*   g_getCompLocFn    = nullptr;// USceneComponent::K2_GetComponentLocation() -- host dust-anchor read
void*   g_setWorldLocFn   = nullptr;// USceneComponent::K2_SetWorldLocation(...) -- mirror dust-anchor pin
R::FName g_dustFName{0, 0};         // the 'dust' particle intensity param name (lazy; needs Kismet up)
// The blueprint's per-tick intensity formula: 'dust' = 1 - distance(trace start, the hit, or the
// trace end when nothing was hit) / 2000 -- a RateScale multiplier on both spawn modules, so its
// valid range is [0,1].
constexpr float kDustTraceLen = 2000.f;
// Interaction-gate fields: the client mirror must carry these so a PARKED drone is interactable,
// since the suppressed tick never sets them. canTakeOff IS the gate (true means parked); hasSack gates the
// action options (open the cargo, drop the sack) and shows the sack on the body.
int32_t g_hasSackOff   = -1;   // Adrone_C::hasSack
int32_t g_sackOff      = -1;   // Adrone_C::sack (the sack's mesh component on the body)

bool EnsureFxResolved() {
    if (g_fxResolved) return true;
    if (g_fxTried || !EnsureResolved() || !g_cls) return false;
    g_fxTried = true;
    g_canTakeOffOff = R::FindPropertyOffset(g_cls, L"canTakeOff");
    g_dustOff = R::FindPropertyOffset(g_cls, L"eff_droneDust");
    g_audioAlarmOff = R::FindPropertyOffset(g_cls, L"audio_alarm");
    g_lightAlarmOff = R::FindPropertyOffset(g_cls, L"light_alarm");
    g_hasSackOff = R::FindPropertyOffset(g_cls, L"hasSack");
    g_sackOff = R::FindPropertyOffset(g_cls, L"sack");
    // SetActive/Activate and bIsActive are declared on UActorComponent (the components' base); resolve
    // them there and dispatch on the concrete component instance (ProcessEvent resolves the impl).
    if (void* acCls = R::FindClass(L"ActorComponent")) {
        g_setActiveFn = R::FindFunction(acCls, L"SetActive");
        g_activateFn  = R::FindFunction(acCls, L"Activate");
        R::FindBoolProperty(acCls, L"bIsActive", g_activeByteOff, g_activeMask);
    }
    if (!g_setActiveFn || !g_activateFn || g_activeByteOff < 0) {
        UE_LOGW("drone: FX component members incomplete (SetActive=%p Activate=%p bIsActive@%d) -- "
                "FX mirror disabled", g_setActiveFn, g_activateFn, g_activeByteOff);
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
    if (void* scCls = R::FindClass(profile::name::SceneComponentClass)) {
        g_setVisFn      = R::FindFunction(scCls, profile::name::SetVisibilityFn);
        g_getCompLocFn  = R::FindFunction(scCls, L"K2_GetComponentLocation");
        g_setWorldLocFn = R::FindFunction(scCls, L"K2_SetWorldLocation");
    }
    if (!g_setFloatParamFn || !g_setVisFn || !g_getCompLocFn || !g_setWorldLocFn)
        UE_LOGW("drone: FX polish UFunctions partial (SetFloatParameter=%p SetVisibility=%p "
                "GetCompLoc=%p SetWorldLoc=%p) -- dust / signal light may be limited",
                g_setFloatParamFn, g_setVisFn, g_getCompLocFn, g_setWorldLocFn);
    g_fxResolved = true;
    UE_LOGI("drone: FX resolved canTakeOff@0x%04X eff_droneDust@0x%04X audio_alarm@0x%04X "
            "light_alarm@0x%04X hasSack@0x%04X sack@%d",
            g_canTakeOffOff, g_dustOff, g_audioAlarmOff, g_lightAlarmOff, g_hasSackOff, g_sackOff);
    return true;
}

void* ReadComp(void* drone, int32_t off) {
    if (!drone || off < 0) return nullptr;
    void* c = *reinterpret_cast<void**>(reinterpret_cast<char*>(drone) + off);
    return (c && R::IsLive(c)) ? c : nullptr;
}

// UActorComponent::IsActive returns bIsActive, so the bit is read where it lives.
bool ComponentIsActive(void* comp) {
    if (!comp || g_activeByteOff < 0) return false;
    return (*(reinterpret_cast<const uint8_t*>(comp) + g_activeByteOff) & g_activeMask) != 0;
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
    void* cls = object_index::ClassByName(L"drone_C");   // one lookup, loaded or not
    if (!cls) return false;  // not loaded yet -- caller retries

    const int32_t activeOff = R::FindPropertyOffset(cls, L"Active");
    if (activeOff < 0) {
        static bool s_said = false;
        if (!s_said) {
            s_said = true;
            UE_LOGW("drone: drone_C has no Active field -- the drone lane is off");
        }
        return false;
    }

    g_cls = cls;
    g_activeOff = activeOff;
    g_leaveAfter5minOff = R::FindPropertyOffset(cls, L"leaveAfter5min");
    if (g_leaveAfter5minOff < 0)
        UE_LOGW("drone: reflected leaveAfter5min offset not found -- the leave timer does not sync");
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("drone: resolved drone_C=%p Active@0x%04X", cls, activeOff);
    return true;
}

void* Find() {
    if (!EnsureResolved()) return nullptr;
    return world_singleton::Find(L"drone_C");   // the delivery drone, one of it
}

bool IsActive(void* drone) {
    if (!drone || g_activeOff < 0) return false;
    return *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(drone) + g_activeOff);
}

bool ReadLeaveAfter5min(void* drone, bool& out) {
    if (!drone || !EnsureResolved() || g_leaveAfter5minOff < 0) return false;
    out = *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(drone) + g_leaveAfter5minOff);
    return true;
}

bool WriteLeaveAfter5min(void* drone, bool value) {
    if (!drone || !EnsureResolved() || g_leaveAfter5minOff < 0) return false;
    *reinterpret_cast<bool*>(reinterpret_cast<char*>(drone) + g_leaveAfter5minOff) = value;
    return true;
}

bool ReadPose(void* drone, FVector& loc, FRotator& rot) {
    if (!drone) return false;
    if (E::TryGetRootPoseRaw(drone, loc, rot)) return true;
    return E::TryGetActorLocation(drone, loc) && E::TryGetActorRotation(drone, rot);
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
    if (!drone) return 0;
    uint8_t bits = 0;
    bool leaveAfter5min = false;
    if (ReadLeaveAfter5min(drone, leaveAfter5min) && leaveAfter5min) bits |= kFxLeaveAfter5min;
    if (!EnsureFxResolved()) return bits;
    if (void* dust = ReadComp(drone, g_dustOff))
        if (ComponentIsActive(dust)) bits |= kFxDust;
    auto rb = [drone](int32_t off) -> bool {
        return off >= 0 && *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(drone) + off);
    };
    if (rb(g_canTakeOffOff)) bits |= kFxArrived;   // parked/arrived -> the interaction gate
    if (rb(g_hasSackOff))    bits |= kFxHasSack;   // the sack aboard -> the action-option prerequisite
    return bits;
}

bool ReadDustAnchor(void* drone, FVector& out) {
    if (!drone || !EnsureFxResolved() || !g_getCompLocFn) return false;
    void* dust = ReadComp(drone, g_dustOff);
    if (!dust) return false;
    ParamFrame f(g_getCompLocFn);
    if (!f.valid() || !Call(dust, f)) return false;
    out = f.Get<FVector>(L"ReturnValue");
    return true;
}

void ApplyDustMirror(void* drone, bool on, const FVector& anchor) {
    if (!drone || !EnsureFxResolved()) return;
    void* dust = ReadComp(drone, g_dustOff);
    if (!dust) return;
    // 1. The blueprint's own edge form: SetActive(want, bReset=false) only when IsActive() != want.
    //    Load-bearing both ways -- it deactivates on leaving the ground AND re-arms the one-loop
    //    system after it self-completes mid-hover, which a latched rising-edge replay cannot do.
    if (ComponentIsActive(dust) != on) SetComponentActive(dust, on);
    if (!on) return;
    // 2. Pin the component to the host's ground-trace hit. eff_droneDust is bAbsoluteLocation with
    //    NO relative offset, so unmoved it renders at WORLD ORIGIN and its fixed relative bounding
    //    box is frustum-culled, so the dust draws invisibly rather than not at all. Same call
    //    form as the
    //    blueprint: sweep=false, teleport=false. The blueprint VInterpTo's toward the hit; we do
    //    not, because the host streams its already-interpolated component location at 20 Hz.
    if (g_setWorldLocFn) {
        ParamFrame f(g_setWorldLocFn);
        if (f.valid()) {
            f.Set<FVector>(L"NewLocation", anchor);
            f.Set<bool>(L"bSweep", false);
            f.Set<bool>(L"bTeleport", false);
            Call(dust, f);
        }
    }
    // 3. The blueprint's intensity formula: 'dust' = 1 - distance(trace start, hit) / 2000, clamped
    //    to [0,1]. Lazy-resolve the FName -- it needs Kismet up, which post-boot it is.
    if (g_setFloatParamFn) {
        if (g_dustFName.ComparisonIndex == 0 && g_dustFName.Number == 0)
            g_dustFName = ue_wrap::fname_utils::StringToFName(L"dust");
        FVector loc{};   // unread: the dust keeps its last value
        if ((g_dustFName.ComparisonIndex != 0 || g_dustFName.Number != 0) && E::TryGetActorLocation(drone, loc)) {
            const float dx = loc.X - anchor.X, dy = loc.Y - anchor.Y, dz = loc.Z - anchor.Z;
            float dustVal = 1.f - std::sqrt(dx * dx + dy * dy + dz * dz) / kDustTraceLen;
            if (dustVal < 0.f) dustVal = 0.f;
            if (dustVal > 1.f) dustVal = 1.f;
            ParamFrame f(g_setFloatParamFn);
            if (f.valid()) {
                f.SetRaw(L"ParameterName", &g_dustFName, sizeof(g_dustFName));
                f.Set<float>(L"Param", dustVal);
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
    // shows no action options. Mirror the host's values so a parked drone is interactable (the
    // actionOptionIndex gate is `IFNOT(canTakeOff) -> "The drone is in motion"`; getActionOptions needs
    // hasSack for the open and drop-sack options to appear).
    if (g_canTakeOffOff >= 0)
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(drone) + g_canTakeOffOff) = canTakeOff;
    if (g_hasSackOff >= 0) {
        bool& field = *reinterpret_cast<bool*>(reinterpret_cast<char*>(drone) + g_hasSackOff);
        const bool changed = field != hasSack;
        field = hasSack;
        // dropSack and putSackOn show and hide the sack on the body; the mirror never runs them.
        if (changed && g_setVisFn) {
            if (void* sack = ReadComp(drone, g_sackOff)) {
                ParamFrame f(g_setVisFn);
                if (f.valid()) {
                    f.Set<bool>(L"bNewVisibility", hasSack);
                    f.Set<bool>(L"bPropagateToChildren", false);
                    Call(sack, f);
                }
            }
        }
    }
}

void* InventoryOf(void* drone) {
    static R::InstanceOffset s_invOff{L"inventoryComponent"};
    if (!drone) return nullptr;
    const int32_t off = s_invOff.Of(drone);
    if (off < 0) return nullptr;
    void* const inv = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(drone) + off);
    return inv && R::IsLive(inv) ? inv : nullptr;
}

bool ReadCargo(void* drone, std::vector<CargoItem>& out, size_t maxItems) {
    void* const inv = InventoryOf(drone);
    return inv && container_inventory::ReadContents(inv, out, maxItems);
}

CargoWrite WriteCargo(void* drone, const std::vector<CargoItem>& items) {
    void* const inv = InventoryOf(drone);
    if (!inv) return CargoWrite::NotReady;
    switch (container_inventory::WriteContents(inv, items)) {
        case container_inventory::WriteResult::Written:  return CargoWrite::Written;
        case container_inventory::WriteResult::NotReady: return CargoWrite::NotReady;
        case container_inventory::WriteResult::Refused:  break;
    }
    return CargoWrite::Refused;
}

bool IsCargo(void* inventory) {
    return inventory && inventory == InventoryOf(Find());
}

bool DropSack(void* drone) {
    if (!drone) return false;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(drone), L"dropSack");
    if (!fn) return false;
    ParamFrame f(fn);
    return f.valid() && Call(drone, f);
}

bool PutSackOn(void* drone, void* sack) {
    if (!drone || !sack) return false;
    void* const fn = R::FindDispatchFunctionCached(R::ClassOf(drone), L"putSackOn");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"sack", sack);
    f.Set<void*>(L"velocityComponent", E::RootComponentOf(sack));
    return Call(drone, f);
}

void* OfSellLocation(void* sellLocation) {
    static R::InstanceOffset s_droneOff{L"drone"};
    if (!sellLocation) return nullptr;
    const int32_t off = s_droneOff.Of(sellLocation);
    if (off < 0) return nullptr;
    void* const d = *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(sellLocation) + off);
    return d && R::IsLive(d) ? d : nullptr;
}

}  // namespace ue_wrap::drone

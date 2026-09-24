// ue_wrap/devices/door.cpp -- see ue_wrap/devices/door.h; engine access for the base doors.
// All field offsets are resolved from the live class by reflection rather than hardcoded, so
// they stay correct across game builds; the known offsets are kept only as a logged fallback
// if the reflected walk ever fails to find a property.

#include "ue_wrap/devices/door.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine_component.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace ue_wrap::door {
namespace {

namespace R = reflection;

// Resolved once at EnsureResolved, then read-only. Published via the resolved flag's release
// store and acquire load, so any thread that sees it set also sees the fully written caches.
// Game-thread writes; observer reads.
std::atomic<bool> g_resolved{false};

void*   g_doorCls      = nullptr;  // door_C UClass
int32_t g_keyOff       = -1;       // AtriggerBase_C::Key
int32_t g_isOpenedOff  = -1;       // Adoor_C::isOpened
int32_t g_isMovingOff  = -1;       // Adoor_C::isMoving -- swing in progress
void*   g_doorOpenFn   = nullptr;  // Adoor_C::doorOpen(bool bypassCheck)
void*   g_doorCloseFn  = nullptr;  // Adoor_C::doorClose(bool bypassCheck)
void*   g_moveFinishFn = nullptr;  // Adoor_C::move__FinishedFunc() -- sets isOpened + stops the timeline
void*   g_moveUpdateFn = nullptr;  // Adoor_C::move__UpdateFunc()  -- lerps the door MESH from move_a (the visual)
// The move timeline's output value and direction live at fixed offsets (the reflected
// property names carry a per-asset GUID suffix, so they are not name-resolvable; the layout
// is stable for this build, verified by the door probe).
constexpr int32_t kMoveAlphaOff = 0x0340;  // float  move_a_<guid>        (0=closed .. 1=open)
constexpr int32_t kMoveDirOff   = 0x0344;  // uint8  move__Direction_<guid> (0=Forward/open, 1=Backward/close)
int32_t g_autocloseOff = -1;       // Adoor_C::autoclose
int32_t g_sensorOff    = -1;       // Adoor_C::sensor (UBoxComponent*); no fallback
int32_t g_sensorOverlapsOff = -1;  // Adoor_C::sensorOverlaps (TArray<AActor*>); no fallback
// The door's power flag, which a press gates on first. The gamemode's power trigger, the keypad's
// set-active and the save write it.
int32_t g_activeOff      = -1;     // Adoor_C::Active (power)

// The documented fallbacks from the header dump.
constexpr int32_t kKeyOffFallback         = 0x0260;
constexpr int32_t kIsOpenedOffFallback    = 0x0350;
constexpr int32_t kIsMovingOffFallback    = 0x0351;
constexpr int32_t kAutocloseOffFallback   = 0x0353;
constexpr int32_t kActiveOffFallback      = 0x0352;

// A per-door cache so the restore can undo the client's suppression. Game thread in practice,
// guarded anyway.
struct SavedAutonomy { bool autoclose; };
std::mutex g_autoMtx;
std::unordered_map<void*, SavedAutonomy> g_saved;

// The smart-apply verify list: a door we just tried to animate; if it has not reached the
// target by the deadline (the swing froze, beyond tick range) it is force-snapped. Game
// thread only, bounded (entries clear within a second), drained by the tick.
struct VerifyEntry {
    bool target;
    std::chrono::steady_clock::time_point deadline;
    ue_wrap::CachedObjRef ref;  // the slot-validated door (the map key is compare-only)
};
std::unordered_map<void*, VerifyEntry> g_verify;

// One of the door's entry verbs on this instance: its class's own override, resolved per class
// and memoised by the reflection layer.
void* EntryVerb(void* door, const wchar_t* name) {
    if (!door) return nullptr;
    void* cls = R::ClassOf(door);
    return cls ? R::FindDispatchFunctionCached(cls, name) : nullptr;
}

}  // namespace

bool EnsureResolved() {
    if (g_resolved.load(std::memory_order_acquire)) return true;

    void* doorCls = R::FindClass(L"door_C");
    if (!doorCls) return false;  // BP class not loaded yet -- caller retries

    // The key is declared on the trigger base; the property lookup does not climb to the
    // superclass, so query the declaring class. The opened flag is declared on the door.
    int32_t keyOff = -1;
    if (void* trigCls = R::FindClass(L"triggerBase_C")) {
        keyOff = R::FindPropertyOffset(trigCls, L"Key");
    }
    if (keyOff < 0) {
        UE_LOGW("door: reflected Key offset not found -- using fallback 0x%04X", kKeyOffFallback);
        keyOff = kKeyOffFallback;
    }
    int32_t isOpenedOff = R::FindPropertyOffset(doorCls, L"isOpened");
    if (isOpenedOff < 0) {
        UE_LOGW("door: reflected isOpened offset not found -- using fallback 0x%04X", kIsOpenedOffFallback);
        isOpenedOff = kIsOpenedOffFallback;
    }
    int32_t isMovingOff = R::FindPropertyOffset(doorCls, L"isMoving");
    if (isMovingOff < 0) {
        UE_LOGW("door: reflected isMoving offset not found -- using fallback 0x%04X", kIsMovingOffFallback);
        isMovingOff = kIsMovingOffFallback;
    }
    int32_t autocloseOff = R::FindPropertyOffset(doorCls, L"autoclose");
    if (autocloseOff < 0) {
        UE_LOGW("door: reflected autoclose offset not found -- using fallback 0x%04X", kAutocloseOffFallback);
        autocloseOff = kAutocloseOffFallback;
    }
    // The sensor and its contents: read by name or not at all, since a guessed offset would hand a
    // reader a pointer from the wrong field.
    const int32_t sensorOff = R::FindPropertyOffset(doorCls, L"sensor");
    const int32_t sensorOverlapsOff = R::FindPropertyOffset(doorCls, L"sensorOverlaps");
    int32_t activeOff = R::FindPropertyOffset(doorCls, L"Active");
    if (activeOff < 0) activeOff = kActiveOffFallback;

    void* openFn  = R::FindFunction(doorCls, L"doorOpen");
    void* closeFn = R::FindFunction(doorCls, L"doorClose");
    if (!openFn || !closeFn) {
        UE_LOGW("door: UFunction resolve incomplete (doorOpen=%p doorClose=%p) -- not ready",
                openFn, closeFn);
        return false;
    }
    void* moveFinishFn = R::FindFunction(doorCls, L"move__FinishedFunc");  // for ForceOpen/ForceClose
    void* moveUpdateFn = R::FindFunction(doorCls, L"move__UpdateFunc");    // drives the mesh from move_a
    if (!moveFinishFn || !moveUpdateFn)
        UE_LOGW("door: move__FinishedFunc=%p / move__UpdateFunc=%p -- force-snap may not move the mesh",
                moveFinishFn, moveUpdateFn);

    g_moveFinishFn = moveFinishFn;
    g_moveUpdateFn = moveUpdateFn;
    g_doorCls      = doorCls;
    g_keyOff       = keyOff;
    g_isOpenedOff  = isOpenedOff;
    g_isMovingOff  = isMovingOff;
    g_autocloseOff = autocloseOff;
    g_sensorOff    = sensorOff;
    g_sensorOverlapsOff = sensorOverlapsOff;
    g_activeOff      = activeOff;
    g_doorOpenFn   = openFn;
    g_doorCloseFn  = closeFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("door: resolved door_C=%p Key@0x%04X isOpened@0x%04X autoclose@0x%04X "
            "sensorOverlaps@0x%04X Active@0x%04X doorOpen=%p doorClose=%p", doorCls, keyOff,
            isOpenedOff, autocloseOff, sensorOverlapsOff < 0 ? 0xFFFF : sensorOverlapsOff, activeOff,
            openFn, closeFn);
    return true;
}

int ReadSensorOverlaps(void* door, void** out, int maxOut) {
    if (!door || g_sensorOverlapsOff < 0) return -1;
    // TArray<AActor*>: the element pointer, then the count and the capacity.
    const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(door) + g_sensorOverlapsOff;
    void* const* data = *reinterpret_cast<void* const* const*>(base);
    const std::int32_t num = *reinterpret_cast<const std::int32_t*>(base + sizeof(void*));
    const std::int32_t max = *reinterpret_cast<const std::int32_t*>(base + sizeof(void*) + 4);
    if (num < 0 || num > max || (num > 0 && !data)) return -1;  // not an array this build can read
    for (int i = 0; i < num && i < maxOut; ++i) out[i] = data[i];
    return num;
}

bool ReadSensorBox(void* door, FVector& centre, FVector& halfExtent) {
    if (!door || g_sensorOff < 0) return false;
    void* sensor = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(door) + g_sensorOff);
    if (!sensor || !R::IsLive(sensor)) return false;
    void* cls = R::ClassOf(sensor);
    void* fn = cls ? R::FindDispatchFunctionCached(cls, L"GetScaledBoxExtent") : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !Call(sensor, f)) return false;
    halfExtent = f.Get<FVector>(L"ReturnValue");
    centre = ue_wrap::engine::GetComponentLocation(sensor);
    return true;
}

bool IsDoor(void* obj) {
    if (!obj || !g_doorCls) return false;
    void* cls = R::ClassOf(obj);
    if (!cls) return false;
    void* bases[1] = { g_doorCls };
    return R::IsDescendantOfAny(cls, bases, 1);
}

std::wstring GetKeyString(void* door) {
    if (!door || g_keyOff < 0) return std::wstring();
    const R::FName& key = *reinterpret_cast<const R::FName*>(
        reinterpret_cast<const char*>(door) + g_keyOff);
    return R::ToString(key);
}

bool TryReadOpen(void* door, bool& open) {
    if (!door || g_isOpenedOff < 0) return false;
    open = *reinterpret_cast<const bool*>(
        reinterpret_cast<const char*>(door) + g_isOpenedOff);
    return true;
}

bool TryReadOpenIntent(void* door, bool& open) {
    if (!door || g_isOpenedOff < 0) return false;
    const char* base = reinterpret_cast<const char*>(door);
    const bool isOpened = *reinterpret_cast<const bool*>(base + g_isOpenedOff);
    // While the door is mid-swing, report the destination (the direction, set at swing start;
    // forward is opening) instead of the opened flag (set at swing end, half a second later), so
    // the host poll broadcasts an open or close the instant it begins, frame-symmetric with the
    // client's input-edge request. A settled door reads the opened flag, the authoritative
    // settled state. The open and close verbs set moving and the direction synchronously within
    // the press dispatch, so the very next poll tick catches the intent.
    const bool moving = (g_isMovingOff >= 0) &&
        *reinterpret_cast<const bool*>(base + g_isMovingOff);
    if (moving) {
        const uint8_t dir = *reinterpret_cast<const uint8_t*>(base + kMoveDirOff);
        open = (dir == 0);  // 0 = Forward = opening
    } else {
        open = isOpened;
    }
    return true;
}

bool CallPress(void* door, void* player, uint8_t action) {
    void* fn = EntryVerb(door, L"actionOptionIndex");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"player", player);
    f.Set<uint8_t>(L"action", action);
    return Call(door, f);
}

bool CallHit(void* door, void* instigator, float damage) {
    void* fn = EntryVerb(door, L"addDamage");
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"actor", instigator);
    f.Set<float>(L"damage", damage);
    return Call(door, f);
}

bool CallCrowbarOpen(void* door) {
    void* fn = EntryVerb(door, L"crowbarOpen");
    if (!fn) return false;  // not a pryable door
    ParamFrame f(fn);
    if (!f.valid()) return false;
    return Call(door, f);  // no crowbar: the pry is the same hit of 100 either way
}

bool CallDoorOpen(void* door, bool bypass) {
    if (!door || !g_doorOpenFn) return false;
    ParamFrame f(g_doorOpenFn);
    if (!f.valid()) return false;
    f.Set<bool>(L"bypassCheck", bypass);
    return Call(door, f);
}

bool CallDoorClose(void* door, bool bypass) {
    if (!door || !g_doorCloseFn) return false;
    ParamFrame f(g_doorCloseFn);
    if (!f.valid()) return false;
    f.Set<bool>(L"bypassCheck", bypass);
    return Call(door, f);
}

void SetActive(void* door, bool on) {
    if (!door || !g_resolved.load(std::memory_order_acquire) || g_activeOff < 0) return;
    *reinterpret_cast<bool*>(reinterpret_cast<char*>(door) + g_activeOff) = on;
}

// Snap a door fully to a state, mesh and flag, proximity-independent: set the timeline alpha
// to the end and the direction, call the update function (which lerps the mesh from the
// alpha, the visual snap; the finished function only sets the flag and stops the timeline,
// never moving the mesh), then the finished function for the state.
static void ForceTo(void* door, bool open) {
    // Idempotent: if the opened flag already matches, do nothing. The finished function re-fires
    // the open or close sound and the opened delegate, so a second force on an already-open door
    // double-sounds; two callers can reach the same open door in adjacent ticks (the keypad
    // accept's force open and the door's own native chain from the replayed digit), and this
    // guard makes both safe.
    if (g_isOpenedOff >= 0 &&
        *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(door) + g_isOpenedOff) == open)
        return;
    *reinterpret_cast<float*>(reinterpret_cast<char*>(door) + kMoveAlphaOff) = open ? 1.0f : 0.0f;
    *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(door) + kMoveDirOff)  = open ? 0 : 1;  // Forward/Backward
    if (g_moveUpdateFn) { ParamFrame u(g_moveUpdateFn); if (u.valid()) Call(door, u); }  // move the MESH
    if (g_moveFinishFn) { ParamFrame f(g_moveFinishFn); if (f.valid()) Call(door, f); }  // set isOpened + stop
}

void ForceOpen(void* door)  { if (door) ForceTo(door, true); }
void ForceClose(void* door) { if (door) ForceTo(door, false); }

void SmartApply(void* door, bool open) {
    if (!door) return;
    // If the door is already swinging toward this target, do nothing: this is the opener's own
    // use animation (its sound already played) receiving the echo of its own request, and
    // re-triggering it, or registering a verify that later force-snaps it, plays the sound a
    // second time. Let the swing finish.
    const bool moving = *reinterpret_cast<bool*>(reinterpret_cast<char*>(door) + 0x0351);  // isMoving
    const uint8_t dir = *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(door) + kMoveDirOff);  // 0=Forward/open
    if (moving && ((open && dir == 0) || (!open && dir == 1))) return;  // already going the right way -> no re-trigger, no double sound
    // Idempotent: a settled door already at the target is left alone. Re-running the blueprint
    // chain on a matching door is destructive, not just wasteful: the open swing is additive
    // (the target is the current pose plus a delta), so re-opening an already-open door drives
    // it past its frame into the wall, the join-time clipping every connect snapshot once
    // produced on saved-open doors. The mid-swing case stays non-idempotent on purpose: the
    // opened flag lags the animation, so a match while moving the wrong way means the authority
    // wants the swing reversed; fall through and re-command it.
    if (!moving && g_isOpenedOff >= 0 &&
        *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(door) + g_isOpenedOff) == open)
        return;
    // Play the native animated swing (smooth wherever the door ticks, any distance up to its
    // real tick range, no magic radius).
    if (open) CallDoorOpen(door, true); else CallDoorClose(door, true);
    // The swing freezes beyond tick range (far, invisible). Verify shortly: a near door reaches
    // the target before the deadline (removed, no snap); a far frozen door is force-snapped so
    // its state stays correct. The deadline exceeds the longest swing, so a slow but completing
    // animation is never double-finished.
    VerifyEntry ve{ open, std::chrono::steady_clock::now() + std::chrono::milliseconds(1500), {} };
    ve.ref.Set(door);  // fresh at the apply seam
    g_verify[door] = ve;
}

void TickSmartApply() {
    if (g_verify.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_verify.begin(); it != g_verify.end();) {
        void* door = it->second.ref.Get();  // slot-validated (the key is compare-only)
        if (!door || g_isOpenedOff < 0) { it = g_verify.erase(it); continue; }
        const bool cur = *reinterpret_cast<bool*>(reinterpret_cast<char*>(door) + g_isOpenedOff);
        if (cur == it->second.target) { it = g_verify.erase(it); continue; }  // animation completed -> smooth, no snap
        if (now >= it->second.deadline) {                                     // froze (far) -> guarantee the state
            if (it->second.target) ForceOpen(door); else ForceClose(door);
            it = g_verify.erase(it);
        } else {
            ++it;
        }
    }
}

void SuppressClientAutonomy(void* door) {
    if (!door) return;
    bool* autoclose = (g_autocloseOff >= 0) ? reinterpret_cast<bool*>(reinterpret_cast<char*>(door) + g_autocloseOff) : nullptr;
    bool firstTime = false;
    {
        std::lock_guard<std::mutex> lk(g_autoMtx);
        if (g_saved.find(door) == g_saved.end()) {
            g_saved[door] = SavedAutonomy{ autoclose ? *autoclose : true };
            firstTime = true;
        }
    }
    // A client door is render-only: with autoclose off its sensor check never arms, so an applied
    // host state stands. The player's own press, hit and pry do not run on this copy; the host runs
    // them (coop/interactables/door_verb_intent).
    if (autoclose) *autoclose = false;
    if (firstTime) UE_LOGI("door: client autonomy suppressed (autoclose=0) for %p", door);
}

void RestoreClientAutonomy(void* door) {
    if (!door) return;
    SavedAutonomy saved;
    {
        std::lock_guard<std::mutex> lk(g_autoMtx);
        auto it = g_saved.find(door);
        if (it == g_saved.end()) return;  // never suppressed
        saved = it->second;
        g_saved.erase(it);
    }
    if (g_autocloseOff >= 0)
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(door) + g_autocloseOff) = saved.autoclose;
}

}  // namespace ue_wrap::door

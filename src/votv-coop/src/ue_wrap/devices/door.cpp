// ue_wrap/devices/door.cpp -- see ue_wrap/devices/door.h; engine access for the base doors.
// All field offsets are resolved from the live class by reflection rather than hardcoded, so
// they stay correct across game builds; the known offsets are kept only as a logged fallback
// if the reflected walk ever fails to find a property.

#include "ue_wrap/devices/door.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/reflection.h"

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
int32_t g_sensorOff    = -1;       // Adoor_C::sensor (UBoxComponent*)
void*   g_setGenOverlapFn = nullptr; // UPrimitiveComponent::SetGenerateOverlapEvents(bool) (lazy)
// The manual-open gate, byte-exact from the door blueprint: a player's use press reaches the
// toggle only past the door's own power check, and the toggle's open needs neither jammed
// nor super-closed, so a door opens on the press iff powered, not jammed and not
// super-closed. All the door's own fields, not the keypad's (a read of the keypad's accept
// flag, a crosshair-hover flag, mis-locked powered doors). The power is driven by the
// gamemode's power trigger, the keypad's set-active and the save.
int32_t g_activeOff      = -1;     // Adoor_C::Active (power)
int32_t g_superClosedOff = -1;     // Adoor_C::superClosed
int32_t g_jammedOff      = -1;     // Adoor_C::jammed

// The documented fallbacks from the header dump.
constexpr int32_t kKeyOffFallback         = 0x0260;
constexpr int32_t kIsOpenedOffFallback    = 0x0350;
constexpr int32_t kIsMovingOffFallback    = 0x0351;
constexpr int32_t kAutocloseOffFallback   = 0x0353;
constexpr int32_t kSensorOffFallback      = 0x0308;
constexpr int32_t kActiveOffFallback      = 0x0352;
constexpr int32_t kSuperClosedOffFallback = 0x0378;
constexpr int32_t kJammedOffFallback      = 0x03B0;

// A per-door cache so the restore can undo a suppression. Two distinct maps: the client
// suppresses every synced door (render-only); the host suppresses only the doors a remote
// client is holding open. A process is one role, so the maps never alias, but separate maps
// keep the two lifecycles independent. Game thread in practice, guarded anyway.
struct SavedAutonomy { bool autoclose; };
std::mutex g_autoMtx;
std::unordered_map<void*, SavedAutonomy> g_saved;      // CLIENT-side render-only suppression
std::unordered_map<void*, SavedAutonomy> g_hostHeld;   // HOST-side per-held-door suppression

// The smart-apply verify list: a door we just tried to animate; if it has not reached the
// target by the deadline (the swing froze, beyond tick range) it is force-snapped. Game
// thread only, bounded (entries clear within a second), drained by the tick.
struct VerifyEntry {
    bool target;
    std::chrono::steady_clock::time_point deadline;
    ue_wrap::CachedObjRef ref;  // the slot-validated door (the map key is compare-only)
};
std::unordered_map<void*, VerifyEntry> g_verify;

// Toggle a door's sensor overlap events. False on the client kills the sensor check at its
// source (no overlap events, so no local auto open or close), so an applied host state
// cannot be reverted by local door logic. Resolves the UFunction lazily off the live sensor
// component's class. Best effort: if the parameter name differs, a bool defaults to false,
// the disable we want.
void SetSensorOverlap(void* door, bool enable) {
    if (!door || g_sensorOff < 0) return;
    void* sensor = *reinterpret_cast<void**>(reinterpret_cast<char*>(door) + g_sensorOff);
    if (!sensor || !R::IsLive(sensor)) return;
    if (!g_setGenOverlapFn) {
        if (void* cls = R::ClassOf(sensor))
            g_setGenOverlapFn = R::FindFunction(cls, L"SetGenerateOverlapEvents");
        static bool s_warned = false;
        if (!g_setGenOverlapFn && !s_warned) {
            s_warned = true;
            UE_LOGW("door: SetGenerateOverlapEvents UFunction NOT found on the sensor -- "
                    "client sensor cannot be disabled (door suppression will be autoclose-only)");
        }
    }
    if (!g_setGenOverlapFn) return;
    ParamFrame f(g_setGenOverlapFn);
    if (!f.valid()) return;
    f.Set<bool>(L"bInGenerateOverlapEvents", enable);
    Call(sensor, f);
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
    int32_t sensorOff = R::FindPropertyOffset(doorCls, L"sensor");
    if (sensorOff < 0) {
        UE_LOGW("door: reflected sensor offset not found -- using fallback 0x%04X", kSensorOffFallback);
        sensorOff = kSensorOffFallback;
    }
    // The open-gate fields: the door's own power, jam and super-closed flags.
    int32_t activeOff = R::FindPropertyOffset(doorCls, L"Active");
    if (activeOff < 0) activeOff = kActiveOffFallback;
    int32_t superClosedOff = R::FindPropertyOffset(doorCls, L"superClosed");
    if (superClosedOff < 0) superClosedOff = kSuperClosedOffFallback;
    int32_t jammedOff = R::FindPropertyOffset(doorCls, L"jammed");
    if (jammedOff < 0) jammedOff = kJammedOffFallback;

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
    g_activeOff      = activeOff;
    g_superClosedOff = superClosedOff;
    g_jammedOff      = jammedOff;
    g_doorOpenFn   = openFn;
    g_doorCloseFn  = closeFn;
    g_resolved.store(true, std::memory_order_release);
    UE_LOGI("door: resolved door_C=%p Key@0x%04X isOpened@0x%04X autoclose@0x%04X sensor@0x%04X "
            "Active@0x%04X superClosed@0x%04X jammed@0x%04X doorOpen=%p doorClose=%p",
            doorCls, keyOff, isOpenedOff, autocloseOff, sensorOff, activeOff, superClosedOff,
            jammedOff, openFn, closeFn);
    return true;
}

void* DoorClass() { return g_doorCls; }
void* DoorOpenFn() { return g_doorOpenFn; }
void* DoorCloseFn() { return g_doorCloseFn; }

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

bool CanOpen(void* door) {
    // The byte-exact door gate: a player's press opens the door iff its own power is on and it
    // is neither jammed nor super-closed. Fail-open on null or unresolved, so a resolve failure
    // never locks every door. Three bool reads, no dispatch; cheap, called per open request.
    // Guarded on the resolved flag (acquire): once set, every offset is written (reflected or
    // fallback, never -1), so the reads are valid; before it, fail-open. The request handler
    // already gates on the resolve; this covers any other call site.
    if (!door || !g_resolved.load(std::memory_order_acquire)) return true;
    auto rb = [door](int32_t off) -> bool {
        return off >= 0 && *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(door) + off);
    };
    return rb(g_activeOff) && !rb(g_jammedOff) && !rb(g_superClosedOff);
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

bool GetActive(void* door) {
    if (!door || !g_resolved.load(std::memory_order_acquire) || g_activeOff < 0) return true;
    return *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(door) + g_activeOff);
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
    // A client door is render-only: no autoclose and no sensor overlaps, so the local sensor
    // check cannot auto-revert an applied host state. The use press stays interactive (an
    // optimistic local open is fine; the host's authoritative state lands right after).
    if (autoclose) *autoclose = false;
    SetSensorOverlap(door, false);
    if (firstTime) UE_LOGI("door: client autonomy suppressed (sensor off + autoclose=0) for %p", door);
}

void SuppressHostHeldDoor(void* door) {
    if (!door) return;
    bool* autoclose = (g_autocloseOff >= 0) ? reinterpret_cast<bool*>(reinterpret_cast<char*>(door) + g_autocloseOff) : nullptr;
    bool firstTime = false;
    {
        std::lock_guard<std::mutex> lk(g_autoMtx);
        if (g_hostHeld.find(door) == g_hostHeld.end()) {
            g_hostHeld[door] = SavedAutonomy{ autoclose ? *autoclose : true };
            firstTime = true;
        }
    }
    // While a remote client holds this door open, the host treats it render-only too, the same
    // recipe as the client (no autoclose, no sensor overlaps), so the host's own sensor check
    // cannot autoclose the door it opened for the client. Lazy and once per door, never per tick
    // or in bulk.
    if (autoclose) *autoclose = false;
    SetSensorOverlap(door, false);
    if (firstTime) UE_LOGI("door: host held-door suppressed (sensor off + autoclose=0) for %p", door);
}

void ReleaseHostHeldDoor(void* door) {
    if (!door) return;
    SavedAutonomy saved;
    {
        std::lock_guard<std::mutex> lk(g_autoMtx);
        auto it = g_hostHeld.find(door);
        if (it == g_hostHeld.end()) return;  // not a held door
        saved = it->second;
        g_hostHeld.erase(it);
    }
    // The client released its hold: restore the authored autoclose and re-enable the sensor so
    // the host's native door autonomy resumes, then close the door honouring the real guards
    // (exactly one close edge; the host poll broadcasts the resulting off). Close with the same
    // near-or-far visual as every other apply (an unconditional snap made opens animate and
    // closes snap), and close before restoring the sensor so the host's own proximity logic
    // cannot fight the close mid-swing.
    SmartApply(door, false);
    if (g_autocloseOff >= 0)
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(door) + g_autocloseOff) = saved.autoclose;
    SetSensorOverlap(door, true);
    UE_LOGI("door: host held-door released (smart-closed + autonomy restored) for %p", door);
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
    SetSensorOverlap(door, true);  // re-enable the sensor's auto open/close
}

}  // namespace ue_wrap::door

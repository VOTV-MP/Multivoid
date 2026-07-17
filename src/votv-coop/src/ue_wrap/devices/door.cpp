// ue_wrap/door.cpp -- see ue_wrap/door.h. Engine access for VOTV base doors.
//
// All field offsets are resolved from the live class via reflection
// (FindPropertyOffset) rather than hardcoded, so they stay correct across game
// builds (version-tagging rule). The known Alpha 0.9.0-n offsets are kept only as
// a logged fallback if the reflected walk ever fails to find the property.

#include "ue_wrap/devices/door.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace ue_wrap::door {
namespace {

namespace R = reflection;

// Resolved once at EnsureResolved, then read-only. Published via the g_resolved
// release-store / acquire-load so any thread that sees g_resolved==true also sees
// the fully-written caches below. Game-thread writes; observer reads.
std::atomic<bool> g_resolved{false};

void*   g_doorCls      = nullptr;  // door_C UClass
int32_t g_keyOff       = -1;       // AtriggerBase_C::Key  (Alpha 0.9.0-n: 0x0260)
int32_t g_isOpenedOff  = -1;       // Adoor_C::isOpened    (Alpha 0.9.0-n: 0x0350)
int32_t g_isMovingOff  = -1;       // Adoor_C::isMoving    (Alpha 0.9.0-n: 0x0351) -- swing in progress
void*   g_doorOpenFn   = nullptr;  // Adoor_C::doorOpen(bool bypassCheck)
void*   g_doorCloseFn  = nullptr;  // Adoor_C::doorClose(bool bypassCheck)
void*   g_moveFinishFn = nullptr;  // Adoor_C::move__FinishedFunc() -- sets isOpened + stops the timeline
void*   g_moveUpdateFn = nullptr;  // Adoor_C::move__UpdateFunc()  -- lerps the door MESH from move_a (the visual)
// The `move` timeline output value + direction live at fixed offsets (the reflected property
// names carry a per-asset GUID suffix -- "move_a_<guid>" / "move__Direction_<guid>" -- so they
// are not name-resolvable; the layout is stable for this build, verified by the door probe).
constexpr int32_t kMoveAlphaOff = 0x0340;  // float  move_a_<guid>        (0=closed .. 1=open)
constexpr int32_t kMoveDirOff   = 0x0344;  // uint8  move__Direction_<guid> (0=Forward/open, 1=Backward/close)
int32_t g_autocloseOff = -1;       // Adoor_C::autoclose  (Alpha 0.9.0-n: 0x0353)
int32_t g_sensorOff    = -1;       // Adoor_C::sensor (UBoxComponent*) (Alpha 0.9.0-n: 0x0308)
void*   g_setGenOverlapFn = nullptr; // UPrimitiveComponent::SetGenerateOverlapEvents(bool) (lazy)
// Manual-open gate (CanOpen). BYTE-EXACT from the door BP disassembly (2026-06-06): a player's
// E-press reaches the toggle only past an `Active` (power) check at ubergraph offset 3533, and
// the toggle's doorOpen needs `!jammed && !superClosed`. So a door opens on E iff
// `Active && !jammed && !superClosed`. These are all the door's OWN fields (NOT the keypad's --
// the old IsLocked read the passlock's isAcc, a crosshair-hover flag, and mis-locked powered
// doors). `Active` is driven by the gamemode power trigger (runTrigger 2/3) + the keypad's
// setActive (door.Active = keypad.Active) + the save.
int32_t g_activeOff      = -1;     // Adoor_C::Active (power) (Alpha 0.9.0-n: 0x0352)
int32_t g_superClosedOff = -1;     // Adoor_C::superClosed    (Alpha 0.9.0-n: 0x0378)
int32_t g_jammedOff      = -1;     // Adoor_C::jammed         (Alpha 0.9.0-n: 0x03B0)

// Documented Alpha 0.9.0-n fallbacks (CXXHeaderDump/door.hpp + triggerBase.hpp).
constexpr int32_t kKeyOffFallback         = 0x0260;
constexpr int32_t kIsOpenedOffFallback    = 0x0350;
constexpr int32_t kIsMovingOffFallback    = 0x0351;
constexpr int32_t kAutocloseOffFallback   = 0x0353;
constexpr int32_t kSensorOffFallback      = 0x0308;
constexpr int32_t kActiveOffFallback      = 0x0352;
constexpr int32_t kSuperClosedOffFallback = 0x0378;
constexpr int32_t kJammedOffFallback      = 0x03B0;

// Per-door cache so Restore*Autonomy can undo a suppression. Two distinct maps: the
// CLIENT suppresses every synced door (render-only); the HOST suppresses only the doors a
// remote client is currently holding open. A given process is one role, so the maps never
// alias, but keeping them separate makes the two lifecycles independent + auditable.
// Game-thread only in practice, but guarded for safety.
struct SavedAutonomy { bool autoclose; };
std::mutex g_autoMtx;
std::unordered_map<void*, SavedAutonomy> g_saved;      // CLIENT-side render-only suppression
std::unordered_map<void*, SavedAutonomy> g_hostHeld;   // HOST-side per-held-door suppression

// SmartApply verify list: a door we just tried to ANIMATE; if it hasn't reached the target by
// the deadline (the swing froze = beyond tick range / invisible) we force-snap it. GT-only,
// bounded (entries clear within ~700ms), so no growth. Drained by TickSmartApply.
struct VerifyEntry { bool target; std::chrono::steady_clock::time_point deadline; };
std::unordered_map<void*, VerifyEntry> g_verify;

// Toggle a door's sensor overlap events. false on the client KILLS checkSensor at its
// source (no BeginOverlap/EndOverlap -> no local auto open/close), so an applied host
// state cannot be reverted by local door logic. Resolves the UFunction lazily off the
// live sensor component's class (a UBoxComponent : UPrimitiveComponent). Best-effort:
// if the param name differs, a bool defaults to false (= the disable we want).
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

    // Key is declared on AtriggerBase_C; FindPropertyOffset does NOT climb to
    // super, so query the declaring class. isOpened is declared on door_C.
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
    // CanOpen gate fields -- the door's OWN power/jam/superClosed (door BP disassembly 2026-06-06).
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
    // While the door is mid-swing, report the DESTINATION (move__Direction, set
    // at swing-START: 0=Forward/open, 1=Backward/close) instead of isOpened (set
    // ~0.5 s later at swing-END). The host poll thus broadcasts an open/close the
    // instant it begins -- frame-symmetric with the client's input-edge request.
    // A settled door (isMoving=false) reads isOpened; move__Direction then holds
    // the last completed swing's value, which agrees, but isOpened is the
    // authoritative settled state so we prefer it. doorOpen/doorClose set isMoving
    // + the direction synchronously within the press dispatch, so the very next
    // poll tick (same/next frame) catches the intent.
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
    // BYTE-EXACT door BP gate (disassembly 2026-06-06, research/findings/votv-keypad-door-BP-
    // disassembly-2026-06-06.md): a player's E-press opens the door iff its OWN power is on
    // (`Active`, gate at ubergraph offset 3533) AND it is neither jammed nor superClosed (the
    // doorOpen open-condition). fail-OPEN on null / unresolved so a resolve failure never locks
    // every door. Reads three bools, no UFunction dispatch -- cheap, call per open-request.
    // Guard on g_resolved (acquire): once it is set, EnsureResolved has written all three
    // offsets (reflected or fallback -- never -1), so the reads below are valid. Before it is
    // set, fail-OPEN. (The caller OnRequest already gates on EnsureResolved; this is belt-and-
    // suspenders for any other call site.)
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

// Snap a door fully to a state, mesh AND flag, proximity-independent. Set the timeline alpha
// (move_a) to the end + the direction, then call move__UpdateFunc (which LERPS THE DOOR MESH
// from move_a -> the visual snap -- this was the missing piece: move__FinishedFunc only sets
// isOpened + stops the timeline, it does NOT move the mesh) then move__FinishedFunc (state).
static void ForceTo(void* door, bool open) {
    // IDEMPOTENT: if isOpened already matches the target, do nothing. move__FinishedFunc re-fires the
    // open/close sound + the doorOpened delegate, so a second force on an already-open door double-
    // sounds. Two callers can reach the same open door in one/adjacent tick (the keypad accept's
    // ForceOpen + the door's OWN native chain from the replayed inputNumber); this guard makes both
    // safe. (The door-channel SmartApply already checks cur!=target; this covers direct callers.)
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
    // If the door is ALREADY swinging toward this target, do NOTHING -- this is the opener's
    // own player_use animation (already played its sound) receiving the echo of its own request;
    // re-triggering it (or registering a verify that later force-snaps it) plays the open/close
    // sound a SECOND time. Let the in-progress swing finish on its own.
    const bool moving = *reinterpret_cast<bool*>(reinterpret_cast<char*>(door) + 0x0351);  // isMoving
    const uint8_t dir = *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(door) + kMoveDirOff);  // 0=Forward/open
    if (moving && ((open && dir == 0) || (!open && dir == 1))) return;  // already going the right way -> no re-trigger, no double sound
    // IDEMPOTENT: a SETTLED door already at the target is left alone. Re-running the BP
    // chain on a matching door is DESTRUCTIVE, not just wasteful: doorOpen's swing is
    // ADDITIVE (target = current pose + delta), so re-opening an already-open door drives
    // it PAST its frame into the wall -- the user-reported join-time clipping (every
    // connect-snapshot re-applied ON to saved-open doors, pushing them further each join,
    // 2026-06-12 round 2). The mid-swing case stays NON-idempotent on purpose: isOpened
    // lags the animation, so cur==target while moving the WRONG way means the authority
    // wants the swing reversed -- fall through and re-command it. (The old HostAuth
    // always-apply existed for the client's native-press echo race; the use-input PRE
    // observer's Active-gate killed that race at its source -- the client door no longer
    // moves from local presses at all.)
    if (!moving && g_isOpenedOff >= 0 &&
        *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(door) + g_isOpenedOff) == open)
        return;
    // Play the native animated swing (smooth wherever the door ticks -- any distance up to its
    // real tick range, no magic radius).
    if (open) CallDoorOpen(door, true); else CallDoorClose(door, true);
    // The swing FREEZES beyond tick range (far, invisible). Verify shortly: a near door reaches
    // the target before the deadline (removed, no snap); a far frozen door gets FORCE-SNAPPED so
    // its state stays correct. Deadline > the longest swing so a slow-but-completing animation
    // is never double-finished by the snap.
    g_verify[door] = VerifyEntry{ open, std::chrono::steady_clock::now() + std::chrono::milliseconds(1500) };
}

void TickSmartApply() {
    if (g_verify.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_verify.begin(); it != g_verify.end();) {
        void* door = it->first;
        if (!R::IsLive(door) || g_isOpenedOff < 0) { it = g_verify.erase(it); continue; }
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
    // Client door = render-only: autoclose=false + sensor overlaps off so local
    // checkSensor can't auto-revert an applied host state. player_use stays interactive
    // (optimistic local open is fine -- the host's authoritative DoorState lands right after).
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
    // While a remote client holds this door open, the host treats it render-only too: the
    // same proven recipe as the client (autoclose=false + sensor overlap events off) so the
    // host's own checkSensor can't autoclose the door it opened for the client. Lazy + once
    // per door (never per-tick/bulk -- the black-screen lesson).
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
    // The client released its hold: restore the authored autoclose + re-enable the sensor so
    // the host's native door autonomy resumes, then close the door honoring the real guards
    // (exactly one close edge -- the host poll broadcasts the resulting OFF).
    // Close with the SAME near/far visual as every other apply (SmartApply: animate the swing
    // if the local camera is near, snap if far) -- ForceClose here was an unconditional SNAP, so
    // opens animated but closes snapped = the "ugly snapping close" the user saw. Close BEFORE
    // restoring the sensor so the host's own proximity logic can't fight the close mid-swing.
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

// ue_wrap/door_box.cpp -- see ue_wrap/door_box.h.

#include "ue_wrap/devices/door_box.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <vector>

namespace ue_wrap::door_box {
namespace {

namespace R = reflection;

// Per-class resolution. The wrapper is operational when EITHER class resolved
// (the console may stream in later than the lockers or vice versa).
struct ClassDesc {
    void*   cls = nullptr;
    int32_t offOpened = -1;
    int32_t offAlpha = -1;      // the timeline track float (GUID-mangled name -> dump offset)
    int32_t offDirection = -1;  // TEnumAsByte<ETimelineDirection> right after alpha
    void*   updateFn = nullptr;     // a__UpdateFunc (rotates the axis from alpha)
    void*   finishedFn = nullptr;   // a__FinishedFunc (locker: close-slam + collision restore)
    int32_t offTimeline = -1;   // UTimelineComponent* A
    int32_t offTrigger = -1;    // locker only: AActor* triggerOnOpen @0x0280 (davyJones gate)
};

ClassDesc g_locker;   // Alocker_C (+ subclasses locker_personal_C / locker_death_C)
ClassDesc g_console;  // AdroneConsole_C
void* g_lockerOpenFn = nullptr;         // Alocker_C::Open(bool opened) -- the full native verb
void* g_consoleButtonsFn = nullptr;     // AdroneConsole_C::setButtonsCollision()
void* g_timelinePlayFn = nullptr;       // UTimelineComponent::Play
void* g_timelineReverseFn = nullptr;    // UTimelineComponent::Reverse
std::atomic<bool> g_anyResolved{false};

// Alpha 0.9.0-n fallbacks (CXXHeaderDump locker.hpp / droneConsole.hpp; the
// alpha/direction property NAMES are GUID-mangled per cook, so the dump offsets
// are the primary source for those two).
constexpr int32_t kLockerOpened    = 0x0270;
constexpr int32_t kLockerAlpha     = 0x0260;
constexpr int32_t kLockerDirection = 0x0264;
constexpr int32_t kLockerTimeline  = 0x0268;
constexpr int32_t kConsoleOpened    = 0x0298;
constexpr int32_t kConsoleAlpha     = 0x0288;
constexpr int32_t kConsoleDirection = 0x028C;
constexpr int32_t kConsoleTimeline  = 0x0290;

// Force-snap verify queue (the door.cpp lesson: the 0.5 s swing Timeline only
// advances while the actor TICKS -- far from the local player it freezes and
// the door leaf sticks mid-swing). Entries clear on completion or snap.
struct Verify {
    void* actor;
    int32_t idx;
    bool want;
    bool isLocker;
    std::chrono::steady_clock::time_point deadline;
};
std::vector<Verify> g_verify;  // GT-only

bool ResolveClass(ClassDesc& d, const wchar_t* clsName,
                  int32_t fbOpened, int32_t fbAlpha, int32_t fbDir, int32_t fbTimeline) {
    if (d.cls) return true;
    void* cls = R::FindClass(clsName);
    if (!cls) return false;
    d.cls = cls;
    d.offOpened = R::FindPropertyOffset(cls, L"opened");
    if (d.offOpened < 0) d.offOpened = fbOpened;
    d.offAlpha = fbAlpha;          // GUID-mangled name -- dump offset is authoritative
    d.offDirection = fbDir;
    d.offTimeline = fbTimeline;
    d.updateFn = R::FindFunction(cls, L"a__UpdateFunc");
    d.finishedFn = R::FindFunction(cls, L"a__FinishedFunc");
    d.offTrigger = R::FindPropertyOffset(cls, L"triggerOnOpen");  // locker only; -1 on the console
    return true;
}

inline bool ReadBool(const void* obj, int32_t off) {
    return *reinterpret_cast<const bool*>(reinterpret_cast<const char*>(obj) + off);
}

// Which family is `obj`? (nullptr ClassDesc when neither.)
const ClassDesc* DescOf(void* obj) {
    if (!obj) return nullptr;
    void* cls = R::ClassOf(obj);
    if (!cls) return nullptr;
    if (g_locker.cls) {
        void* bases[1] = { g_locker.cls };
        if (R::IsDescendantOfAny(cls, bases, 1)) return &g_locker;
    }
    if (g_console.cls) {
        void* bases[1] = { g_console.cls };
        if (R::IsDescendantOfAny(cls, bases, 1)) return &g_console;
    }
    return nullptr;
}

// Snap the swing to its end state: write the timeline track float + direction,
// then dispatch a__UpdateFunc (rotates the axis -- the VISUAL move; FinishedFunc
// alone does not move the mesh, the door.cpp lesson) and a__FinishedFunc (on a
// locker close it plays the slam + restores the door collision -- required).
void ForceSnap(void* actor, const ClassDesc& d, bool want) {
    *reinterpret_cast<float*>(reinterpret_cast<char*>(actor) + d.offAlpha) = want ? 1.0f : 0.0f;
    *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(actor) + d.offDirection) =
        want ? 0u : 1u;  // ETimelineDirection: 0=Forward, 1=Backward
    if (d.updateFn) { ParamFrame f(d.updateFn); if (f.valid()) Call(actor, f); }
    if (d.finishedFn) { ParamFrame f(d.finishedFn); if (f.valid()) Call(actor, f); }
}

}  // namespace

bool EnsureResolved() {
    if (g_anyResolved.load(std::memory_order_acquire)) {
        // Keep trying the OTHER class lazily (cheap: FindClass early-outs once
        // both are cached in their descs).
        ResolveClass(g_locker, L"locker_C", kLockerOpened, kLockerAlpha,
                     kLockerDirection, kLockerTimeline);
        ResolveClass(g_console, L"droneConsole_C", kConsoleOpened, kConsoleAlpha,
                     kConsoleDirection, kConsoleTimeline);
        return true;
    }
    const bool l = ResolveClass(g_locker, L"locker_C", kLockerOpened, kLockerAlpha,
                                kLockerDirection, kLockerTimeline);
    const bool c = ResolveClass(g_console, L"droneConsole_C", kConsoleOpened, kConsoleAlpha,
                                kConsoleDirection, kConsoleTimeline);
    if (!l && !c) return false;

    if (l && !g_lockerOpenFn) {
        // FName case flips with load order (cooked export 'open' vs live 'Open');
        // NameEquals is case-sensitive -- try both, log which hit.
        g_lockerOpenFn = R::FindFunction(g_locker.cls, L"Open");
        if (!g_lockerOpenFn) g_lockerOpenFn = R::FindFunction(g_locker.cls, L"open");
        if (!g_lockerOpenFn)
            UE_LOGW("door_box: locker Open verb unresolved -- locker apply degraded to snap-only");
    }
    if (c && !g_consoleButtonsFn)
        g_consoleButtonsFn = R::FindFunction(g_console.cls, L"setButtonsCollision");
    if (!g_timelinePlayFn) {
        if (void* tc = R::FindClass(L"TimelineComponent")) {
            g_timelinePlayFn    = R::FindFunction(tc, L"Play");
            g_timelineReverseFn = R::FindFunction(tc, L"Reverse");
        }
    }
    g_anyResolved.store(true, std::memory_order_release);
    UE_LOGI("door_box: resolved locker=%p (opened@0x%04X open=%p upd=%p fin=%p) "
            "console=%p (opened@0x%04X buttons=%p) timeline Play=%p Reverse=%p",
            g_locker.cls, g_locker.offOpened, g_lockerOpenFn, g_locker.updateFn,
            g_locker.finishedFn, g_console.cls, g_console.offOpened,
            g_consoleButtonsFn, g_timelinePlayFn, g_timelineReverseFn);
    return true;
}

bool IsDoorBox(void* obj) { return DescOf(obj) != nullptr; }

std::wstring GetNameKey(void* actor) {
    if (!actor) return std::wstring();
    return R::ToString(R::NameOf(actor));
}

bool TryReadOpened(void* actor, bool& out) {
    const ClassDesc* d = DescOf(actor);
    if (!d || d->offOpened < 0) return false;
    out = ReadBool(actor, d->offOpened);
    return true;
}

bool ApplyOpened(void* actor, bool want) {
    const ClassDesc* d = DescOf(actor);
    if (!d) return false;
    const bool isLocker = (d == &g_locker);
    // Trigger-wired locker (locker_davyJones: triggerOnOpen -> a batchSpawner):
    // the native Open verb FIRES the trigger -- correct on the OPENER (SP
    // behavior, runs once), wrong on a MIRROR apply (the spawner would run a
    // second time on this peer = the double-spawn). Targeted per-site fix
    // (principle 4): a trigger-wired locker mirrors via the SNAP path (opened
    // write + ForceSnap swing) -- door state mirrors, the trigger fires only
    // where the player actually opened it. The other 18 lockers carry no
    // trigger (uexp census) and keep the full native verb.
    const bool triggerWired =
        isLocker && d->offTrigger >= 0 &&
        *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(actor) + d->offTrigger) != nullptr;
    if (isLocker && g_lockerOpenFn && !triggerWired) {
        // The full native verb: writes opened, plays the sound, swings the
        // Timeline, sets door collision.
        ParamFrame f(g_lockerOpenFn);
        if (!f.valid()) return false;
        f.Set<bool>(L"opened", want);
        if (!Call(actor, f)) return false;
    } else if (triggerWired) {
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(actor) + d->offOpened) = want;
        ForceSnap(actor, *d, want);
        UE_LOGI("door_box: trigger-wired locker mirrored via snap (want=%d) -- "
                "triggerOnOpen left to the opening peer", want ? 1 : 0);
        return true;  // snapped synchronously -- no verify entry needed
    } else {
        // Console (no public verb) -- the garage write+refresh precedent:
        // opened := want, refresh the button/blinklight collision, drive the
        // swing natively via the Timeline component.
        *reinterpret_cast<bool*>(reinterpret_cast<char*>(actor) + d->offOpened) = want;
        if (d == &g_console && g_consoleButtonsFn) {
            ParamFrame f(g_consoleButtonsFn);
            if (f.valid()) Call(actor, f);
        }
        void* tl = *reinterpret_cast<void* const*>(
            reinterpret_cast<const char*>(actor) + d->offTimeline);
        void* fn = want ? g_timelinePlayFn : g_timelineReverseFn;
        if (tl && R::IsLive(tl) && fn) {
            ParamFrame f(fn);
            if (f.valid()) Call(tl, f);
        }
    }
    // Verify + force-snap for the far-frozen-timeline case.
    g_verify.push_back(Verify{ actor, R::InternalIndexOf(actor), want, isLocker,
                               std::chrono::steady_clock::now() + std::chrono::milliseconds(1500) });
    return true;
}

void OnDisconnect() {
    // Audit IMPORTANT-2 (2026-06-12): a swing mid-verify at session teardown must
    // not survive into the next session/SP world -- a stale entry's ForceSnap
    // (a__UpdateFunc/a__FinishedFunc) would fire on a world state it wasn't
    // queued for.
    g_verify.clear();
}

void TickVerify() {
    if (g_verify.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_verify.begin(); it != g_verify.end();) {
        if (!R::IsLiveByIndex(it->actor, it->idx)) { it = g_verify.erase(it); continue; }
        const ClassDesc& d = it->isLocker ? g_locker : g_console;
        const float alpha = *reinterpret_cast<const float*>(
            reinterpret_cast<const char*>(it->actor) + d.offAlpha);
        const bool done = it->want ? (alpha >= 0.99f) : (alpha <= 0.01f);
        if (done) { it = g_verify.erase(it); continue; }
        if (now >= it->deadline) {
            ForceSnap(it->actor, d, it->want);
            UE_LOGI("door_box: force-snapped frozen swing (want=%d)", it->want ? 1 : 0);
            it = g_verify.erase(it);
            continue;
        }
        ++it;
    }
}

}  // namespace ue_wrap::door_box

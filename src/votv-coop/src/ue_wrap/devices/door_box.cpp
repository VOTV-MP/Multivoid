// ue_wrap/devices/door_box.cpp -- see ue_wrap/devices/door_box.h.

#include "ue_wrap/devices/door_box.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"

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
    void*   failedCls = nullptr;  // a class whose `opened` did not resolve: said once, not asked again
    int32_t offOpened = -1;
    int32_t offAlpha = -1;      // the swing timeline's value, a_a_<guid>, by its prefix
    int32_t offDirection = -1;  // its TEnumAsByte<ETimelineDirection>, a__Direction_<guid>
    void*   updateFn = nullptr;     // a__UpdateFunc (rotates the axis from alpha)
    void*   finishedFn = nullptr;   // a__FinishedFunc (locker: close-slam + collision restore)
    void*   verbFn = nullptr;       // the family's own apply verb, resolved WITH the class
    int32_t offTimeline = -1;   // UTimelineComponent* A
    int32_t offTrigger = -1;    // locker only: AActor* triggerOnOpen (the davyJones gate)
};

ClassDesc g_locker;   // Alocker_C (+ subclasses locker_personal_C / locker_death_C); verb = Open(bool)
ClassDesc g_console;  // AdroneConsole_C; verb = setButtonsCollision()
void* g_timelinePlayFn = nullptr;       // UTimelineComponent::Play
void* g_timelineReverseFn = nullptr;    // UTimelineComponent::Reverse
bool  g_timelineFailed = false;         // the engine class held no Play or Reverse: said once

// Force-snap verify queue: the 0.5 s swing Timeline only advances while the actor TICKS, so far
// from the local player it freezes and the door leaf sticks mid-swing (door.cpp drives the same
// swing and carries the same note). Entries clear on completion or snap.
struct Verify {
    void* actor;
    int32_t idx;
    bool want;
    bool isLocker;
    std::chrono::steady_clock::time_point deadline;
};
std::vector<Verify> g_verify;  // GT-only

// Everything a family needs is resolved on the ONE tick its class first appears, verb included:
// a verb resolved later, under a latch that another family already closed, is a verb never
// resolved at all -- whichever class streams in second used to lose its own. Every offset is read
// by name, the swing timeline's GUID-suffixed ones by their prefix; a family whose `opened` does not
// resolve stays out, said once, and one whose swing does not runs without the snap.
bool ResolveClass(ClassDesc& d, const wchar_t* clsName, const wchar_t* verbName) {
    if (d.cls) return true;
    void* cls = ue_wrap::object_index::ClassByName(clsName);
    if (!cls || cls == d.failedCls) return false;
    const int32_t opened = R::FindPropertyOffset(cls, L"opened");
    if (opened < 0) {
        d.failedCls = cls;
        UE_LOGE("door_box: %ls.opened did not resolve by name -- that family stays out of the sync", clsName);
        return false;
    }
    d.cls = cls;
    d.offOpened = opened;
    d.offAlpha = R::FindPropertyOffsetByPrefix(cls, L"a_a_");
    d.offDirection = R::FindPropertyOffsetByPrefix(cls, L"a__Direction_");
    d.offTimeline = R::FindPropertyOffset(cls, L"a");
    d.verbFn = R::FindFunction(cls, verbName);
    d.updateFn = R::FindFunction(cls, L"a__UpdateFunc");
    d.finishedFn = R::FindFunction(cls, L"a__FinishedFunc");
    d.offTrigger = R::FindPropertyOffset(cls, L"triggerOnOpen");  // locker only; -1 on the console
    if (!d.verbFn)
        UE_LOGW("door_box: %ls::%ls unresolved -- that family's apply degrades to snap-only",
                clsName, verbName);
    if (d.offAlpha < 0 || d.offDirection < 0 || d.offTimeline < 0)
        UE_LOGW("door_box: %ls's swing timeline did not resolve (value %d, direction %d, component %d) -- no "
                "snap for that family", clsName, d.offAlpha, d.offDirection, d.offTimeline);
    UE_LOGI("door_box: resolved %ls=%p opened@0x%04X swing@%d/%d/%d verb=%p upd=%p fin=%p", clsName, cls,
            opened, d.offAlpha, d.offDirection, d.offTimeline, d.verbFn, d.updateFn, d.finishedFn);
    return true;
}

// Whether the swing of `d`'s class can be read and snapped.
bool SwingReadable(const ClassDesc& d) { return d.offAlpha >= 0 && d.offDirection >= 0; }

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

// Snap the swing to its end state: write the timeline track float + direction, then dispatch
// a__UpdateFunc, which rotates the axis and is the VISUAL move -- FinishedFunc alone does not move
// the mesh -- and a__FinishedFunc, which on a locker close plays the slam and restores the door
// collision, so it is required.
void ForceSnap(void* actor, const ClassDesc& d, bool want) {
    if (!SwingReadable(d)) return;
    *reinterpret_cast<float*>(reinterpret_cast<char*>(actor) + d.offAlpha) = want ? 1.0f : 0.0f;
    *reinterpret_cast<uint8_t*>(reinterpret_cast<char*>(actor) + d.offDirection) =
        want ? 0u : 1u;  // ETimelineDirection: 0=Forward, 1=Backward
    if (d.updateFn) { ParamFrame f(d.updateFn); if (f.valid()) Call(actor, f); }
    if (d.finishedFn) { ParamFrame f(d.finishedFn); if (f.valid()) Call(actor, f); }
}

// The timeline verbs belong to no family, so they are resolved on their own rather than with
// whichever class latched first. The engine's class is always loaded; a verb it does not hold is
// said once and not asked for again.
void ResolveTimelineVerbs() {
    if (g_timelinePlayFn || g_timelineFailed) return;
    void* tc = ue_wrap::object_index::ClassByName(L"TimelineComponent");
    if (!tc) return;
    g_timelinePlayFn    = R::FindFunction(tc, L"Play");
    g_timelineReverseFn = R::FindFunction(tc, L"Reverse");
    if (!g_timelinePlayFn || !g_timelineReverseFn) {
        g_timelinePlayFn = g_timelineReverseFn = nullptr;
        g_timelineFailed = true;
        UE_LOGW("door_box: UTimelineComponent Play/Reverse unresolved -- swings snap instead of animating");
    }
}

}  // namespace

bool EnsureResolved() {
    // Both families are tried until each has its class: whichever streams in second resolves its
    // own verb on its own edge, which a latch shared with the first would have denied it forever.
    // A class held costs one pointer test, and one not loaded one object-index lookup, which costs
    // nothing on a miss.
    ResolveClass(g_locker, L"locker_C", L"Open");
    ResolveClass(g_console, L"droneConsole_C", L"setButtonsCollision");
    ResolveTimelineVerbs();
    return g_locker.cls || g_console.cls;
}

bool IsDoorBox(void* obj) { return DescOf(obj) != nullptr; }
bool IsLocker(void* obj) { return obj && g_locker.cls && DescOf(obj) == &g_locker; }
bool IsDroneConsole(void* obj) { return obj && g_console.cls && DescOf(obj) == &g_console; }

bool CallAction(void* actor, void* player, uint8_t action) {
    void* cls = actor ? R::ClassOf(actor) : nullptr;
    void* fn = cls ? R::FindDispatchFunctionCached(cls, L"actionOptionIndex") : nullptr;
    if (!fn) return false;
    ParamFrame f(fn);
    if (!f.valid() || !f.Set<void*>(L"player", player) || !f.Set<uint8_t>(L"action", action)) return false;
    return Call(actor, f);
}

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
    // Trigger-wired locker (locker_davyJones: triggerOnOpen -> a batchSpawner): the native Open
    // verb FIRES the trigger -- correct on the OPENER, where it runs once, and wrong on a MIRROR
    // apply, where the spawner would run a second time on this peer and double-spawn. Targeted
    // per-site fix (principle 4): a trigger-wired locker mirrors via the SNAP path (opened write +
    // ForceSnap swing), so door state mirrors and the trigger fires only where the player actually
    // opened it. The test is per instance, on this locker's own triggerOnOpen: a locker whose
    // trigger is null keeps the full native verb.
    const bool triggerWired =
        isLocker && d->offTrigger >= 0 &&
        *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(actor) + d->offTrigger) != nullptr;
    if (isLocker && g_locker.verbFn && !triggerWired) {
        // The full native verb: writes opened, plays the sound, swings the
        // Timeline, sets door collision.
        ParamFrame f(g_locker.verbFn);
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
        if (d == &g_console && g_console.verbFn) {
            ParamFrame f(g_console.verbFn);
            if (f.valid()) Call(actor, f);
        }
        void* tl = d->offTimeline < 0 ? nullptr : *reinterpret_cast<void* const*>(
            reinterpret_cast<const char*>(actor) + d->offTimeline);
        void* fn = want ? g_timelinePlayFn : g_timelineReverseFn;
        if (tl && R::IsLive(tl) && fn) {
            ParamFrame f(fn);
            if (f.valid()) Call(tl, f);
        }
    }
    // Verify + force-snap for the far-frozen-timeline case, where the swing can be read.
    if (SwingReadable(*d))
        g_verify.push_back(Verify{ actor, R::InternalIndexOf(actor), want, isLocker,
                                   std::chrono::steady_clock::now() + std::chrono::milliseconds(1500) });
    return true;
}

void OnDisconnect() {
    // A swing mid-verify at session teardown must not survive into the next session or an SP world:
    // a stale entry's ForceSnap (a__UpdateFunc / a__FinishedFunc) would fire on a world state it
    // was not queued for.
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

// coop/dev/badsun_drill_lane.cpp -- the Bad Sun drill's six lane arms: the host's existence of a Bad Sun against
// every peer's own copies (coop/world/weather_badsun). See badsun_drill_shared.h for what each arm sets up and
// judges. absent_stale's host resumes after its session ends at the rejoin, on the copy it made. Every arm is
// readiness-armed on the join being over and every wait ends on a state this peer can read; the
// bounds below only end a phase that did not come. The lane's own counters are the receiver evidence.

#include "badsun_drill_shared.h"  // co-located private header (src tree, not include/)

#include "coop/dev/rollover_watch.h"
#include "coop/dev/set_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/save/save_transfer.h"
#include "coop/world/time_sync.h"
#include "coop/world/weather_badsun.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/world/bad_sun.h"
#include "ue_wrap/world/daynightcycle.h"
#include "ue_wrap/world/game_mode.h"
#include "ue_wrap/world/skysphere.h"
#include "ue_wrap/world/world_singleton.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace coop::dev::badsun_drill::detail {
namespace {

namespace BS  = ue_wrap::bad_sun;
namespace DNC = ue_wrap::daynightcycle;
namespace GM  = ue_wrap::game_mode;
namespace WB  = coop::weather_badsun;

// One road per arm and role; Setup is the first step after the join, where the world is read and the arm's act done.
enum class P {
    Begin, WaitJoin, Setup, Done,
    // The host.
    HostEdge,    // midnight: the day number moves
    HostRoll,     // midnight: the in-graph Spawn Bad Sun is seen and the copy is held
    HostActive,   // join: its own copy goes active
    HostAlpha,    // end: its widget's ramp reaches 1.0 and stays there
    HostRemove,   // end: its own remove() runs and the lane sends absent
    HostCopy,     // absent_stale: its one inactive copy exists
    HostRejoin,   // absent_stale: a joiner's world is taken and not yet ready
    HostReady,    // absent_stale: that joiner's world-ready
    // A client.
    CopyAppear,   // spawn: the host's copy arrives
    CopyActive,   // spawn: and goes active
    ClockWait,    // cheat, join: the host's clock reads 08:00 here
    JoinActive,   // join: the copy goes active
    CallWatch,    // cheat: its own Spawn Bad Sun, refused
    BaseWait,     // midnight: the first sample before the edge
    EdgeWait,     // midnight: the host's day number moves
    EdgeWatch,    // midnight: the copy born at the edge
    ApplyWait,    // join, absent_stale: the seed's apply
    LiveWait,     // absent_stale: the life-1 live edge
    ZeroWatch,    // absent_stale: no copy once the seed applied absent
    AbsentWait,   // end: the host's absent
    OwnRemove,    // end: this copy's own remove() and the sky it resets
};

constexpr float kAt0200 = (2 * 60) / 1440.f;
constexpr float kAt0800 = (8 * 60) / 1440.f;
constexpr float kAt1859 = (18 * 60 + 59) / 1440.f;
constexpr float kAt2358 = (23 * 60 + 58) / 1440.f;
constexpr int   kBadSunMode = 7;       // the midnight roll's "always" game mode
// The end arm's gate: the widget's ramp clamped at 1.0 and held there for kAlphaHold. Both peers' ramps then fade for
// the same 60 s, so the host's remove() runs first by the clock lag and the client's copy is still fading when the
// host's absent lands. The hold is a margin for the two copies' own 5 s activation poll plus the clock lag, not a
// readiness clock: at 6 s the rig's client copy was still ramping and ended 7 s before the host's.
constexpr float kAlphaGate = 1.0f;
constexpr float kSkyTolerance = 0.001f;
constexpr int   kMaxDrainFails = 10;
constexpr auto  kSample = std::chrono::seconds(1);
constexpr auto  kJoinBound = std::chrono::seconds(300);   // from the first tick to the join being over
constexpr auto  kSetupBound = std::chrono::seconds(60);   // from the join being over to the world reading
constexpr auto  kAppearBound = std::chrono::seconds(15);  // spawn: the copy arrives
constexpr auto  kActiveBound = std::chrono::seconds(15);  // spawn, join: it goes active
constexpr auto  kEdgeBound = std::chrono::seconds(180);   // midnight: the clock reaches the day edge
constexpr auto  kEdgeCopyBound = std::chrono::seconds(10);  // midnight: the copy exists after the edge
constexpr auto  kCallBound = std::chrono::seconds(10);    // cheat: after the client's own call
constexpr auto  kClockBound = std::chrono::seconds(60);   // the host's clock reaches 08:00 on the client
constexpr auto  kApplyBound = std::chrono::seconds(60);   // the seed's apply lands
constexpr auto  kAlphaHold = std::chrono::seconds(15);    // end: the ramp has been full this long
constexpr auto  kAlphaBound = std::chrono::seconds(120);  // end: the ramp reaches the gate and holds
constexpr auto  kRemoveBound = std::chrono::seconds(150); // end: the host's fade and remove()
constexpr auto  kAbsentBound = std::chrono::seconds(400); // end: the client waits for the host's whole run
constexpr auto  kOwnRemoveBound = std::chrono::seconds(70);  // end: the fade (60 s) plus 10 s
constexpr auto  kRejoinBound = std::chrono::seconds(400); // absent_stale: the host waits for the rejoin
constexpr auto  kLiveBound = std::chrono::seconds(30);    // absent_stale: a live edge or the blob's copy
constexpr auto  kZeroBound = std::chrono::seconds(10);    // absent_stale: no copy after the seed
constexpr auto  kReadyBound = std::chrono::seconds(120);  // absent_stale: the joiner's world-ready

struct Snap {
    int n = -1;
    int active = 0, inactive = 0;  // by both tests set / neither set; a copy with one test set is neither
    void* first = nullptr;
};

P         g_phase = P::Begin;
int       g_slot = -1;
int       g_rjSlot = -1;
int       g_drainFails = 0;
int       g_hour = -1, g_min = -1;
char      g_desc[120] = "not read";   // the last count and each copy's state, for a client's verdict
Steady::time_point g_deadline{}, g_nextSample{}, g_tMark{};
// The midnight arm: the mode put back, the day it started on, the watch's count before.
bool      g_modeChanged = false;
int       g_oldMode = -1;
int32_t   g_dayZ = -1;
uint64_t  g_ranBefore = 0;
WB::Counts g_c0{};                    // the lane's counts at an arm's baseline
uint64_t  g_refused0 = 0;

const char* ArmName() {
    switch (ArmOf()) {
    case Arm::Midnight:    return "midnight";
    case Arm::Spawn:       return "spawn";
    case Arm::Cheat:       return "cheat";
    case Arm::Join:        return "join";
    case Arm::End:         return "end";
    case Arm::AbsentStale: return "absent_stale";
    default:               return "off";
    }
}

void Emit(bool host, const char* word, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    VerdictV(ArmName(), host, word, fmt, ap);
    va_end(ap);
}

void RestoreMode() {
    if (!g_modeChanged) return;
    g_modeChanged = false;
    void* gi = ue_wrap::world_singleton::GameInstance();
    if (gi) GM::WriteTo(gi, g_oldMode);
    UE_LOGI("badsun_drill: [host] the game mode put back to %d", g_oldMode);
}

// The one verdict line, and the arm is over. A client's names its live count and each copy's state.
void Finish(bool host, const char* word, const char* fmt, va_list ap) {
    char reason[200];
    std::vsnprintf(reason, sizeof(reason), fmt, ap);
    if (host) Emit(host, word, "%s", reason);
    else      Emit(host, word, "%s | live %s", reason, g_desc);
    RestoreMode();
    g_phase = P::Done;
}

void Pass(bool host, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Finish(host, "PASS", fmt, ap);
    va_end(ap);
}

void Fail(bool host, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Finish(host, "FAIL", fmt, ap);
    va_end(ap);
}

void Inconclusive(bool host, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Finish(host, "INCONCLUSIVE", fmt, ap);
    va_end(ap);
}

void Enter(P phase, std::chrono::seconds bound) {
    g_phase = phase;
    g_deadline = Steady::now() + bound;
    g_nextSample = {};
}

bool Due() {
    const auto now = Steady::now();
    if (now < g_nextSample) return false;
    g_nextSample = now + kSample;
    return true;
}

bool ReadClock() {
    int32_t h = 0, m = 0, d = 0;
    if (!DNC::ReadTimeZ(h, m, d)) return false;
    g_hour = h;
    g_min = m;
    return true;
}

const char* StateWord(bool widget, bool sender) { return widget && sender ? "active" : !widget && !sender ? "inactive" : "half"; }

// The cheat gate for a log line: an unread gate is not "0".
const char* CheatsWord(bool read, bool allowed) { return !read ? "unread" : allowed ? "1" : "0"; }

// The live copies and each one's two tests. False while the index cannot drain or a test does not read; a run of
// either failure ends the arm. A caller that sees the phase already Done has had its verdict from here.
bool Take(bool host, Snap& out) {
    void* copies[BS::kMaxLive] = {};
    const int n = BS::CountLive(copies, BS::kMaxLive);
    if (n < 0) {
        if (++g_drainFails >= kMaxDrainFails) Fail(host, "the index could not drain (%d tries in a row)", g_drainFails);
        return false;
    }
    Snap s;
    s.n = n;
    s.first = copies[0];
    char desc[sizeof(g_desc)];
    int len = std::snprintf(desc, sizeof(desc), "%d", n);
    for (int i = 0; i < n && i < BS::kMaxLive; ++i) {
        bool widget = false, sender = false;
        if (!BS::ReadActive(copies[i], widget) || !BS::ReadIsSender(copies[i], sender)) {
            if (++g_drainFails >= kMaxDrainFails)
                Fail(host, "a copy's two tests did not read (%d tries in a row)", g_drainFails);
            return false;
        }
        if (widget && sender) ++s.active;
        else if (!widget && !sender) ++s.inactive;
        if (len > 0 && len < static_cast<int>(sizeof(desc)))
            len += std::snprintf(desc + len, sizeof(desc) - len, "%s%s", i == 0 ? ": " : ", ", StateWord(widget, sender));
    }
    std::snprintf(g_desc, sizeof(g_desc), "%s", desc);
    g_drainFails = 0;
    out = s;
    return true;
}

uint64_t RollRan() { return coop::dev::rollover_watch::RanCount(L"Spawn Bad Sun"); }

// absent_stale, the HOST: set once this process's own Spawn Bad Sun made the arm's copy. LaneReset keeps it: the host's
// session ends when the last client leaves for the rejoin, and only a copy this process made may be resumed on.
bool g_staleSpawned = false;

// ---- the host's setup, after the join ------------------------------------------------------------------------

void HostSetup() {
    if (!Due()) return;
    Snap sn;
    if (!Take(true, sn)) return;
    const Arm a = ArmOf();
    const bool rigSave = a == Arm::Join || a == Arm::End;
    if (rigSave && sn.n != 1)
        return Inconclusive(true, "the rig save holds %d Bad Sun copies at the join's end, expected 1", sn.n);
    if (!rigSave && sn.n != 0)
        return Inconclusive(true, "the fresh world holds %d Bad Sun copies at the start, expected 0", sn.n);
    switch (a) {
    case Arm::Midnight: {
        if (!coop::dev::rollover_watch::IsEnabled())
            return Inconclusive(true, "rollover_watch is off on this host: the in-graph Spawn Bad Sun would go unseen");
        void* gi = ue_wrap::world_singleton::GameInstance();
        const int old = gi ? GM::ReadFrom(gi) : -1;
        int32_t h = 0, m = 0, z = 0;
        if (old < 0 || !DNC::ReadSavedTime(h, m, z)) return;  // not readable yet: the next tick asks again
        GM::WriteTo(gi, kBadSunMode);
        g_modeChanged = true;
        g_oldMode = old;
        g_dayZ = z;
        g_ranBefore = RollRan();
        if (!coop::dev::set_clock::ApplyTimeFraction(kAt2358)) return Fail(true, "the clock set to 23:58 was refused");
        UE_LOGI("badsun_drill: [host] slot %d's join is over; game mode %d -> %d, clock to 23:58 of day %d, waiting for "
                "the day edge", g_slot, old, kBadSunMode, z);
        Enter(P::HostEdge, kEdgeBound);
        return;
    }
    case Arm::Spawn:
        if (!coop::dev::set_clock::ApplyTimeFraction(kAt0800)) return Fail(true, "the clock set to 08:00 was refused");
        if (!BS::CallSpawn()) return Fail(true, "the gamemode's Spawn Bad Sun did not run");
        UE_LOGI("badsun_drill: [host] slot %d's join is over; clock to 08:00 and Spawn Bad Sun called; the verdict is the "
                "client's", g_slot);
        g_phase = P::Done;
        return;
    case Arm::Cheat:
        if (!coop::dev::set_clock::ApplyTimeFraction(kAt0800)) return Fail(true, "the clock set to 08:00 was refused");
        // A client's own Spawn Bad Sun makes a local actor that never reaches this host, so a host count could not go
        // red: the verdict is the client's.
        UE_LOGI("badsun_drill: [host] slot %d's join is over; clock to 08:00; the verdict is the client's", g_slot);
        g_phase = P::Done;
        return;
    case Arm::Join:
        if (!coop::dev::set_clock::ApplyTimeFraction(kAt0800)) return Fail(true, "the clock set to 08:00 was refused");
        UE_LOGI("badsun_drill: [host] slot %d's join is over; clock to 08:00, waiting for this host's copy to go active; "
                "the verdict is the joiner's", g_slot);
        Enter(P::HostActive, kActiveBound);
        return;
    case Arm::End:
        if (!coop::dev::set_clock::ApplyTimeFraction(kAt0800)) return Fail(true, "the clock set to 08:00 was refused");
        UE_LOGI("badsun_drill: [host] slot %d's join is over; clock to 08:00, waiting for the widget's alpha to read %.1f "
                "for %d s", g_slot, kAlphaGate, static_cast<int>(kAlphaHold.count()));
        g_tMark = {};
        Enter(P::HostAlpha, kAlphaBound);
        return;
    case Arm::AbsentStale:
        if (!coop::dev::set_clock::ApplyTimeFraction(kAt0200)) return Fail(true, "the clock set to 02:00 was refused");
        if (!BS::CallSpawn()) return Fail(true, "the gamemode's Spawn Bad Sun did not run");
        g_staleSpawned = true;
        UE_LOGI("badsun_drill: [host] slot %d's join is over; clock to 02:00 and Spawn Bad Sun called; the copy stays "
                "inactive outside 07:00..18:00", g_slot);
        Enter(P::HostCopy, kSetupBound);
        return;
    default:
        return;
    }
}

// ---- a client's setup, after the join ------------------------------------------------------------------------

void ClientSetup() {
    switch (ArmOf()) {
    case Arm::Spawn:       Enter(P::CopyAppear, kAppearBound); return;
    case Arm::Midnight:    Enter(P::BaseWait, kClockBound); return;
    case Arm::Cheat:       Enter(P::ClockWait, kClockBound); return;
    case Arm::Join:        Enter(P::ApplyWait, kApplyBound); return;
    case Arm::AbsentStale: Enter(P::ApplyWait, kApplyBound); return;
    case Arm::End:
        g_c0 = WB::GetCounts();
        UE_LOGI("badsun_drill: [client] joined; waiting for the host's absent while this copy is active");
        Enter(P::AbsentWait, kAbsentBound);
        return;
    default:
        return;
    }
}

// ---- the host's phases -----------------------------------------------------------------------------------------

void HostEdge() {
    int32_t h = 0, m = 0, z = 0;
    if (!DNC::ReadSavedTime(h, m, z) || z == g_dayZ) return;
    UE_LOGI("badsun_drill: [host] the day number moved %d -> %d; waiting for the roll's Spawn Bad Sun", g_dayZ, z);
    Enter(P::HostRoll, kEdgeCopyBound);
}

void HostRoll() {
    if (!Due()) return;
    const uint64_t ran = RollRan() - g_ranBefore;
    bool present = false;
    if (ran == 0 || !BS::ReadGamemodeCopy(nullptr, present) || !present) return;
    Pass(true, "the day edge ran Spawn Bad Sun in the day cycle's graph (the watch counted %llu) and gamemode.badSun is "
         "present", static_cast<unsigned long long>(ran));
}

void HostActive() {
    if (!Due()) return;
    Snap sn;
    if (!Take(true, sn)) return;
    if (sn.n != 1) return Fail(true, "%d live copies on this host, expected 1", sn.n);
    if (sn.active == 1) {
        UE_LOGI("badsun_drill: [host] its copy is active by both tests");
        g_phase = P::Done;
    }
}

void HostAlpha() {
    if (!Due()) return;
    Snap sn;
    if (!Take(true, sn)) return;
    if (sn.n != 1) return Fail(true, "%d live copies on this host, expected 1", sn.n);
    float alpha = 0.f;
    if (!BS::ReadWidgetAlpha(sn.first, alpha) || alpha < kAlphaGate) {
        g_tMark = {};
        return;
    }
    if (g_tMark == Steady::time_point{}) g_tMark = Steady::now();
    if (Steady::now() - g_tMark < kAlphaHold) return;
    g_c0 = WB::GetCounts();
    if (!coop::dev::set_clock::ApplyTimeFraction(kAt1859)) return Fail(true, "the clock set to 18:59 was refused");
    UE_LOGI("badsun_drill: [host] the widget's alpha has read %.2f for %d s; clock to 18:59, waiting for its own "
            "remove() and the lane's absent", alpha, static_cast<int>(kAlphaHold.count()));
    Enter(P::HostRemove, kRemoveBound);
}

void HostRemove() {
    if (!Due()) return;
    const WB::Counts c = WB::GetCounts();
    Snap sn;
    if (!Take(true, sn) || sn.n != 0 || c.removesRan <= g_c0.removesRan || c.sentAbsent <= g_c0.sentAbsent) return;
    Pass(true, "its own remove() ran (%llu) and the lane sent absent (%llu); no copy is left",
         static_cast<unsigned long long>(c.removesRan - g_c0.removesRan),
         static_cast<unsigned long long>(c.sentAbsent - g_c0.sentAbsent));
}

void HostCopy() {
    if (!Due()) return;
    Snap sn;
    if (!Take(true, sn)) return;
    if (sn.n > 1) return Fail(true, "%d live copies on this host, expected 1", sn.n);
    if (sn.n == 1 && sn.inactive == 1) {
        UE_LOGI("badsun_drill: [host] one inactive copy; waiting for a joiner's world to be taken");
        Enter(P::HostRejoin, kRejoinBound);
    }
}

// The stand-in for "the host's copy ended": a forced remove() on an inactive copy would run setEvent(false) and
// underflow the refcount (lib.cpp:3132-3142), so the drill destroys the inactive copy as the wrapper does for the
// lane's own absent apply. It runs once a joiner's world is taken and not yet ready, so the blob holds a copy the
// host no longer has and the seed at the joiner's world-ready says absent.
void HostRejoin(coop::net::Session* s) {
    if (!Due()) return;
    for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers); ++slot) {
        if (!coop::save_transfer::WorldTakenFor(slot) || s->IsSlotWorldReady(slot)) continue;
        Snap sn;
        if (!Take(true, sn)) return;
        if (sn.n != 1) return Fail(true, "%d live copies on this host when slot %d's world was taken, expected 1", sn.n, slot);
        if (!BS::DestroyInactive(sn.first)) return Fail(true, "the inactive copy was not destroyed");
        g_rjSlot = slot;
        UE_LOGI("badsun_drill: [host] slot %d's world is taken and not yet ready; this host's inactive copy destroyed "
                "(the drill's stand-in for the host's copy ended)", slot);
        Enter(P::HostReady, kReadyBound);
        return;
    }
}

void HostReady(coop::net::Session* s) {
    if (!s->IsSlotWorldReady(g_rjSlot)) return;
    UE_LOGI("badsun_drill: [host] slot %d is world-ready, after the destroy; its seed says absent", g_rjSlot);
    g_phase = P::Done;
}

// ---- a client's phases -----------------------------------------------------------------------------------------

// spawn: the host's copy arrives, then goes active.
void CopyAppear() {
    if (!Due()) return;
    Snap sn;
    if (!Take(false, sn)) return;
    if (sn.n >= 2) return Fail(false, "%d live copies, expected 1", sn.n);
    if (sn.n == 1) {
        g_tMark = Steady::now();
        UE_LOGI("badsun_drill: [client] the host's copy arrived (%s); waiting for it to go active", g_desc);
        Enter(P::CopyActive, kActiveBound);
    }
}

void CopyActive() {
    if (!Due()) return;
    Snap sn;
    if (!Take(false, sn)) return;
    if (sn.n != 1) return Fail(false, "%d live copies, expected 1", sn.n);
    if (sn.active != 1) return;
    const uint64_t spawned = WB::GetCounts().spawned;
    if (spawned == 0) return Fail(false, "one active copy, but the lane spawned none: it came by another path");
    Pass(false, "exactly one copy, active %.1f s after it arrived; the lane spawned %llu",
         std::chrono::duration<float>(Steady::now() - g_tMark).count(), static_cast<unsigned long long>(spawned));
}

// cheat and join: the host's 08:00 reaches this clock.
void ClockWait() {
    if (coop::time_sync::LastHostDayZ() < 0 || !ReadClock() || g_hour < 8) return;
    if (ArmOf() == Arm::Join) {
        g_tMark = Steady::now();
        UE_LOGI("badsun_drill: [client] the clock reads %02d:%02d; waiting for the copy to go active", g_hour, g_min);
        Enter(P::JoinActive, kActiveBound);
        return;
    }
    if (!Due()) return;
    Snap sn;
    if (!Take(false, sn)) return;
    if (sn.n != 0) return Inconclusive(false, "%d live copies before this client's own call", sn.n);
    g_refused0 = WB::GetCounts().refused;
    bool cheats = false;
    const bool cheatsRead = GM::CheatsAllowed(cheats);
    // The cheat menu's own handler is gated by hasWeapon, which only Sandbox or flying sets; a New Game is Story, so the
    // handler would stop at its "Illegal action" and never reach the verb. The reflected verb meets the same gate.
    if (!BS::CallSpawn()) return Fail(false, "the gamemode's Spawn Bad Sun did not run");
    UE_LOGI("badsun_drill: [client] clock %02d:%02d; this client's own Spawn Bad Sun called (cheats allowed %s); "
            "watching %d s", g_hour, g_min, CheatsWord(cheatsRead, cheats), static_cast<int>(kCallBound.count()));
    Enter(P::CallWatch, kCallBound);
}

void CallWatch() {
    if (!Due()) return;
    Snap sn;
    if (Take(false, sn) && sn.n > 0)
        Fail(false, "%d live copies after this client's own Spawn Bad Sun: the gate did not hold it back", sn.n);
}

void JoinActive() {
    if (!Due()) return;
    Snap sn;
    if (!Take(false, sn)) return;
    if (sn.n >= 2) return Fail(false, "%d live copies, expected 1", sn.n);
    if (sn.n == 1 && sn.active == 1) {
        const WB::Counts c = WB::GetCounts();
        Pass(false, "the seed's apply counted %d copy (the blob's) and spawned %llu; one copy, active %.1f s after the "
             "clock read %02d:%02d", c.lastApplyLive, static_cast<unsigned long long>(c.spawned),
             std::chrono::duration<float>(Steady::now() - g_tMark).count(), g_hour, g_min);
    }
}

// midnight: the first sample, before the host's edge.
void BaseWait() {
    const int32_t hostDay = coop::time_sync::LastHostDayZ();
    if (hostDay < 0 || !ReadClock() || !Due()) return;
    Snap sn;
    if (!Take(false, sn)) return;
    if (g_hour == 0) return Inconclusive(false, "this peer's first sample came after the host's edge (clock %02d:%02d)", g_hour, g_min);
    if (sn.n != 0) return Inconclusive(false, "%d live copies before the host's edge", sn.n);
    g_dayZ = hostDay;
    UE_LOGI("badsun_drill: [client] clock %02d:%02d on the host's day %d; waiting for its day edge", g_hour, g_min, hostDay);
    Enter(P::EdgeWait, kEdgeBound);
}

void EdgeWait() {
    if (coop::time_sync::LastHostDayZ() == g_dayZ) return;
    g_tMark = Steady::now();
    UE_LOGI("badsun_drill: [client] the host's day number moved %d -> %d", g_dayZ, coop::time_sync::LastHostDayZ());
    Enter(P::EdgeWatch, kEdgeCopyBound);
}

void EdgeWatch() {
    if (!Due()) return;
    Snap sn;
    if (!Take(false, sn)) return;
    ReadClock();
    if (sn.n >= 2) return Fail(false, "%d live copies after the edge, expected 1", sn.n);
    if (sn.n == 1 && sn.active == 1) return Fail(false, "the copy is active at %02d:%02d", g_hour, g_min);
    if (sn.n == 1 && sn.inactive == 1)
        Pass(false, "exactly one inactive copy %.1f s after the host's day edge (clock %02d:%02d)",
             std::chrono::duration<float>(Steady::now() - g_tMark).count(), g_hour, g_min);
}

// join and absent_stale: the seed's apply, the first proof of what the lane did at the world-ready.
void ApplyWait() {
    const WB::Counts c = WB::GetCounts();
    if (!c.lastApplied) return;
    if (ArmOf() == Arm::Join) {
        if (!c.lastApplyWant || c.lastApplyLive != 1 || c.lastApplySpawned)
            return Fail(false, "the apply wanted %d, counted %d copies and spawned %d; expected 1, 1 and 0",
                        c.lastApplyWant ? 1 : 0, c.lastApplyLive, c.lastApplySpawned ? 1 : 0);
        UE_LOGI("badsun_drill: [client] the apply counted 1 copy (the blob's) and spawned none; waiting for the host's "
                "clock to read 08:00");
        Enter(P::ClockWait, kClockBound);
        return;
    }
    // absent_stale. A life-1 client meets an absent seed with no copy, then the host's live edge (spawned here).
    if (c.lastApplyWant && c.lastApplySpawned) {
        Enter(P::LiveWait, kLiveBound);
        return;
    }
    if (c.lastApplyWant)
        return Inconclusive(false, "the seed said a Bad Sun is present: the host's destroy came after this world-ready (see "
                            "the host's line)");
    // At 02:00 the blob's copy cannot be active: one that was (left active) is not the case built, and one that
    // stayed alive with nothing left active is the lane failing to destroy a stale copy.
    if (c.lastApplyLive >= 1 && c.leftActive > 0)
        return Inconclusive(false, "the blob's copy was active when the seed applied (left active %llu)",
                            static_cast<unsigned long long>(c.leftActive));
    if (c.lastApplyLive >= 1 && c.destroyed == 0)
        return Fail(false, "the stale copy was not destroyed: the seed applied absent to %d live copies and destroyed "
                    "none (apply failures %llu)", c.lastApplyLive, static_cast<unsigned long long>(c.applyFailed));
    if (c.lastApplyLive >= 1) {
        UE_LOGI("badsun_drill: [client] the seed applied absent to the blob's copy: destroyed %llu; checking for no copy "
                "in %d s", static_cast<unsigned long long>(c.destroyed), static_cast<int>(kZeroBound.count()));
        Enter(P::ZeroWatch, kZeroBound);
        return;
    }
    Enter(P::LiveWait, kLiveBound);
}

void LiveWait() {
    if (!Due()) return;
    Snap sn;
    if (!Take(false, sn)) return;
    if (sn.n >= 2) return Fail(false, "%d live copies, expected 1", sn.n);
    if (sn.n == 1 && sn.inactive == 1) {
        UE_LOGI("badsun_drill: [client] absent_stale ready for the rejoin");
        g_phase = P::Done;
    }
}

void AbsentWait() {
    if (!Due()) return;
    const WB::Counts c = WB::GetCounts();
    Snap sn;
    Take(false, sn);
    if (g_phase == P::Done) return;
    const bool absentApplied = c.lastApplied && !c.lastApplyWant;
    if (c.leftActive > g_c0.leftActive) {
        g_tMark = Steady::now();
        UE_LOGI("badsun_drill: [client] the host's absent arrived while this copy was active; waiting for its own "
                "remove()");
        Enter(P::OwnRemove, kOwnRemoveBound);
    } else if (c.destroyed > g_c0.destroyed) {
        Inconclusive(false, "the copy was inactive when the host's absent arrived");
    } else if (absentApplied && c.lastApplyLive >= 1) {
        Fail(false, "the absent apply counted %d live copies and neither left an active one nor destroyed an inactive "
             "one (apply failures %llu)", c.lastApplyLive, static_cast<unsigned long long>(c.applyFailed));
    } else if (c.removesRan > g_c0.removesRan) {
        Inconclusive(false, "this copy's own remove() ran before the host's absent arrived");
    }
}

void OwnRemove() {
    if (!Due()) return;
    const WB::Counts c = WB::GetCounts();
    Snap sn;
    if (!Take(false, sn) || sn.n != 0 || c.removesRan <= g_c0.removesRan) return;
    float mult = 0.f;
    // The copy is gone and cannot change back: an unread sky (its lookup is throttled) is asked again until the bound.
    if (!ue_wrap::skysphere::ReadSunIntensityMult(mult)) return;
    if (std::fabs(mult - 1.f) > kSkyTolerance)
        return Fail(false, "the copy ended but the sky's sunIntensityMult reads %.3f, not 1", mult);
    Pass(false, "absent arrived while the copy was active; its own remove() ran %.1f s later and the sky's "
         "sunIntensityMult reads %.3f", std::chrono::duration<float>(Steady::now() - g_tMark).count(), mult);
}

// ---- the bound ending a phase ------------------------------------------------------------------------------

void OnDeadline(bool host) {
    ReadClock();
    Snap sn;
    switch (g_phase) {
    case P::CallWatch: {
        const uint64_t moved = WB::GetCounts().refused - g_refused0;
        const bool read = Take(false, sn);
        if (g_phase == P::Done) return;
        if (!read) return Fail(false, "this client's copies did not read");
        if (sn.n != 0) return Fail(false, "%d live copies after its own Spawn Bad Sun", sn.n);
        bool cheats = false;
        const bool cheatsRead = GM::CheatsAllowed(cheats);
        if (moved == 0) return Fail(false, "no copy, but the lane's refusal count did not move");
        return Pass(false, "no copy %d s after its own Spawn Bad Sun and the lane refused it (%llu); the reflected verb, "
                    "since the cheat menu's own handler needs hasWeapon (Sandbox or flying; cheats allowed %s): the "
                    "in-graph route stays RD (read, not driven)", static_cast<int>(kCallBound.count()),
                    static_cast<unsigned long long>(moved), CheatsWord(cheatsRead, cheats));
    }
    case P::ZeroWatch: {
        const WB::Counts c = WB::GetCounts();
        const bool read = Take(false, sn);
        if (g_phase == P::Done) return;
        if (!read) return Fail(false, "this client's copies did not read");
        if (sn.n != 0) return Fail(false, "the joiner keeps %d copies after the seed applied absent", sn.n);
        if (c.destroyed != 1) return Fail(false, "the lane destroyed %llu copies, expected 1", static_cast<unsigned long long>(c.destroyed));
        return Pass(false, "the seed applied absent to the blob's inactive copy (the lane destroyed 1); no copy %d s later",
                    static_cast<int>(kZeroBound.count()));
    }
    default:
        break;
    }
    Take(host, sn);
    if (g_phase == P::Done) return;
    switch (g_phase) {
    case P::WaitJoin:  return Fail(host, "timed out waiting for the join");
    case P::Setup:      return Fail(host, "the world did not read within %d s of the join being over", static_cast<int>(kSetupBound.count()));
    case P::HostEdge:   return Fail(true, "the day number did not move within %d s of the clock set to 23:58 (clock %02d:%02d)", static_cast<int>(kEdgeBound.count()), g_hour, g_min);
    case P::HostRoll: {
        bool present = false;
        BS::ReadGamemodeCopy(nullptr, present);
        return Fail(true, "%d s after the day edge: the watch counted %llu Spawn Bad Sun calls, gamemode.badSun present %d",
                    static_cast<int>(kEdgeCopyBound.count()), static_cast<unsigned long long>(RollRan() - g_ranBefore),
                    present ? 1 : 0);
    }
    case P::HostActive: return Fail(true, "its copy was not active by both tests %d s after the clock set to 08:00", static_cast<int>(kActiveBound.count()));
    case P::HostAlpha:  return Fail(true, "the widget's alpha did not read %.1f for %d s within %d s", kAlphaGate, static_cast<int>(kAlphaHold.count()), static_cast<int>(kAlphaBound.count()));
    case P::HostRemove: return Fail(true, "no own remove() with the lane's absent within %d s of the clock set to 18:59 (clock %02d:%02d)", static_cast<int>(kRemoveBound.count()), g_hour, g_min);
    case P::HostCopy:   return Fail(true, "no inactive copy within %d s of Spawn Bad Sun", static_cast<int>(kSetupBound.count()));
    case P::HostRejoin: return Fail(true, "no joiner's world was taken within %d s of the first copy", static_cast<int>(kRejoinBound.count()));
    case P::HostReady:  return Fail(true, "slot %d was not world-ready within %d s of the destroy", g_rjSlot, static_cast<int>(kReadyBound.count()));
    case P::CopyAppear: return Fail(false, "no copy within %d s of the join (clock %02d:%02d)", static_cast<int>(kAppearBound.count()), g_hour, g_min);
    case P::CopyActive: return Fail(false, "the copy was not active by both tests within %d s of arriving (clock %02d:%02d)", static_cast<int>(kActiveBound.count()), g_hour, g_min);
    case P::ClockWait:  return Fail(false, "the host's clock never read 08:00 here within %d s (last read %02d:%02d)", static_cast<int>(kClockBound.count()), g_hour, g_min);
    case P::JoinActive: return Fail(false, "the copy was not active by both tests %d s after the clock read 08:00", static_cast<int>(kActiveBound.count()));
    case P::BaseWait:   return Fail(false, "no first sample within %d s of the join (host day %d, clock %02d:%02d)", static_cast<int>(kClockBound.count()), coop::time_sync::LastHostDayZ(), g_hour, g_min);
    case P::EdgeWait:   return Fail(false, "the host's day number never moved within %d s (clock %02d:%02d)", static_cast<int>(kEdgeBound.count()), g_hour, g_min);
    case P::EdgeWatch:  return Fail(false, "%d live copies %d s after the host's day edge (clock %02d:%02d)", sn.n, static_cast<int>(kEdgeCopyBound.count()), g_hour, g_min);
    case P::ApplyWait:  return Fail(false, "no apply of the host's state within %d s of the join", static_cast<int>(kApplyBound.count()));
    case P::LiveWait:   return Inconclusive(false, "no copy came, from the blob or a live edge, within %d s: the blob carried none", static_cast<int>(kLiveBound.count()));
    case P::AbsentWait: return Fail(false, "the host's absent never arrived within %d s", static_cast<int>(kAbsentBound.count()));
    case P::OwnRemove: {
        const uint64_t removes = WB::GetCounts().removesRan;
        if (removes > g_c0.removesRan && sn.n == 0)
            return Fail(false, "this copy ended but the sky's sunIntensityMult did not read within %d s of the host's absent",
                        static_cast<int>(kOwnRemoveBound.count()));
        if (removes > g_c0.removesRan)
            return Fail(false, "this copy's own remove() ran but %d copies are still live %d s after the host's absent", sn.n,
                        static_cast<int>(kOwnRemoveBound.count()));
        return Fail(false, "this copy's own remove() did not run within %d s of the host's absent (removes %llu)", static_cast<int>(kOwnRemoveBound.count()), static_cast<unsigned long long>(removes));
    }
    default:            return Fail(host, "timed out in phase %d", static_cast<int>(g_phase));
    }
}

// absent_stale, the HOST: the last client leaving ends the host's session too, so the drill re-arms from Begin at the
// rejoin while its world keeps the copy it made. One inactive copy at that arm is this run's own, and the arm resumes
// at the window it waits for (a joiner's world taken, not yet ready) instead of reading it as a stale world.
// 0 = not a resume (the arm starts as usual), 1 = resumed, -1 = a resume whose read failed: Begin asks again.
int ResumeStaleHost() {
    if (!g_staleSpawned) return 0;
    Snap sn;
    if (!Take(true, sn)) return -1;
    if (sn.n != 1 || sn.inactive != 1) {
        Inconclusive(true, "resumed after the session's end with %d copies (%d inactive), not this run's one", sn.n,
                     sn.inactive);
        return 1;
    }
    UE_LOGI("badsun_drill: [host] arm absent_stale resumed after the session's end: this run's inactive copy is here; "
            "waiting for the joiner's world to be taken");
    Enter(P::HostRejoin, kRejoinBound);
    return 1;
}

}  // namespace

void LaneTick(coop::net::Session* s, bool host) {
    if (g_phase == P::Done) return;
    if (g_phase == P::Begin) {
        if (host && ArmOf() == Arm::AbsentStale) {
            const int r = ResumeStaleHost();
            if (r != 0) return;  // resumed, judged, or asked again next tick
        }
        Enter(P::WaitJoin, kJoinBound);
        UE_LOGI("badsun_drill: [%s] arm %s armed; waiting for the join to be over", RoleName(host), ArmName());
    }
    if (Steady::now() > g_deadline) return OnDeadline(host);
    if (g_phase == P::WaitJoin) {
        if (JoinOver(s, host, g_slot)) Enter(P::Setup, kSetupBound);
        return;
    }
    if (g_phase == P::Setup) {
        if (host) HostSetup();
        else      ClientSetup();
        return;
    }
    switch (g_phase) {
    case P::HostEdge:   HostEdge(); return;
    case P::HostRoll:   HostRoll(); return;
    case P::HostActive: HostActive(); return;
    case P::HostAlpha:  HostAlpha(); return;
    case P::HostRemove: HostRemove(); return;
    case P::HostCopy:   HostCopy(); return;
    case P::HostRejoin: HostRejoin(s); return;
    case P::HostReady:  HostReady(s); return;
    case P::CopyAppear: CopyAppear(); return;
    case P::CopyActive: CopyActive(); return;
    case P::ClockWait:  ClockWait(); return;
    case P::JoinActive: JoinActive(); return;
    case P::CallWatch:  CallWatch(); return;
    case P::BaseWait:   BaseWait(); return;
    case P::EdgeWait:   EdgeWait(); return;
    case P::EdgeWatch:  EdgeWatch(); return;
    case P::ApplyWait:  ApplyWait(); return;
    case P::LiveWait:   LiveWait(); return;
    case P::ZeroWatch:  return;  // judged at its bound
    case P::AbsentWait: AbsentWait(); return;
    case P::OwnRemove:  OwnRemove(); return;
    default:            return;
    }
}

void LaneReset() {
    RestoreMode();
    g_phase = P::Begin;
    g_slot = g_rjSlot = -1;
    g_drainFails = 0;
    g_hour = g_min = -1;
    std::snprintf(g_desc, sizeof(g_desc), "not read");
    g_dayZ = -1;
    g_ranBefore = 0;
    g_c0 = WB::Counts{};
    g_refused0 = 0;
}

}  // namespace coop::dev::badsun_drill::detail

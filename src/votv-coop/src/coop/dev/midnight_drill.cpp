// coop/dev/midnight_drill.cpp -- see coop/dev/midnight_drill.h.

#include "coop/dev/midnight_drill.h"

#include "coop/config/config.h"
#include "coop/dev/rollover_watch.h"
#include "coop/dev/set_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers
#include "coop/player/sleep_sync.h"
#include "coop/props/prop_snapshot.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/world/time_sync.h"

#include "ue_wrap/actors/sleep.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/active_events.h"
#include "ue_wrap/world/daynightcycle.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::midnight_drill {
namespace {

namespace DNC = ue_wrap::daynightcycle;
namespace SLP = ue_wrap::sleep;
namespace R   = ue_wrap::reflection;
namespace sg  = ue_wrap::script_gate;
namespace AE  = ue_wrap::active_events;
namespace WI  = ue_wrap::world_identity;

enum class Arm { Off, Awake, Asleep, Cheat };

// The host: the join and the set, then (asleep) a quiet world, the bed, the fast-forward, the wake.
// The client: (asleep) the join, the bed, the fast-forward, the wake; (cheat) its writes. The set and
// the sleep entry both finish inside their call, so each is judged the moment it returns.
enum class Step { WaitJoin, WaitQuiet, WaitAccelerate, WatchWake, Done, Invalid };

constexpr float kAwakeFraction  = 0.999f;  // a few game units before the wrap
constexpr float kAsleepFraction = 0.98f;   // runway for the bed and the gate, burnt at 1x until then
constexpr float kNeedForTheArm  = 30.f;    // the wake loop ends a sleep at a need of 100
constexpr int   kCheatWrites    = 3;       // one write can meet a host sample at the same tick

// Game thread only, but for the session pointer the Install fanout stores.
std::atomic<coop::net::Session*> g_session{nullptr};
Step    g_step = Step::WaitJoin;
bool    g_saidArm = false;
bool    g_saidEvents = false;
int     g_slot = -1;          // the host's client whose join armed it
int32_t g_setDayZ = -1;       // the host's day number when it set the clock
uint64_t g_minutesAtAccel = 0;  // this peer's minute pulses when the fast-forward began
bool     g_pokedRate = false;   // the clock latch's control write is done (client, once per session)

// The cheat arm, client: the writes made and how each ended, and the one awaiting the cycle's tick
// with what it is judged against.
int      g_cheatWrites = 0;
int      g_cheatRolled = 0;
int      g_cheatHeld = 0;      // the tick found the day it had before the write
int      g_cheatMet = 0;       // a host sample was written in at the same tick
bool     g_cheatPending = false;
uint64_t g_cheatHashRan = 0;   // generteHashcode's run count before the write
int32_t  g_cheatDayZ = -1;     // this client's day number before the write
float    g_cheatDay = 0.f;     // and its `day`

// Who ends the night. Three classes declare a function of this name (the gamemode's, the player's,
// the ATV's wakeUp), so the watch keeps only the gamemode's, by its declaring class. Each entry is
// logged with its caller; our own sleep lane's reflected wakeup at the END is one of them.
constexpr const wchar_t* kWakeupName = L"wakeup";
constexpr int kWakeupTag = 0x4D440001;  // 'MD'
constexpr int kMaxWakeLines = 8;
void*    g_gmClass = nullptr;     // looked up once per world: a new world may load it anew
uint32_t g_gmClassGen = 0;
bool     g_wakeWatched = false;
bool     g_wakeRefused = false;   // the gate refused it; a full table refuses it again, so never retried
bool     g_wakeLive = false;
std::chrono::steady_clock::time_point g_nextLiveCheck{};
int      g_wakeLines = 0;

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::midnight_drill);
        return v == "awake" ? Arm::Awake : v == "asleep" ? Arm::Asleep : v == "cheat" ? Arm::Cheat : Arm::Off;
    }();
    return a;
}

const char* ArmName() {
    switch (ArmOf()) {
    case Arm::Awake:  return "awake";
    case Arm::Asleep: return "asleep";
    case Arm::Cheat:  return "cheat";
    default:          return "off";
    }
}

// What this peer does on its arm, for the first line.
const char* ArmPlan(bool host) {
    if (ArmOf() == Arm::Cheat)
        return host ? "this host leaves its clock alone" : "writing a day onto this client's clock once joined";
    if (host) return "waiting for a client's join to end";
    return ArmOf() == Arm::Asleep ? "going to bed once joined" : "watching; the host sets the clock";
}

bool IsHost(coop::net::Session* s) { return s->role() == coop::net::Role::Host; }

void Invalid(char role, const char* why) {
    g_step = Step::Invalid;
    UE_LOGW("midnight_drill: [%c] INVALID (arm %s) -- %s", role, ArmName(), why);
}

// The vitals and gate inputs the sleep entry refuses on and the wake loop ends a sleep on. Field reads
// only, but for the dilation, which is a reflected call and is left out inside a gate callback.
std::string SleepInputs(bool withDilation = true) {
    float food = -1.f, need = -1.f;
    int32_t events = -1;
    ue_wrap::vitals::Read(ue_wrap::vitals::Field::Food, &food);
    SLP::ReadSleepNeed(need);
    AE::ReadCount(events);
    char buf[128];
    int n = std::snprintf(buf, sizeof(buf), "food %.1f need %.1f activeEvents %d isSleep %d", food, need, events,
                          SLP::IsSleeping() ? 1 : 0);
    if (withDilation && n > 0 && n < static_cast<int>(sizeof(buf)))
        std::snprintf(buf + n, sizeof(buf) - n, " dilation %.2f", SLP::GetGlobalTimeDilation());
    return buf;
}

// What a set leaves to burn, at the rate in force now and at the fast-forward's 20x.
void PrintRunway(float frac) {
    float maxTime = 0.f, total = 0.f, day = 0.f, scale = 0.f;
    DNC::Rates rt{};
    if (!DNC::ReadMaxTime(maxTime) || !DNC::ReadClock(total, day, scale) || !DNC::ReadRates(rt)) {
        UE_LOGW("midnight_drill: [H] runway unreadable -- the clock or its rates did not resolve");
        return;
    }
    const float dilation = SLP::GetGlobalTimeDilation();
    const float perGameSecond = scale * rt.diffMult * rt.settingMultiplayer * rt.sleepingTimeDilation;
    const float units = (1.f - frac) * maxTime;
    const float here = (perGameSecond > 0.f && dilation > 0.f) ? units / (perGameSecond * dilation) : -1.f;
    const float fast = perGameSecond > 0.f ? units / (perGameSecond * 20.f) : -1.f;
    UE_LOGI("midnight_drill: [H] runway -- %.3f of maxTime %.1f leaves %.1f units; timescale %.3f diff_mult "
            "%.3f settingMultiplayer %.3f sleepingTimeDilation %.3f dilation %.2f -> %.2f s at this dilation, "
            "%.2f s at 20x",
            frac, maxTime, units, scale, rt.diffMult, rt.settingMultiplayer, rt.sleepingTimeDilation, dilation, here,
            fast);
}

bool HostDayMoved() {
    int32_t h = 0, m = 0, z = 0;
    return DNC::ReadSavedTime(h, m, z) && g_setDayZ >= 0 && z != g_setDayZ;
}

// The sleep entry sets isSleep inside the call itself, so the answer is read the moment it returns:
// asleep, or refused (an active event, food at 10, no floor under the player or the bed).
void GoToBed(char role) {
    const std::string before = SleepInputs();
    SLP::WriteSleepNeed(kNeedForTheArm);
    void* bed = SLP::FindBed();
    const bool called = bed && SLP::CallSleep(bed);
    const std::string after = SleepInputs();
    UE_LOGI("midnight_drill: [%c] to bed -- bed %s, call %s | before: %s | after: %s", role,
            bed ? "found" : "NONE", called ? "dispatched" : "FAILED", before.c_str(), after.c_str());
    if (!called) {
        Invalid(role, bed ? "the sleep call did not dispatch" : "no bed_C in the world");
        return;
    }
    if (!SLP::IsSleeping()) {
        const std::string why = "the sleep call was refused | " + after;
        Invalid(role, why.c_str());
        return;
    }
    UE_LOGI("midnight_drill: [%c] in bed; waiting for the shared fast-forward", role);
    g_step = Step::WaitAccelerate;
}

void CheckAccelerate(char role) {
    if (coop::sleep_sync::InAcceleratePhase()) {
        float total = 0.f, day = 0.f, scale = 0.f;
        DNC::ReadClock(total, day, scale);
        g_minutesAtAccel = coop::dev::rollover_watch::RanCount(L"func_newMinute");
        UE_LOGI("midnight_drill: [%c] ACCELERATE -- timescale %.3f day=%.2f | %s", role, scale, day,
                SleepInputs().c_str());
        g_step = Step::WatchWake;
        return;
    }
    if (role == 'H' && HostDayMoved()) {
        Invalid(role, "the host reached midnight before the fast-forward began: the runway burnt out at 1x");
        return;
    }
    if (!SLP::IsSleeping()) {
        const std::string why = "woken before the fast-forward began | " + SleepInputs();
        Invalid(role, why.c_str());
    }
}

// The night's end, with the inputs the wake loop reads: a need of 100, food at 20, an active event.
// None of them set means something called wakeup() directly (a nightmare, the ariral, the drone).
void WatchWake(char role) {
    if (SLP::IsSleeping()) return;
    float total = 0.f, day = 0.f, scale = 0.f;
    DNC::ReadClock(total, day, scale);
    int32_t h = 0, m = 0, z = 0;
    DNC::ReadSavedTime(h, m, z);
    // The minute pulses this peer ran through the night: a client that saw every game minute of the
    // host's clock matches the host's count; one whose samples skipped minutes falls short.
    const uint64_t minutes = coop::dev::rollover_watch::RanCount(L"func_newMinute") - g_minutesAtAccel;
    UE_LOGI("midnight_drill: [%c] woke -- savedtime %d:%02d day %d, day=%.2f, %llu minute pulses since the "
            "fast-forward | %s", role, h, m, z, day, static_cast<unsigned long long>(minutes),
            SleepInputs().c_str());
    g_step = Step::Done;
}

sg::Verdict OnWakeupPre(const sg::Call& c) {
    if (!c.function || R::OuterOf(c.function) != g_gmClass || g_wakeLines >= kMaxWakeLines)
        return sg::Verdict::Run;
    ++g_wakeLines;
    const std::wstring fn = c.callerFunction ? R::ToString(R::NameOf(c.callerFunction)) : L"<ProcessEvent>";
    const std::wstring cls = c.callerObject ? R::ClassNameOf(c.callerObject) : L"-";
    UE_LOGI("midnight_drill: wakeup() entered from %ls::%ls%s | %s", cls.c_str(), fn.c_str(),
            c.fromOurCode ? " (a reflected call of ours)" : "", SleepInputs(false).c_str());
    return sg::Verdict::Run;
}

// Registered once per process, on the asleep arm only, and said LIVE once the gate has resolved the
// name; its liveness is asked at most once a second, since a name that resolved into a full table
// never goes live. The class it filters on is looked up once per world.
void EnsureWakeWatch() {
    if (ArmOf() != Arm::Asleep || g_wakeRefused || !sg::IsInstalled()) return;
    // A lookup that misses walks the object array, so a miss waits for the next world.
    const uint32_t gen = WI::Generation();
    if (gen != g_gmClassGen) {
        g_gmClass = R::FindClass(L"mainGamemode_C");
        g_gmClassGen = gen;
    }
    if (!g_gmClass) return;
    if (!g_wakeWatched) {
        g_wakeWatched = sg::WatchName(kWakeupName, kWakeupTag, &OnWakeupPre, nullptr);
        if (!g_wakeWatched) {
            g_wakeRefused = true;
            UE_LOGW("midnight_drill: the gate refused the wakeup watch -- a wake names no caller this run");
            return;
        }
    }
    if (g_wakeLive) return;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextLiveCheck) return;
    g_nextLiveCheck = now + std::chrono::seconds(1);
    sg::ResolvePendingNames();
    if (!sg::NameWatchLive(kWakeupName, kWakeupTag)) {
        if (sg::PendingNameCount() == 0) {
            g_wakeRefused = true;  // resolved into a full table: it will never go live
            UE_LOGW("midnight_drill: the wakeup watch is dead in a full gate table -- a wake names no caller this run");
        }
        return;
    }
    g_wakeLive = true;
    UE_LOGI("midnight_drill: the wakeup watch is LIVE; each entry on the gamemode names its caller");
}

void TickHost(coop::net::Session* s) {
    switch (g_step) {
    case Step::WaitJoin: {
        for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers) && g_slot < 0; ++slot)
            if (s->IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot)) g_slot = slot;
        if (g_slot < 0) return;
        if (ArmOf() == Arm::Cheat) {
            UE_LOGI("midnight_drill: [H] arm cheat -- slot %d's join is over; this host's clock is left alone "
                    "while its client writes a day onto its own", g_slot);
            g_step = Step::Done;
            return;
        }
        int32_t h = 0, m = 0;
        if (!DNC::ReadSavedTime(h, m, g_setDayZ)) return;
        const float frac = ArmOf() == Arm::Awake ? kAwakeFraction : kAsleepFraction;
        UE_LOGI("midnight_drill: [H] arm %s -- slot %d's join is over (world-ready, bracket closed); setting "
                "the clock to %.3f of day %d", ArmName(), g_slot, frac, g_setDayZ);
        PrintRunway(frac);
        if (!coop::dev::set_clock::ApplyTimeFraction(frac)) {
            Invalid('H', "the clock set was refused (the dev gate, or a clock that did not resolve)");
            return;
        }
        if (ArmOf() == Arm::Awake) {
            UE_LOGI("midnight_drill: [H] the clock is set; the evidence is rollover_watch's DAY lines");
            g_step = Step::Done;
        } else {
            UE_LOGI("midnight_drill: [H] the clock is set; waiting for no event to be active");
            g_step = Step::WaitQuiet;
        }
        return;
    }
    case Step::WaitQuiet: {
        int32_t events = -1;
        if (!AE::ReadCount(events)) return;
        if (HostDayMoved()) {
            Invalid('H', "the host reached midnight before its bed: the runway burnt out at 1x");
            return;
        }
        if (events > 0) {
            if (!g_saidEvents) {
                g_saidEvents = true;
                UE_LOGI("midnight_drill: [H] %d event(s) active; the bed waits for none", events);
            }
            return;
        }
        GoToBed('H');
        return;
    }
    case Step::WaitAccelerate: CheckAccelerate('H'); return;
    case Step::WatchWake:      WatchWake('H'); return;
    default:                   return;
    }
}

// The clock latch's own control, both arms: once joined, write a rate into this client's clock as a
// game writer would (the cheat menu, the purple wisp's rewind); time_sync must name it and hold 0
// before the cycle's next tick, so the clock does not move.
void PokeClockRate() {
    if (g_pokedRate || !coop::net_pump::HasAnnouncedWorldReady() ||
        coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
        return;
    g_pokedRate = true;
    DNC::WriteTimeScale(1.f);
    UE_LOGI("midnight_drill: [C] wrote 1 into this client's clock rate, as a game writer would -- time_sync "
            "must name it and hold 0");
}

// The cheat arm: what the cheat menu's day button writes -- a whole day onto both accumulators,
// `ui_cheatMenu` @23765 -- written into this client's clock, which puts `day` past the day's end, so
// the cycle's next tick rolls this machine's own midnight unless the clock lane rewrites it first.
// Each write waits for the clock to be the host's (a sample applied, the day number the host's, the
// rate held again after the control write); each is judged once the cycle has ticked, which brings
// `day` back below the day's end whether it rolled, was held at the day it had, or met a new sample.
void TickCheat() {
    if (g_step == Step::Done || g_step == Step::Invalid || !g_pokedRate) return;
    if (!coop::dev::rollover_watch::IsEnabled()) {
        Invalid('C', "rollover_watch is off on this peer: a roll of the hash codes would go uncounted");
        return;
    }
    float total = 0.f, day = 0.f, scale = 0.f, maxT = 0.f;
    int32_t h = 0, m = 0, z = 0;
    if (!DNC::ReadClock(total, day, scale) || !DNC::ReadMaxTime(maxT) || maxT <= 0.f || !DNC::ReadSavedTime(h, m, z))
        return;
    if (g_cheatPending) {
        if (day > maxT) return;  // the cycle has not ticked since the write
        g_cheatPending = false;
        const uint64_t ran = coop::dev::rollover_watch::RanCount(L"generteHashcode") - g_cheatHashRan;
        const bool rolled = ran > 0 || z != g_cheatDayZ;
        const bool held = !rolled && day == g_cheatDay;
        const char* verdict = rolled ? "ROLLED this client's own midnight"
                              : held ? "HELD at the day it had before the write"
                                     : "MET a host sample at the same tick, no roll";
        ++(rolled ? g_cheatRolled : held ? g_cheatHeld : g_cheatMet);
        UE_LOGI("midnight_drill: [C] write %d of %d %s -- generteHashcode ran %llu time(s), this client's day "
                "number %d -> %d, day %.2f -> %.2f", g_cheatWrites, kCheatWrites, verdict,
                static_cast<unsigned long long>(ran), g_cheatDayZ, z, g_cheatDay, day);
        if (g_cheatWrites >= kCheatWrites) {
            UE_LOGI("midnight_drill: [C] cheat done -- %d write(s): %d rolled this client's own midnight, %d held, "
                    "%d met a host sample", g_cheatWrites, g_cheatRolled, g_cheatHeld, g_cheatMet);
            g_step = Step::Done;
        }
        return;
    }
    const int32_t hostDay = coop::time_sync::LastHostDayZ();
    if (hostDay < 0 || z != hostDay || scale != 0.f) return;
    if (day <= 0.f) {
        Invalid('C', "the clock is at the day's start or running back: a day forward would not pass midnight");
        return;
    }
    g_cheatHashRan = coop::dev::rollover_watch::RanCount(L"generteHashcode");
    g_cheatDayZ = z;
    g_cheatDay = day;
    ++g_cheatWrites;
    g_cheatPending = true;
    DNC::ApplyClock(total + maxT, day + maxT);
    UE_LOGI("midnight_drill: [C] write %d of %d -- a day onto this client's clock, as the cheat menu's day button "
            "writes it: day %.2f -> %.2f, past the day's end %.1f (day number %d, the host's)", g_cheatWrites,
            kCheatWrites, day, day + maxT, maxT, z);
}

void TickClient() {
    PokeClockRate();
    if (ArmOf() == Arm::Cheat) {
        TickCheat();
        return;
    }
    if (ArmOf() != Arm::Asleep) return;  // awake: the client only watches
    switch (g_step) {
    case Step::WaitJoin:
        // Joined: world-ready announced and the join's phases over.
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        UE_LOGI("midnight_drill: [C] arm asleep -- joined; going to bed");
        GoToBed('C');
        return;
    case Step::WaitAccelerate: CheckAccelerate('C'); return;
    case Step::WatchWake:      WatchWake('C'); return;
    default:                   return;
    }
}

}  // namespace

bool IsEnabled() { return ArmOf() != Arm::Off; }

void Install(coop::net::Session* session) {
    if (!IsEnabled()) return;
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!IsEnabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !SLP::EnsureResolved()) return;
    AE::EnsureResolved();
    const bool host = IsHost(s);
    if (!g_saidArm) {
        g_saidArm = true;
        UE_LOGI("midnight_drill: [%c] arm %s -- %s", host ? 'H' : 'C', ArmName(), ArmPlan(host));
        if (!coop::dev::rollover_watch::IsEnabled())
            UE_LOGW("midnight_drill: rollover_watch is off on this peer -- the arm runs, but prints none of its "
                    "evidence");
    }
    EnsureWakeWatch();
    if (host) TickHost(s);
    else      TickClient();
}

void OnDisconnect() {
    g_step = Step::WaitJoin;
    g_saidArm = false;
    g_saidEvents = false;
    g_slot = -1;
    g_setDayZ = -1;
    g_minutesAtAccel = 0;
    g_pokedRate = false;
    g_cheatWrites = g_cheatRolled = g_cheatHeld = g_cheatMet = 0;
    g_cheatPending = false;
    g_cheatHashRan = 0;
    g_cheatDayZ = -1;
    g_cheatDay = 0.f;
    g_wakeLines = 0;
}

}  // namespace coop::dev::midnight_drill

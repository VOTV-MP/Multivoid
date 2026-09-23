// coop/dev/midnight_drill.cpp -- see coop/dev/midnight_drill.h.

#include "coop/dev/midnight_drill.h"

#include "coop/config/config.h"
#include "coop/dev/set_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"  // coop::players::kMaxPeers
#include "coop/player/sleep_sync.h"
#include "coop/props/prop_snapshot.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/sleep.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/world/daynightcycle.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::midnight_drill {
namespace {

namespace DNC = ue_wrap::daynightcycle;
namespace SLP = ue_wrap::sleep;
namespace R   = ue_wrap::reflection;
namespace sg  = ue_wrap::script_gate;

enum class Arm { Off, Awake, Asleep };

// The host: the join, the set, (asleep) the jump landing, a quiet world, the bed, the fast-forward,
// the wake. The client: (asleep) the join, the bed, the fast-forward, the wake.
enum class Step { WaitJoin, WaitJump, WaitQuiet, WaitBed, WaitAccelerate, WatchWake, Done, Invalid };

constexpr float kAwakeFraction  = 0.999f;  // a few game units before the wrap
constexpr float kAsleepFraction = 0.98f;   // runway for the bed and the gate, burnt at 1x until then
constexpr float kNeedForTheArm  = 30.f;    // the wake loop ends a sleep at a need of 100

// Game thread only, but for the session pointer the Install fanout stores.
std::atomic<coop::net::Session*> g_session{nullptr};
Step    g_step = Step::WaitJoin;
bool    g_saidArm = false;
bool    g_saidEvents = false;
int     g_slot = -1;          // the host's client whose join armed it
float   g_target = 0.f;       // the host's `day` the set writes
int32_t g_setDayZ = -1;       // the host's day number when it set the clock
int     g_bedTicks = 0;       // pump ticks since this peer's sleep call

// Who ends the night. Three classes declare a function of this name (the gamemode's, the player's,
// the ATV's wakeUp), so the watch keeps only the gamemode's, by its declaring class. Each entry is
// logged with its caller; our own sleep lane's reflected wakeup at the END is one of them.
constexpr const wchar_t* kWakeupName = L"wakeup";
constexpr int kWakeupTag = 0x4D440001;  // 'MD'
constexpr int kMaxWakeLines = 8;
void* g_gmClass = nullptr;
bool  g_wakeWatched = false;
int   g_wakeLines = 0;

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::midnight_drill);
        return v == "awake" ? Arm::Awake : v == "asleep" ? Arm::Asleep : Arm::Off;
    }();
    return a;
}

const char* ArmName() { return ArmOf() == Arm::Awake ? "awake" : "asleep"; }

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
    SLP::ReadActiveEvents(events);
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

void GoToBed(char role) {
    const std::string before = SleepInputs();
    SLP::WriteSleepNeed(kNeedForTheArm);
    void* bed = SLP::FindBed();
    const bool called = bed && SLP::CallSleep(bed);
    UE_LOGI("midnight_drill: [%c] to bed -- bed %s, call %s | before: %s | after: %s", role,
            bed ? "found" : "NONE", called ? "dispatched" : "FAILED", before.c_str(), SleepInputs().c_str());
    if (!called) {
        Invalid(role, bed ? "the sleep call did not dispatch" : "no bed_C in the world");
        return;
    }
    g_bedTicks = 0;
    g_step = Step::WaitBed;
}

// The sleep entry writes isSleep inside the call itself; one more pump tick absorbs a frame's
// delay, and a peer still awake after that was refused.
void CheckBed(char role) {
    if (SLP::IsSleeping()) {
        UE_LOGI("midnight_drill: [%c] in bed; waiting for the shared fast-forward | %s", role, SleepInputs().c_str());
        g_step = Step::WaitAccelerate;
        return;
    }
    if (++g_bedTicks < 2) return;
    const std::string why = "the sleep call was refused | " + SleepInputs();
    Invalid(role, why.c_str());
}

void CheckAccelerate(char role) {
    if (coop::sleep_sync::InAcceleratePhase()) {
        float total = 0.f, day = 0.f, scale = 0.f;
        DNC::ReadClock(total, day, scale);
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
    UE_LOGI("midnight_drill: [%c] woke -- savedtime %d:%02d day %d, day=%.2f | %s", role, h, m, z, day,
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

// Registered once per process, on the asleep arm only; live once the gate resolves the name.
void EnsureWakeWatch() {
    if (g_wakeWatched || ArmOf() != Arm::Asleep || !sg::IsInstalled()) return;
    if (!g_gmClass) g_gmClass = R::FindClass(L"mainGamemode_C");
    if (!g_gmClass) return;
    g_wakeWatched = sg::WatchName(kWakeupName, kWakeupTag, &OnWakeupPre, nullptr);
    if (!g_wakeWatched) UE_LOGW("midnight_drill: the gate refused the wakeup watch -- a wake names no caller");
}

void TickHost(coop::net::Session* s) {
    switch (g_step) {
    case Step::WaitJoin: {
        for (int slot = 1; slot < static_cast<int>(coop::players::kMaxPeers) && g_slot < 0; ++slot)
            if (s->IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot)) g_slot = slot;
        if (g_slot < 0) return;
        float maxTime = 0.f;
        int32_t h = 0, m = 0;
        if (!DNC::ReadMaxTime(maxTime) || !DNC::ReadSavedTime(h, m, g_setDayZ)) return;
        const float frac = ArmOf() == Arm::Awake ? kAwakeFraction : kAsleepFraction;
        UE_LOGI("midnight_drill: [H] arm %s -- slot %d's join is over (world-ready, bracket closed); setting "
                "the clock to %.3f of day %d", ArmName(), g_slot, frac, g_setDayZ);
        PrintRunway(frac);
        coop::dev::set_clock::SetTimeFraction(frac);
        g_target = frac * maxTime;
        g_step = ArmOf() == Arm::Awake ? Step::Done : Step::WaitJump;
        return;
    }
    case Step::WaitJump: {
        // The set is posted to the game thread; it has landed once `day` sits just past the target.
        float total = 0.f, day = 0.f, scale = 0.f;
        if (!DNC::ReadClock(total, day, scale) || day < g_target - 0.5f || day > g_target + 30.f) return;
        UE_LOGI("midnight_drill: [H] the jump landed (day=%.2f); waiting for no event to be active", day);
        g_step = Step::WaitQuiet;
        return;
    }
    case Step::WaitQuiet: {
        int32_t events = -1;
        if (!SLP::ReadActiveEvents(events)) return;
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
    case Step::WaitBed:        CheckBed('H'); return;
    case Step::WaitAccelerate: CheckAccelerate('H'); return;
    case Step::WatchWake:      WatchWake('H'); return;
    default:                   return;
    }
}

void TickClient() {
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
    case Step::WaitBed:        CheckBed('C'); return;
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
    const bool host = IsHost(s);
    if (!g_saidArm) {
        g_saidArm = true;
        UE_LOGI("midnight_drill: [%c] arm %s -- %s", host ? 'H' : 'C', ArmName(),
                host ? "waiting for a client's join to end"
                     : (ArmOf() == Arm::Asleep ? "going to bed once joined" : "watching; the host sets the clock"));
    }
    EnsureWakeWatch();
    if (g_wakeWatched) sg::ResolvePendingNames();
    if (host) TickHost(s);
    else      TickClient();
}

void OnDisconnect() {
    g_step = Step::WaitJoin;
    g_saidArm = false;
    g_saidEvents = false;
    g_slot = -1;
    g_target = 0.f;
    g_setDayZ = -1;
    g_bedTicks = 0;
    g_wakeLines = 0;
}

}  // namespace coop::dev::midnight_drill

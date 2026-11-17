// coop/dev/sleep_speedup_drill.cpp -- see coop/dev/sleep_speedup_drill.h.

#include "coop/dev/sleep_speedup_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/roster_ledger.h"
#include "coop/player/sleep_rest.h"
#include "coop/player/sleep_sync.h"
#include "coop/props/prop_snapshot.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "coop/player/players_registry.h"

#include "harness/autotest/peer_signal.h"

#include "ue_wrap/actors/sleep.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/active_events.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::sleep_speedup_drill {
namespace {

namespace SLP = ue_wrap::sleep;
namespace V   = ue_wrap::vitals;
namespace AE  = ue_wrap::active_events;
namespace PS  = harness::autotest::peer_signal;
namespace RV  = coop::rig_verdict;
namespace SS  = coop::sleep_sync;
using coop::net::SleepPeerState;
using Clock = std::chrono::steady_clock;

enum class Mode { Host, Everyone, Never };

// The host: the join, a quiet world, its bed, then the client's rest edge and (everyone) its wake edge, then its own
// wake. The client: the join, the host's sleep, a quiet world, its bed, the host's signal (or the host's wake), its wake.
enum class Step {
    WaitJoin, WaitHost, Quiet, HostWaitClientRest, HostWaitClientWake, HostWaitClientLeave, HostRowWaitHost,
    HostRowWaitEveryone, ClientResting, Ending, Done
};

constexpr float kHoldNeed  = 50.f;   // written every tick while asleep: the game's own wake at 100 never comes
constexpr float kWakeNeed  = 100.f;  // written once: the game's own poll wakes on it
constexpr float kNative    = 20.f;   // the game's sleep dilation (mainGamemode.cpp:5428)
constexpr float kFactorTol = 0.5f;
constexpr float kOneTol    = 0.01f;
constexpr const char* kTag = "sleep-speedup";
constexpr const char* kWakeSignal = "sleep_speedup_wake";  // the host to the client: it has read, wake now
constexpr auto kJoinBound  = std::chrono::seconds(300);
constexpr auto kQuietBound = std::chrono::seconds(120);
constexpr auto kPeerBound  = std::chrono::seconds(120);

Step        g_step = Step::WaitJoin;
const char* g_waitingFor = "the join";
Clock::time_point g_deadline{};
bool        g_armed = false;
bool        g_hold = false;        // this peer's sleep need is held at 50
bool        g_dreamHeld = false;   // the host's nightmare chance is held at 0
int         g_slot = -1;
Mode        g_mode = Mode::Everyone;

// modes: one arm per value of the host's row. row_change: the host's row changed mid-sleep, under everyone. leave: the
// client leaves while both sleep, under everyone or never.
enum class Arm { Off, Modes, RowChange, Leave };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::sleep_speedup_drill);
        return v == "modes" ? Arm::Modes : v == "row_change" ? Arm::RowChange : v == "leave" ? Arm::Leave : Arm::Off;
    }();
    return a;
}

bool On() { return ArmOf() != Arm::Off; }

Mode ReadMode() {
    const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::world_sleep_speedup);
    return v == "host" ? Mode::Host : v == "never" ? Mode::Never : Mode::Everyone;
}

const char* ModeName() { return g_mode == Mode::Host ? "host" : g_mode == Mode::Never ? "never" : "everyone"; }

// What the verdict and the log lines call this arm.
const char* Label() {
    static char buf[32];
    if (ArmOf() == Arm::RowChange) return "row_change";
    if (ArmOf() == Arm::Leave) std::snprintf(buf, sizeof(buf), "leave-%s", ModeName());
    else std::snprintf(buf, sizeof(buf), "%s", ModeName());
    return buf;
}

// Notifications of the row's change, counted: sleep_rest's subscriber is registered first, so by the time this one has
// run the host's sleep has been declared again, which is the edge row_change reads at.
int g_rowNotified = 0;
void OnRowNotified() { ++g_rowNotified; }
int g_rowNotifiedBase = 0;

bool Near(float v, float want, float tol) { return std::fabs(v - want) <= tol; }

void Enter(Step step, const char* waitingFor, std::chrono::seconds bound) {
    g_step = step;
    g_waitingFor = waitingFor;
    g_deadline = Clock::now() + bound;
}

void RestoreDream() {
    if (!g_dreamHeld) return;
    SLP::SetDreamProbability(-1.f);
    g_dreamHeld = false;
}

// The verdict line, once, in the shape `<mode>: <what>`.
void Say(RV::Outcome outcome, const char* fmt, ...) {
    char what[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(what, sizeof(what), fmt, ap);
    va_end(ap);
    RV::Say(kTag, outcome, "%s: %s", Label(), what);
}

// This peer's sleep is over: the need written to 100, which the game's own poll wakes on, and the DONE line at the
// wake. The host's verdict is said before this, at the edge that decided it.
void BeginEnding(bool host) {
    g_hold = false;
    V::Write(V::Field::Sleep, kWakeNeed);
    Enter(Step::Ending, host ? "the host to wake" : "the client to wake", kPeerBound);
}

void Ending(bool host) {
    if (SLP::IsSleeping()) return;
    if (host) RestoreDream();
    UE_LOGI("[SLEEP-SPEEDUP] %s %s DONE", Label(), host ? "host" : "client");
    g_step = Step::Done;
}

// The host's word reached this client (a signal file: read at a pace, since a poll of a folder is dear per frame). A
// client still waiting for the host's sleep or for its bed is done at once: the verdict is said.
bool Told() {
    static Clock::time_point next{};
    const auto now = Clock::now();
    if (now < next) return false;
    next = now + std::chrono::milliseconds(250);
    std::string text;
    return PS::Peek(kWakeSignal, text);
}

// A verdict at an edge, then the host wakes and the client is told to: nothing later is run.
void Decide(RV::Outcome outcome, const char* fmt, ...) {
    char what[224];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(what, sizeof(what), fmt, ap);
    va_end(ap);
    Say(outcome, "%s", what);
    PS::Mark(kWakeSignal);
    BeginEnding(true);
}

// ---- the host ---------------------------------------------------------------------------------

void HostSetRow(const char* value, Step next, const char* waitingFor);

// The host to bed through the game's own sleep, with the client awake, and the first read of the world's dilation in
// the same tick: the sleep watch's answer has run inside the call.
void HostGoToBed() {
    int32_t events = -1;
    if (!AE::ReadCount(events) || events > 0) return;  // the bound names the wait
    if (ArmOf() == Arm::RowChange && g_mode != Mode::Everyone)
        return Decide(RV::Outcome::Invalid, "row_change starts from world.sleep_speedup=everyone");
    if (ArmOf() == Arm::Leave && g_mode == Mode::Host)
        return Decide(RV::Outcome::Invalid, "leave runs under world.sleep_speedup=everyone or never");
    // A host that rests at 1 needs both watches: a dead one is a rig that cannot measure, not a failed rule.
    if (g_mode != Mode::Host) {
        const coop::sleep_rest::WatchState w = coop::sleep_rest::Watches();
        if (w == coop::sleep_rest::WatchState::Dead)
            return Decide(RV::Outcome::Invalid, "the sleep or wakeup watch is dead: no rest can be carried out");
        if (w == coop::sleep_rest::WatchState::Pending) return;  // the bound names the wait
    }
    V::Write(V::Field::Sleep, kHoldNeed);
    if (SLP::SetDreamProbability(0.f)) g_dreamHeld = true;
    void* const bed = SLP::FindBed();
    if (!bed) return Decide(RV::Outcome::Invalid, "no bed_C in the world");
    const bool called = SLP::CallSleep(bed);
    if (!called || !SLP::IsSleeping()) return Decide(RV::Outcome::Invalid, "the host's sleep call was refused");
    g_hold = true;
    const float d = SLP::GetGlobalTimeDilation();
    const bool clientAwake = SS::PeerState(g_slot) == SleepPeerState::Awake;
    UE_LOGI("[SLEEP-SPEEDUP] %s host asleep: world dilation %.2f, client %s", Label(), d,
            clientAwake ? "awake" : "not awake");
    if (!clientAwake) return Decide(RV::Outcome::Invalid, "the client was not awake when the host slept");
    if (g_mode == Mode::Host) {
        if (Near(d, kNative, kFactorTol))
            return Decide(RV::Outcome::Pass, "the world read %.2f right after the host's sleep, the client awake", d);
        return Decide(RV::Outcome::Fail, "the world read %.2f right after the host's sleep, the game's is %.0f", d,
                      kNative);
    }
    if (!Near(d, 1.f, kOneTol))
        return Decide(RV::Outcome::Fail, "the host's sleep sped the world up to %.2f while the client was awake", d);
    if (ArmOf() == Arm::RowChange) return HostSetRow("host", Step::HostRowWaitHost, "the row's change to host");
    Enter(Step::HostWaitClientRest, "the client's rest", kPeerBound);
}

void HostWaitClientRest() {
    if (SS::PeerState(g_slot) == SleepPeerState::Awake) return;
    const float d = SLP::GetGlobalTimeDilation();
    UE_LOGI("[SLEEP-SPEEDUP] %s host: the client's rest state rose, world dilation %.2f", Label(), d);
    if (g_mode == Mode::Never) {
        if (!Near(d, 1.f, kOneTol))
            return Decide(RV::Outcome::Fail, "both asleep and the world read %.2f, no sleep should speed it up", d);
        if (ArmOf() == Arm::Modes) return Decide(RV::Outcome::Pass, "both asleep and the world read %.2f", d);
    } else if (!Near(d, kNative, kFactorTol)) {
        return Decide(RV::Outcome::Fail, "everyone asleep and the world read %.2f, the game's is %.0f", d, kNative);
    }
    PS::Mark(kWakeSignal);  // the edge is read: the client may wake (modes) or leave (leave)
    if (ArmOf() == Arm::Leave) return Enter(Step::HostWaitClientLeave, "the client to leave", kPeerBound);
    Enter(Step::HostWaitClientWake, "the client's wake", kPeerBound);
}

// The client has left (its roster row is gone, which every subscriber of the leave has seen): nobody is connected, so
// under `everyone` the host's sleep is the game's, and under `never` it still rests at 1.
void HostWaitClientLeave() {
    if (coop::roster_ledger::Occupied(g_slot)) return;
    const float d = SLP::GetGlobalTimeDilation();
    const float want = g_mode == Mode::Never ? 1.f : kNative;
    UE_LOGI("[SLEEP-SPEEDUP] %s host: the client left, world dilation %.2f", Label(), d);
    if (Near(d, want, g_mode == Mode::Never ? kOneTol : kFactorTol))
        return Decide(RV::Outcome::Pass, "the client left both asleep and the world read %.2f", d);
    Decide(RV::Outcome::Fail, "the client left both asleep and the world read %.2f, not %.0f", d, want);
}

// The row set for this session only, then the wait for its notification: the host's sleep is declared again by then.
void HostSetRow(const char* value, Step next, const char* waitingFor) {
    g_rowNotifiedBase = g_rowNotified;
    if (coop::config::SetForSession(::coop::config_registry::rows::world_sleep_speedup, value) ==
        coop::config::SetResult::Refused)
        return Decide(RV::Outcome::Invalid, "world.sleep_speedup could not be set to %s for this session", value);
    Enter(next, waitingFor, kPeerBound);
}

void HostRowWaitHost() {
    if (g_rowNotified == g_rowNotifiedBase) return;
    const float d = SLP::GetGlobalTimeDilation();
    UE_LOGI("[SLEEP-SPEEDUP] %s host: the row is host, world dilation %.2f", Label(), d);
    if (!Near(d, kNative, kFactorTol))
        return Decide(RV::Outcome::Fail, "the row went to host mid-sleep and the world read %.2f, not %.0f", d, kNative);
    HostSetRow("everyone", Step::HostRowWaitEveryone, "the row's change back to everyone");
}

void HostRowWaitEveryone() {
    if (g_rowNotified == g_rowNotifiedBase) return;
    const float d = SLP::GetGlobalTimeDilation();
    UE_LOGI("[SLEEP-SPEEDUP] %s host: the row is everyone again, world dilation %.2f", Label(), d);
    if (Near(d, 1.f, kOneTol))
        return Decide(RV::Outcome::Pass, "the row went host and back to everyone mid-sleep: %.2f", d);
    Decide(RV::Outcome::Fail, "the row went back to everyone, the client awake, and the world read %.2f, not 1", d);
}

void HostWaitClientWake() {
    if (SS::PeerState(g_slot) != SleepPeerState::Awake) return;
    const float d = SLP::GetGlobalTimeDilation();
    UE_LOGI("[SLEEP-SPEEDUP] %s host: the client's rest state fell, world dilation %.2f", Label(), d);
    if (Near(d, 1.f, kOneTol))
        return Decide(RV::Outcome::Pass, "the world read %.2f once the client woke, the host asleep", d);
    Decide(RV::Outcome::Fail, "the client woke and the world read %.2f, not 1", d);
}

void TickHost(coop::net::Session* s) {
    switch (g_step) {
    case Step::WaitJoin:
        for (int slot = 1; slot < static_cast<int>(coop::net::kMaxPeers) && g_slot < 0; ++slot)
            if (s->IsSlotWorldReady(slot) && coop::prop_snapshot::IsBracketClosed(slot)) g_slot = slot;
        if (g_slot < 0) return;
        UE_LOGI("[SLEEP-SPEEDUP] %s host: slot %d's join is over; waiting for no event, then to bed", Label(), g_slot);
        // After sleep_rest's own subscription (its Install ran at world-up), so its answer to a change comes first.
        coop::config::Subscribe(::coop::config_registry::rows::world_sleep_speedup, &OnRowNotified);
        Enter(Step::Quiet, "no active event", kQuietBound);
        return;
    case Step::Quiet:               HostGoToBed(); return;
    case Step::HostWaitClientRest:  HostWaitClientRest(); return;
    case Step::HostWaitClientWake:  HostWaitClientWake(); return;
    case Step::HostWaitClientLeave: HostWaitClientLeave(); return;
    case Step::HostRowWaitHost:     HostRowWaitHost(); return;
    case Step::HostRowWaitEveryone: HostRowWaitEveryone(); return;
    case Step::Ending:              Ending(true); return;
    default: return;
    }
}

// ---- the client -------------------------------------------------------------------------------

void ClientGoToBed() {
    int32_t events = -1;
    if (!AE::ReadCount(events) || events > 0) return;  // the bound names the wait
    V::Write(V::Field::Sleep, kHoldNeed);
    ue_wrap::FVector from{};
    void* const body = coop::players::Registry::Get().Local();
    void* const bed = body && ue_wrap::engine::TryGetActorLocation(body, from) ? SLP::NearestBed(from) : nullptr;
    const char* why = !bed ? "no bed_C in the world, or no local body to measure from"
                      : !SLP::CallSleep(bed) || !SLP::IsSleeping() ? "the client's sleep call was refused"
                                                                   : nullptr;
    if (why) {
        Say(RV::Outcome::Invalid, "%s", why);
        g_step = Step::Done;
        return;
    }
    g_hold = true;
    UE_LOGI("[SLEEP-SPEEDUP] %s client: in bed", Label());
    Enter(Step::ClientResting, "the host's signal", kPeerBound + kPeerBound);
}

void ClientResting(coop::net::Session* s) {
    if (!Told() && SS::HostAsleepNow()) return;
    if (ArmOf() == Arm::Leave) {  // the host has read both asleep: this client leaves the session, still asleep
        UE_LOGI("[SLEEP-SPEEDUP] %s client: the host has read; leaving the session", Label());
        g_hold = false;
        g_step = Step::Done;
        if (s->running()) coop::net_pump::FleeAfterNativeMenuTravel(*s, "sleep speed-up drill: the client leaves asleep");
        return;
    }
    UE_LOGI("[SLEEP-SPEEDUP] %s client: the host has read; waking", Label());
    BeginEnding(false);
}

void TickClient(coop::net::Session* s) {
    switch (g_step) {
    case Step::WaitJoin:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        g_mode = ReadMode();
        UE_LOGI("[SLEEP-SPEEDUP] client: joined; the host's world.sleep_speedup reads %s", Label());
        // The host's sleep alone is read (the host mode), or the row's change mid-sleep, the client awake throughout.
        if (g_mode == Mode::Host || ArmOf() == Arm::RowChange) {
            g_step = Step::Done;
            return;
        }
        Enter(Step::WaitHost, "the host to sleep", kJoinBound);
        return;
    case Step::WaitHost:
        if (Told()) {  // the host decided before this client could sleep (a red arm): nothing to measure here
            g_step = Step::Done;
            return;
        }
        if (!SS::HostAsleepNow()) return;
        Enter(Step::Quiet, "no active event", kQuietBound);
        return;
    case Step::Quiet:
        if (Told()) {
            g_step = Step::Done;
            return;
        }
        ClientGoToBed();
        return;
    case Step::ClientResting: ClientResting(s); return;
    case Step::Ending:        Ending(false); return;
    default: return;
    }
}

}  // namespace

void Tick(coop::net::Session* session) {
    if (!On() || !session || !session->running() || !SLP::EnsureResolved() || g_step == Step::Done) return;
    const bool host = session->role() == coop::net::Role::Host;
    if (!g_armed) {
        g_armed = true;
        if (!PS::Begin(kTag)) {  // says its INVALID line
            g_step = Step::Done;
            return;
        }
        AE::EnsureResolved();
        if (host) g_mode = ReadMode();
        Enter(Step::WaitJoin, "the join", kJoinBound);
        UE_LOGI("[SLEEP-SPEEDUP] %s %s armed, red sleep_speedup_rule_off=%d", Label(), host ? "host" : "client",
                coop::config::ResolveFlag(::coop::config_registry::rows::sleep_speedup_rule_off) ? 1 : 0);
    }
    if (g_hold && SLP::IsSleeping()) V::Write(V::Field::Sleep, kHoldNeed);
    if (Clock::now() > g_deadline) {
        // A wake that does not come after the verdict is said changes nothing: only a verdict not yet said is INVALID.
        if (g_step == Step::Ending) UE_LOGW("[SLEEP-SPEEDUP] %s timed out waiting for %s", Label(), g_waitingFor);
        else Say(RV::Outcome::Invalid, "timed out waiting for %s", g_waitingFor);
        RestoreDream();
        g_hold = false;
        g_step = Step::Done;
        return;
    }
    if (host) TickHost(session);
    else TickClient(session);
}

void OnDisconnect() {
    // The leave arm is the client's leaving: the host's last client going is the edge it waits for, not an end.
    if (ArmOf() == Arm::Leave) return;
    RestoreDream();
    g_hold = false;
    g_step = Step::WaitJoin;
    g_armed = false;
    g_slot = -1;
}

}  // namespace coop::dev::sleep_speedup_drill

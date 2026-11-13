// harness/autotest/autotest_seat_ragdoll.cpp -- TEST-ONLY: a device's claim when its user ragdolls.
//
// One peer (the client, or the host with dev.seat_ragdoll_who=host) enters a device the way the game's enter ends
// (setActiveInterface): the laptop, or with dev.seat_ragdoll_device=desk or sat the desk's coordinates screen or the
// SAT console. Once the other peer reads its claim, it ragdolls as a faint does (ragdollMode(true, passOut, false)).
// Arms: dev.seat_ragdoll_ping (at the desk) starts a ping first, dev.seat_ragdoll_kill dies of a lethal hit instead (a
// client dying mid-ping), dev.seat_ragdoll_exit leaves by the game's own exit (with the ping, the host acting). The host
// measures the hold and says `rig: VERDICT seat-ragdoll PASS|FAIL|INVALID`, every verdict decided by an event: the hold's
// state once the reconcile has run twice after the host saw the knock-down (the exit arm: after the actor's signal), a
// claim still held being a FAIL; the exit with a ping decides at the ping run's own end (the hold dropping before it
// fails). A bound only says INVALID. PASS: the hold is free. The red: dev.occupancy_pinger_down_off on the host, with the
// ping arm, must FAIL.
// Gated by VOTVCOOP_RUN_SEAT_RAGDOLL; the peers' folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/death_state_probe.h"
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/desk_input_sync.h"
#include "coop/interactables/device_occupancy.h"
#include "coop/player/players_registry.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/device_screen.h"
#include "ue_wrap/devices/laptop_widget.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <string>

namespace harness::autotest {
namespace {

namespace PS = harness::autotest::peer_signal;
namespace R = ue_wrap::reflection;
using disc_drill::OnGameThread;

constexpr const char* kTag = "seat-ragdoll";
constexpr int kStepSec = 120;
constexpr int kBackstopSec = 10;  // a bound says INVALID only: every verdict is decided by an event

std::string Device() {
    return coop::config::ResolveEnum(coop::config_registry::rows::seat_ragdoll_device);
}
const wchar_t* Key() { return Device() == "desk" ? L"desk" : Device() == "sat" ? L"sat" : L"laptop"; }

// The device's widget: the laptop's, or the first live instance of the desk's or the SAT console's shared widget.
void* DeviceWidget() {
    if (Device() == "laptop") return ue_wrap::laptop_widget::Widget();
    void* const cls = R::FindClass(Device() == "desk" ? L"ui_consolesAtlas_C" : L"ui_console_C");
    if (!cls) return nullptr;
    void* found = nullptr;
    ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        void*& out = *static_cast<void**>(c);
        if (!out && obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) out = obj;
    }, &found);
    return found;
}

bool EnterDevice() {
    void* player = coop::players::Registry::Get().Local();
    void* widget = DeviceWidget();
    void* const fn = player ? R::FindDispatchFunctionCached(R::ClassOf(player), L"setActiveInterface") : nullptr;
    if (!fn || !widget) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"activeInterface", widget);
    f.Set<bool>(L"isActiveINterface3D", true);
    return ue_wrap::Call(player, f);
}

unsigned Holder() {
    unsigned h = 0xFF;
    OnGameThread([&] { h = coop::device_occupancy::HolderOf(Key()); });
    return h;
}

bool InInterface() {
    bool in = false;
    OnGameThread([&] {
        void* p = coop::players::Registry::Get().Local();
        in = p && ue_wrap::device_screen::ReadActiveInterface(p) != nullptr;
    });
    return in;
}

void LogState(const char* who, int t) {
    uint8_t ping = 0xFF;
    OnGameThread([&] { ping = coop::desk_input_sync::PingActiveSlot(); });
    UE_LOGI("seat-ragdoll: %hs t=%d holder=%u inInterface=%d pinger=%u", who, t, Holder(), InInterface() ? 1 : 0, ping);
}

// Which peer enters and ragdolls (dev.seat_ragdoll_who, the client by default); the other watches.
// dev.seat_ragdoll_ping: the actor (the host) starts a ping on the desk before it ragdolls, as a player who faints
// mid-ping would; its machine keeps running after the faint.
bool Ping() { return coop::config::ResolveFlag(coop::config_registry::rows::seat_ragdoll_ping); }

// dev.seat_ragdoll_exit: the actor leaves the device by the game's own exit instead of a ragdoll. With the ping
// arm the desk stays the pinger's while its machine runs, so the other peer's enter must be refused.
bool ExitOnly() { return coop::config::ResolveFlag(coop::config_registry::rows::seat_ragdoll_exit); }

// dev.seat_ragdoll_kill: the actor dies of a lethal hit instead of ragdolling, as a client that dies mid-ping does.
bool Kill() { return coop::config::ResolveFlag(coop::config_registry::rows::seat_ragdoll_kill); }

const char* Outcome() { return Kill() ? "died of a lethal hit" : ExitOnly() ? "left by the game's exit" : "ragdolled as a faint"; }

bool HostActs() { return coop::config::ResolveEnum(coop::config_registry::rows::seat_ragdoll_who) == "host"; }

// HOST: what the actor leaves held. Every verdict is an event, and a bound only says INVALID.
// The no-change point is the second reconcile pass after the departure was seen: the first can run in the tick that is
// still taking the actor's own release, and after the second nothing can free the claim that has not already (a ping's
// hold is re-asserted by the reconcile, a plain claim is released by its peer's departure, sent before it). A knock-down is
// seen by the host's own read (the one the reconcile makes); an exit by the actor's signal. The hold's state then is the
// verdict, a held claim included. The exit arm with a ping decides after that, at the ping run's own end (below).
// Says INVALID and returns false when an event never came.
bool PassesAfter(uint32_t seen, int passes) {
    return PS::WaitFor("the reconcile passes after the departure", kBackstopSec, [seen, passes] {
        uint32_t now = 0;
        OnGameThread([&] { now = coop::device_occupancy::ReconcilePasses(); });
        return now >= seen + static_cast<uint32_t>(passes);
    });
}

// The exit with a ping: the pinger's machine keeps the desk, so the hold must stay held to the ping run's own end. The
// events are the hold dropping (FAIL) and the run ending (PASS); the run is long, so its bound is a step's.
bool HoldsToPingEnd(char& freed) {
    bool dropped = false;
    // The hold and the ping are read in one game-thread step, the hold first: a hold already free while the ping still runs
    // is a premature drop (FAIL). Inside the one poll that sees the run end, a free hold is the legitimate release (a
    // reliable message a moment after the ping's end) or a drop in the same window: that cannot be decided, so it is '?'
    // and the verdict is INVALID. A drop earlier is read at its own poll.
    bool endedFree = false;
    const bool ended = PS::WaitFor("the ping run's end", kStepSec, [&dropped, &endedFree] {
        uint8_t ping = 0xFF;
        bool free = false;
        OnGameThread([&] {
            free = coop::device_occupancy::HolderOf(Key()) == 0xFF;
            ping = coop::desk_input_sync::PingActiveSlot();
        });
        if (ping == 0xFF) {
            endedFree = free;
            return true;
        }
        if (free) dropped = true;
        return dropped;
    });
    freed = dropped ? '1' : endedFree ? '?' : '0';
    return ended;
}

bool MeasureHold(unsigned actorSlot, char& freed) {
    if (!ExitOnly() &&
        !PS::WaitFor("the actor's knock-down seen by the host", kBackstopSec, [actorSlot] {
            bool down = false;
            OnGameThread([&] { down = coop::device_occupancy::KnockedDown(static_cast<uint8_t>(actorSlot)); });
            return down;
        }))
        return false;
    uint32_t seen = 0;
    OnGameThread([&] { seen = coop::device_occupancy::ReconcilePasses(); });
    if (!PassesAfter(seen, 2)) return false;
    UE_LOGI("seat-ragdoll: the reconcile ran twice after the departure (from pass %u); the hold reads holder=%u", seen,
            Holder());
    if (ExitOnly() && Ping()) return HoldsToPingEnd(freed);
    freed = Holder() == 0xFF ? '1' : '0';
    return true;
}

std::string Act(const char* me) {
    bool entered = false;
    OnGameThread([&] { entered = EnterDevice(); });
    UE_LOGI("seat-ragdoll: %hs entered the %hs (call=%d)", me, Device().c_str(), entered ? 1 : 0);
    if (!PS::WaitFor("this peer's claim", kBackstopSec, [] { return InInterface() && Holder() != 0xFF; })) return "";
    LogState(me, 0);
    if (Ping()) {
        bool wrote = false;
        OnGameThread([&] {
            ue_wrap::console_desk::Scalars sc;
            if (ue_wrap::console_desk::EnsureResolved() && ue_wrap::console_desk::ReadScalars(sc)) {
                sc.coordIsPing = true;
                wrote = ue_wrap::console_desk::WriteScalars(sc, ue_wrap::console_desk::kPaintNone);
            }
        });
        // The attribution is the host's: a client actor leaves the wait to the watcher (the host), which reads it.
        if (HostActs() && !PS::WaitFor("the desk's ping machine attributed", kBackstopSec, [] {
                uint8_t slot = 0xFF;
                OnGameThread([&] { slot = coop::desk_input_sync::PingActiveSlot(); });
                return slot != 0xFF;
            }))
            return "";
        UE_LOGI("seat-ragdoll: %hs started a ping (write=%d)", me, wrote ? 1 : 0);
    }
    PS::Mark("actor_in");
    std::string saw;
    if (!PS::AwaitText("observer_saw_claim", saw, kStepSec)) return "";
    // A joiner is protected for a while (startInvinc) and a lethal hit lands nowhere then: the same precondition the
    // death drill waits on, and the death itself is read back below, so a hit that killed nobody is INVALID, never a FAIL.
    if (Kill() && !PS::WaitFor("a player a lethal hit can kill", kStepSec, [] {
            DeathSnapshot s;
            OnGameThread([&] { s = ReadDeathState(); });
            return s.havePawn && s.haveState && s.haveCanRagdoll && s.health > 0.f && s.canRagdoll && !s.dead &&
                   !s.startInvinc && !s.immortal && s.inGameplay;
        }))
        return "";
    bool rag = false;
    OnGameThread([&] {
        void* p = coop::players::Registry::Get().Local();
        if (!p) return;
        if (Kill()) {
            // The death drill's lethal hit: twice the max health, the game's own Add Player Damage.
            float maxHp = 100.f;
            if (!ue_wrap::vitals::Read(ue_wrap::vitals::Field::MaxHealth, &maxHp) || maxHp <= 0.f) maxHp = 100.f;
            rag = ue_wrap::engine::InvokeAddPlayerDamage(p, maxHp * 2.f, /*blood=*/true);
        } else {
            rag = ExitOnly() ? ue_wrap::device_screen::ForceExitInterface(p)
                             : ue_wrap::engine::SetMainPlayerRagdollMode(p, true, true, false);
        }
    });
    UE_LOGI("seat-ragdoll: %hs %hs (call=%d)", me, Outcome(), rag ? 1 : 0);
    if (Kill() && !PS::WaitFor("the lethal hit's death", kBackstopSec, [] {
            DeathSnapshot s;
            OnGameThread([&] { s = ReadDeathState(); });
            return s.dead;
        }))
        return "";
    PS::Mark("actor_ragdolled");
    if (!HostActs()) {
        PS::Await("observer_done", kStepSec);  // the actor stays down (or out) while the host measures
        LogState(me, 1);
        return "";
    }
    // The host is the actor: it measures its own table, the client having said what it saw of the claim.
    char freed = '0';
    const bool ok = MeasureHold(0, freed);
    LogState(me, 1);
    return ok ? std::string(saw == "1" ? "1" : "0") + " " + freed : std::string();
}

// The watcher's prelude: the actor's claim seen here, and the client's ping attributed when the host watches it. It says
// what it saw of the claim; the host measures what the actor leaves.
bool WatchClaim(const char* me, unsigned actorSlot) {
    if (!PS::Await("actor_in", kStepSec)) return false;
    if (!PS::WaitFor("the actor's claim here", kBackstopSec, [actorSlot] { return Holder() == actorSlot; })) return false;
    if (Ping() && !HostActs() &&
        !PS::WaitFor("the client's ping attributed", kBackstopSec, [actorSlot] {
            uint8_t slot = 0xFF;
            OnGameThread([&] { slot = coop::desk_input_sync::PingActiveSlot(); });
            return slot == actorSlot;
        }))
        return false;
    LogState(me, 0);
    PS::Mark("observer_saw_claim", "1");
    return true;
}

// "claimed freed": the claim the watcher saw (0 or 1), and the hold the actor left (0, 1, or ? when the hold was free in the
// poll that saw the ping run end, which cannot be decided).
void Judge(const std::string& r) {
    // Empty: a wait that never saw its event said INVALID itself (peer_signal), so no second line.
    if (r.size() != 3 || r[1] != ' ') return;
    const int claimed = r[0] == '1';
    if (r[2] == '?') {
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid,
                               "the hold was free in the same poll that saw the ping run end: a release after the run "
                               "and a drop in that window cannot be told apart");
        return;
    }
    const int freed = r[2] == '1';
    // The exit arm mid-ping: the pinger's machine keeps the desk, so nothing frees it.
    const bool pass = ExitOnly() && Ping() ? claimed && !freed : claimed && freed;
    coop::rig_verdict::Say(kTag, pass ? coop::rig_verdict::Outcome::Pass : coop::rig_verdict::Outcome::Fail,
                           "%s %s at the %s: the claim was seen=%d, the hold %s", HostActs() ? "the host" : "the client",
                           Kill() ? "died" : ExitOnly() ? "left" : "ragdolled", Device().c_str(), claimed,
                           freed ? "is free" : "is still held");
}

void RunClient() {
    if (!disc_drill::HostPuppetUp()) return;
    PS::Mark("client_ready");
    if (!PS::Await("host_ready", kStepSec)) return;
    if (HostActs()) WatchClaim("CLIENT", 0);
    else Act("CLIENT");
    PS::Await("host_done", kStepSec);
    UE_LOGI("seat-ragdoll: DONE on client");
}

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    // A client acting: its puppet stands here and has sent an alive pose, so its death is an edge from a living player
    // (a death already in a first pose is raised too, but the arm is meant to measure the hit it lands).
    if (!HostActs() && !PS::WaitFor("the client's puppet, alive", 120, [] {
            bool alive = false;
            OnGameThread([&] { alive = coop::players::Registry::Get().LifeOf(1) == coop::players::PuppetLife::Alive; });
            return alive;
        }))
        return;
    PS::Mark("host_ready");
    std::string r;
    if (HostActs()) {
        r = Act("HOST");
    } else if (WatchClaim("HOST", 1) && PS::Await("actor_ragdolled", kStepSec)) {
        char freed = '0';
        if (MeasureHold(1, freed)) r = std::string("1 ") + freed;
        PS::Mark("observer_done");
    }
    PS::Mark("host_done");
    Judge(r);
    UE_LOGI("seat-ragdoll: DONE on host");
}

}  // namespace

DWORD WINAPI SeatRagdollThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (!IsClientRole()) RunHost();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest

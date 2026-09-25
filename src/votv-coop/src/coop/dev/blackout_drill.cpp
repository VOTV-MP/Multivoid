// coop/dev/blackout_drill.cpp -- see coop/dev/blackout_drill.h.

#include "coop/dev/blackout_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/door_approach.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"
#include "coop/world/event_fire_sync.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/door.h"
#include "ue_wrap/devices/lightswitch.h"
#include "ue_wrap/devices/power_control.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::blackout_drill {
namespace {

namespace R   = ue_wrap::reflection;
namespace PC  = ue_wrap::power_control;
namespace LS  = ue_wrap::lightswitch;
namespace DR  = ue_wrap::door;
namespace E   = ue_wrap::engine;
namespace GT  = ue_wrap::game_thread;
namespace DIR = coop::director;
namespace EF  = coop::event_fire_sync;
using Clock = std::chrono::steady_clock;

// A client whose copies have not all read the blackout this long after its join ends the run.
constexpr auto kCutBound = std::chrono::seconds(60);
constexpr auto kReadEvery = std::chrono::milliseconds(250);
// A client's press of the named door, run on the host, opens it within this or the leg fails.
constexpr auto kPressBound = std::chrono::seconds(20);
// The control's press, run on the host with the power on, must leave the door shut this long.
constexpr auto kControlWindow = std::chrono::seconds(5);
constexpr float kLegReachCm = 60.f;  // at the door's approach point, not merely near it

void*   g_panel = nullptr;
int32_t g_panelIdx = -1;
bool    g_prepared = false;  // host: the panel's groups lit and its blackout doors shut
bool    g_fired = false;     // host: the solar fire went out
bool    g_done = false;
int     g_sawLit = 0;        // client: the most of the panel's groups it read on at one read
ue_wrap::CachedObjRef g_door;  // the door blackout_drill_door names, once found in this world
bool    g_doorSought = false;  // this world was walked for it (a miss is said once)
bool    g_gridAfter = false;   // host: the grid was said once its breakers read cut
// This copy read the whole blackout once: a door it opened may autoclose later, and the leg goes on.
bool    g_reached = false;
Clock::time_point g_since{};
Clock::time_point g_nextRead{};

// The client's door leg runs on its own thread, since the director's walk blocks. The thread writes into its
// own record; the game thread holds the current one and lets go of it at a session's end, when the leg is
// cancelled and its walk, which carries the director's epoch of that session, ends.
enum LegVerdict : int { kLegRunning, kLegOpened, kLegShut, kLegNoWalk };
struct Leg {
    std::atomic<bool> cancel{false};
    std::atomic<int>  verdict{kLegRunning};
    void*    door = nullptr;
    int32_t  doorIdx = -1;
    uint32_t walks = 0;        // the director's walk epoch when the leg started
    bool     powerOn = false;  // the control: the press must leave the door shut
};
std::shared_ptr<Leg> g_leg;  // game thread only

long long MsSince(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

// A bool member of `obj` by name: 1 or 0, -1 unread. The drill's diagnostic lines only, a few a run.
int Flag(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask) || off < 0) return -1;
    return (*(static_cast<const uint8_t*>(obj) + off) & mask) ? 1 : 0;
}

// Each entry once: the panel's processKeys adds what the save's keys name to its lists without a clear,
// so after a load a group or a door can stand in them more than once.
void Distinct(std::vector<void*>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

// The power panel, or null while there is none. It is one map actor: one walk of the object array finds
// it, and its slot and serial keep it.
void* Panel() {
    if (!PC::EnsureResolved()) return nullptr;
    if (!g_panel || !R::IsLiveByIndex(g_panel, g_panelIdx)) {
        g_panel = nullptr;
        const int32_t n = R::NumObjects();
        for (int32_t i = 0; i < n && !g_panel; ++i) {
            void* o = R::ObjectAt(i);
            if (!o || !R::IsLive(o) || !PC::IsPowerControl(o)) continue;
            if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;
            g_panel = o;
            g_panelIdx = R::InternalIndexOf(o);
        }
    }
    return g_panel;
}

// Whether the blackout opens this listed door: the panel's powerChanged skips one that ignores a blackout.
// An unread flag counts as one it opens, the drill's stricter reading.
bool BlackoutOpens(void* door) {
    bool ignores = false;
    return !DR::TryReadIgnoresBlackout(door, ignores) || !ignores;
}

// What the blackout reaches on this copy: the breaker mask (-1 unread), how many of the panel's groups
// read on, and how many of the listed doors the blackout opens read open, of how many.
struct Reach {
    int mask = -1;
    int groups = 0, lit = 0;
    int doors = 0, open = 0;
};
Reach Read() {
    Reach r{};
    void* p = Panel();
    if (!p) return r;
    uint8_t mask = 0;
    if (PC::ReadPress(p, mask)) r.mask = mask;
    std::vector<void*> roots, doors;
    if (LS::EnsureResolved() && PC::ReadLightRoots(p, roots)) {
        Distinct(roots);
        for (void* root : roots) {
            bool on = false;
            if (!LS::TryReadActive(root, on)) continue;
            ++r.groups;
            r.lit += on ? 1 : 0;
        }
    }
    if (DR::EnsureResolved() && PC::ReadBlackoutDoors(p, doors)) {
        Distinct(doors);
        for (void* door : doors) {
            bool open = false;
            if (!BlackoutOpens(door) || !DR::TryReadOpen(door, open)) continue;
            ++r.doors;
            r.open += open ? 1 : 0;
        }
    }
    return r;
}

// The door blackout_drill_door names, or null: none named, or none of that name in this world (said once).
void* NamedDoor() {
    static const std::string name = coop::config::ResolveString(::coop::config_registry::rows::blackout_drill_door);
    if (name.empty() || !DR::EnsureResolved()) return nullptr;
    if (void* d = g_door.Get()) return d;
    if (g_doorSought) return nullptr;
    g_doorSought = true;
    const std::wstring wname(name.begin(), name.end());
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (o && R::IsLive(o) && DR::IsDoor(o) && R::NameEquals(R::NameOf(o), wname.c_str())) {
            g_door.Set(o);
            return o;
        }
    }
    UE_LOGW("[BLACKOUT-DRILL] no door named '%s' in this world -- the door leg is skipped", name.c_str());
    return nullptr;
}

// One door's flags, for the grid lines.
void LogDoor(const char* what, void* d) {
    UE_LOGI("[BLACKOUT-DRILL]   %s %ls key='%ls': isOpened=%d isMoving=%d active=%d ignoreBlackout=%d jammed=%d "
            "superClosed=%d", what, R::ToString(R::NameOf(d)).c_str(), DR::GetKeyString(d).c_str(),
            Flag(d, L"isOpened"), Flag(d, L"isMoving"), Flag(d, L"active"), Flag(d, L"ignoreBlackout"),
            Flag(d, L"jammed"), Flag(d, L"superClosed"));
}

// Host: what decides whether the blackout opens the listed doors -- the panel's buttonsVisibility sets
// the power, whose powerChanged opens each listed door that does not ignore a blackout, only while the
// panel is not disabled and its generators are fine -- and each listed door's own flags, and the named
// door's.
void LogGrid(const char* when) {
    const int32_t gmOff = R::FindPropertyOffset(R::ClassOf(g_panel), L"gamemode");
    void* gm = gmOff >= 0 ? *reinterpret_cast<void**>(static_cast<char*>(g_panel) + gmOff) : nullptr;
    UE_LOGI("[BLACKOUT-DRILL] host grid %s: panel disabled=%d areGensFine=%d, gamemode usesp_light=%d", when,
            Flag(g_panel, L"disabled"), Flag(g_panel, L"areGensFine"), Flag(gm, L"usesp_light"));
    std::vector<void*> doors;
    if (DR::EnsureResolved() && PC::ReadBlackoutDoors(g_panel, doors)) {
        Distinct(doors);
        for (void* d : doors) LogDoor("listed door", d);
    }
    if (void* door = NamedDoor()) LogDoor("named door", door);
}

// The control arm: no fire, and the named door, inactive with the power on, must stay shut to the press.
bool Control() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::blackout_drill_control);
    return s;
}

// Host: light the panel's groups and shut the listed doors the blackout opens through their own verbs, so
// the blackout's runTrigger and doorOpen each change a state the lanes carry. A save's groups can already
// be off and its doors already open, and then the blackout moves nothing. The named door is shut too.
void Prepare() {
    std::vector<void*> roots, doors;
    int lit = 0, shut = 0, opens = 0;
    if (LS::EnsureResolved() && PC::ReadLightRoots(g_panel, roots)) {
        Distinct(roots);
        for (void* root : roots) lit += LS::ApplyGroupState(root, true) ? 1 : 0;
    }
    if (DR::EnsureResolved() && PC::ReadBlackoutDoors(g_panel, doors)) {
        Distinct(doors);
        for (void* door : doors) {
            if (!BlackoutOpens(door)) continue;
            ++opens;
            shut += DR::CallDoorClose(door, true) ? 1 : 0;
        }
    }
    UE_LOGI("[BLACKOUT-DRILL] host lit %d of %zu light group(s) and shut %d of the %d listed door(s) the blackout "
            "opens (of %zu listed)%s", lit, roots.size(), shut, opens, doors.size(),
            opens == 0 ? " -- every listed door ignores a blackout: its door writer opens none in this world" : "");
    // The door leg: a door the blackout itself opens proves nothing about a press.
    if (void* door = NamedDoor()) {
        const bool listed = std::find(doors.begin(), doors.end(), door) != doors.end();
        UE_LOGI("[BLACKOUT-DRILL] host shut the named door (%s)%s%s", DR::CallDoorClose(door, true) ? "yes" : "NO",
                listed && BlackoutOpens(door) ? " -- the blackout itself opens this door" : "",
                BlackoutOpens(door) ? "" : " -- it ignores a blackout, so no press opens it by hand");
    }
    LogGrid(Control() ? "with the power on" : "before the fire");
}

// Host: the press leg wants the named door inactive, as a keypad's lock or a trigger leaves one. After the fire
// it says whether the blackout left it so; otherwise the drill makes it so through the door's own runTrigger 2.
void MakeNamedDoorInactive(const char* when) {
    void* door = NamedDoor();
    bool active = false;
    if (!door || !DR::TryReadActive(door, active)) return;
    if (!active) {
        UE_LOGI("[BLACKOUT-DRILL] host: the named door is inactive already %s", when);
        return;
    }
    const bool made = DR::CallRunTrigger(door, door, 2) && DR::TryReadActive(door, active) && !active;
    UE_LOGI("[BLACKOUT-DRILL] host: the named door was active %s; the drill's runTrigger 2 made it inactive (%s)",
            when, made ? "yes" : "NO");
}

// Whether the named door reads open on this copy: -1 none named or unread.
int NamedDoorOpen() {
    void* door = NamedDoor();
    bool open = false;
    return door && DR::TryReadOpen(door, open) ? (open ? 1 : 0) : -1;
}

void Done(const char* line) {
    g_done = true;
    UE_LOGI("[BLACKOUT-DRILL] %s", line);
}

bool LegDoorLive(void* door, int32_t idx) { return door && R::IsLiveByIndex(door, idx); }

// A walk's budget from its own route at a slow walk, so a far door is not a timeout and a hang is.
int WalkSeconds(float routeCm) { return std::clamp(static_cast<int>(routeCm / 100.f) + 60, 90, 900); }

// Client, off the game thread: walk to the named door's approach with the director, press the door --
// the press runs on the host, which bounds a client's reach -- and read whether it opened. Every engine
// read and call is marshalled to the game thread.
DWORD WINAPI LegThread(LPVOID arg) {
    const std::shared_ptr<Leg> leg = *std::unique_ptr<std::shared_ptr<Leg>>(static_cast<std::shared_ptr<Leg>*>(arg));
    void* const door = leg->door;
    const int32_t idx = leg->doorIdx;
    // The session the leg ran in has ended: its drill let go of it, or the director's walks moved past it.
    auto cancelled = [&leg] {
        return leg->cancel.load(std::memory_order_acquire) || DIR::WalkEpoch() != leg->walks;
    };
    DIR::DoorApproach best;
    const int picked = GT::RunAndWait([&](std::atomic<int>& done) {
        void* p = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        const bool ok = p && R::IsLive(p) && E::GetController(p) && E::TryGetActorLocation(p, at) &&
                        LegDoorLive(door, idx) && DIR::PickDoorApproach(p, at, door, true, best);
        done.store(ok ? 1 : 2);
    });
    if (picked != 1) {
        UE_LOGW("[BLACKOUT-DRILL] client: no route to the named door's approach (%d without a route, %d ending "
                "short) -- INCONCLUSIVE", best.noRoute, best.endsFar);
        leg->verdict.store(kLegNoWalk, std::memory_order_release);
        return 0;
    }
    DIR::DirectorGoal goal;
    goal.targetActor = door;
    goal.targetPos = best.target;
    goal.reachCm = kLegReachCm;
    goal.epoch = leg->walks;
    UE_LOGI("[BLACKOUT-DRILL] client walks to the named door's approach, a %.0fcm route, %d s budget", best.len,
            WalkSeconds(best.len));
    DIR::ControlManager mgr;
    DIR::AddWalkToProcesses(mgr, goal);
    mgr.Run(goal, WalkSeconds(best.len));
    if (cancelled()) {
        UE_LOGI("[BLACKOUT-DRILL] client: the door leg ended with its session");
        return 0;
    }
    if (!goal.reached) {
        UE_LOGW("[BLACKOUT-DRILL] client did not reach the named door's approach (%s) -- INCONCLUSIVE",
                goal.failReason);
        leg->verdict.store(kLegNoWalk, std::memory_order_release);
        return 0;
    }
    const int pressed = GT::RunAndWait([&](std::atomic<int>& done) {
        void* me = coop::players::Registry::Get().Local();
        done.store(me && LegDoorLive(door, idx) && DR::CallPress(door, me, DR::kUseAction) ? 1 : 2);
    });
    const Clock::time_point pressedAt = Clock::now();
    const char* const power = leg->powerOn ? "with the power on" : "in the blackout";
    UE_LOGI("[BLACKOUT-DRILL] client pressed the named inactive door %s (dispatched=%d)", power, pressed == 1 ? 1 : 0);
    const auto bound = leg->powerOn ? kControlWindow : kPressBound;
    while (!cancelled() && Clock::now() - pressedAt <= bound) {
        int open = -1;
        GT::RunAndWait([&](std::atomic<int>& done) {
            bool o = false;
            if (LegDoorLive(door, idx) && DR::TryReadOpen(door, o)) open = o ? 1 : 0;
            done.store(1);
        });
        if (open == 1) {
            UE_LOGI("[BLACKOUT-DRILL] client: the named inactive door opened %lld ms after its press %s",
                    MsSince(pressedAt), power);
            leg->verdict.store(kLegOpened, std::memory_order_release);
            return 0;
        }
        ::Sleep(100);
    }
    if (cancelled()) return 0;
    UE_LOGI("[BLACKOUT-DRILL] client: the named inactive door stayed shut %lld s after its press %s",
            static_cast<long long>(bound.count()), power);
    leg->verdict.store(kLegShut, std::memory_order_release);
    return 0;
}

// Client: the door leg's next step. True while the leg holds the drill.
bool DoorLeg(void* door) {
    if (!g_leg) {
        // A door that ignores a blackout stays shut to a press by its own clause, and the flag is the
        // save's, the same on both copies: the leg would measure nothing.
        if (!BlackoutOpens(door)) {
            Done("client DONE -- INCONCLUSIVE: the named door ignores a blackout, so no press opens it by hand");
            return true;
        }
        auto leg = std::make_shared<Leg>();
        leg->door = door;
        leg->doorIdx = R::InternalIndexOf(door);
        leg->walks = DIR::WalkEpoch();
        leg->powerOn = Control();
        auto* arg = new std::shared_ptr<Leg>(leg);
        if (HANDLE h = ::CreateThread(nullptr, 0, &LegThread, arg, 0, nullptr)) {
            ::CloseHandle(h);
            g_leg = std::move(leg);
        } else {
            delete arg;
            Done("client DONE -- INCONCLUSIVE: the door leg's walker did not start");
        }
        return true;
    }
    switch (g_leg->verdict.load(std::memory_order_acquire)) {
    case kLegRunning: return true;
    case kLegNoWalk:  Done("client DONE -- INCONCLUSIVE: the walk to the named door did not arrive"); return true;
    case kLegOpened:
        if (!g_leg->powerOn) return false;  // opened by hand in the blackout: the drill's own DONE follows
        Done("client DONE -- FAIL: an inactive door opened to a press with the power on");
        return true;
    default:  // shut
        Done(g_leg->powerOn ? "client DONE: control -- the inactive door stayed shut to a press with the power on"
                            : "client DONE -- FAIL");
        return true;
    }
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::blackout_drill);
    return s;
}

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || g_done || !session || !session->connected()) return;
    if (Clock::now() < g_nextRead) return;
    g_nextRead = Clock::now() + kReadEvery;
    if (coop::roster::LocalIsHost()) {
        if (!g_fired) {
            if (!session->AnyWorldReadyPeer() || !Panel()) return;
            if (!g_prepared) {
                g_prepared = true;
                Prepare();
                return;  // the fire goes at the next read, its own edges after these
            }
            if (Control()) {
                // The control: no fire, and the named door made inactive with the power on.
                MakeNamedDoorInactive("with the power on");
                Done("host DONE: control -- no fire");
                return;
            }
            const Reach before = Read();
            g_fired = EF::HostFire(EF::FireKind::RunEvent, L"solar", L"None");
            g_since = Clock::now();
            UE_LOGI("[BLACKOUT-DRILL] host FIRED solar (breakers %d, groups lit %d of %d, blackout doors open %d of "
                    "%d before), sent=%d", before.mask, before.lit, before.groups, before.open, before.doors,
                    g_fired ? 1 : 0);
            if (!g_fired) Done("host DONE: the fire was refused -- INCONCLUSIVE");
            return;
        }
        const Reach r = Read();
        if (!g_gridAfter && r.mask == 0) {
            g_gridAfter = true;  // solar() ran whole inside its dispatch: its door verbs are in
            LogGrid("once its breakers read cut");
            MakeNamedDoorInactive("once its breakers read cut");
        }
        if (!g_reached && r.mask == 0 && r.lit == 0 && r.open == r.doors) {
            g_reached = true;
            UE_LOGI("[BLACKOUT-DRILL] host: breakers cut, %d group(s) off, %d blackout door(s) open, %lld ms after "
                    "the fire", r.groups, r.doors, MsSince(g_since));
        }
        // With a named door the host waits for the client's press to open it.
        const int named = NamedDoorOpen();
        if (g_reached && named != 0)
            Done(named == 1 ? "host DONE: the named inactive door open, by the client's press" : "host DONE");
        return;
    }
    if (!coop::net_pump::HasAnnouncedWorldReady() ||
        coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
        return;
    if (g_since == Clock::time_point{}) g_since = Clock::now();
    if (Control()) {
        // The control has no blackout to read: the leg alone, with the power on.
        if (void* door = NamedDoor()) DoorLeg(door);
        else Done("client DONE -- INCONCLUSIVE: the control names no door (blackout_drill_door)");
        return;
    }
    const Reach r = Read();
    if (r.lit > g_sawLit) g_sawLit = r.lit;
    if (!g_reached && r.mask == 0 && r.lit == 0 && r.open == r.doors) {
        g_reached = true;
        UE_LOGI("[BLACKOUT-DRILL] client: breakers cut, %d group(s) off (it read up to %d of them on before), %d "
                "blackout door(s) open, %lld ms after its join", r.groups, g_sawLit, r.doors, MsSince(g_since));
    }
    if (g_reached) {
        // The door leg, once this copy has read the blackout: a walk to the named door and its press.
        if (void* door = NamedDoor())
            if (DoorLeg(door)) return;
        Done("client DONE");
    } else if (Clock::now() - g_since > kCutBound) {
        UE_LOGW("[BLACKOUT-DRILL] client: %lld s after its join its breakers read %d, %d of %d group(s) still on, %d "
                "of %d blackout door(s) open -- FAIL", static_cast<long long>(kCutBound.count()), r.mask, r.lit,
                r.groups, r.open, r.doors);
        Done("client DONE -- FAIL");
    }
}

void OnDisconnect() {
    if (!IsEnabled()) return;
    if (g_leg) g_leg->cancel.store(true, std::memory_order_release);  // its walk ends with the session
    g_leg.reset();
    g_panel = nullptr;
    g_panelIdx = -1;
    g_prepared = false;
    g_fired = false;
    g_done = false;
    g_sawLit = 0;
    g_door.Reset();
    g_doorSought = false;
    g_gridAfter = false;
    g_reached = false;
    g_since = g_nextRead = {};
}

}  // namespace coop::dev::blackout_drill

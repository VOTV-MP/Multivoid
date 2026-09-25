// coop/dev/keypad_drill.cpp -- see coop/dev/keypad_drill.h.

#include "coop/dev/keypad_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/interactables/keypad_sync.h"  // the keypad lane's key
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/door.h"
#include "ue_wrap/devices/passwordlock.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::keypad_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace PL = ue_wrap::passwordlock;
namespace DR = coop::director;
using Clock = std::chrono::steady_clock;

constexpr auto  kLegBound   = std::chrono::seconds(10);
constexpr float kStandCm    = 150.f;   // this near the keypad, a player stands where it can use it
constexpr int   kCandidates = 14;      // the nearest keypads by straight distance; each costs a route

enum class Leg { Accept, Cancel, Deny };
enum class Phase { Unpicked, Walking, Typing, Landing, Done };

// What the walker thread hands the game thread: the keypad it stood at, or that it could not.
std::atomic<void*> g_arrivedAt{nullptr};
std::atomic<int>   g_walkResult{0};   // 0 walking, 1 arrived, 2 no keypad or no arrival
std::atomic<bool>  g_walkerStarted{false};

void*        g_lock = nullptr;
int32_t      g_lockIdx = -1;
std::wstring g_key;
std::wstring g_password;
PL::State    g_last;
bool         g_haveLast = false;
Phase        g_phase = Phase::Unpicked;
std::vector<Leg> g_legs;
size_t       g_leg = 0;
bool         g_cancelSawDigits = false;
bool         g_sawEcho = false;   // this leg's typing has shown on this copy: the host's digits came back
Clock::time_point g_since{};
int          g_failures = 0;

// The host watches every keypad that gates a door, since the client picks by its own walk.
struct Watched { void* lock; int32_t idx; std::wstring key; PL::State last; };
std::vector<Watched> g_watched;

const char* Side() { return coop::roster::LocalIsHost() ? "host" : "client"; }

const char* LegName(Leg leg) {
    switch (leg) {
    case Leg::Accept: return "ACCEPT";
    case Leg::Cancel: return "CANCEL";
    case Leg::Deny:   return "DENY";
    }
    return "?";
}

bool RoleIsReady() {
    if (coop::roster::LocalIsHost()) return true;
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

float HorizDist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y;
    return std::sqrt(dx * dx + dy * dy);
}

// The keypad this peer's navmesh reaches by the shortest walk, among keypads the lane names that gate
// a door: a route asked for the keypad itself ends on the floor below it, where a player stands to use
// it. From far off every route stops short at the search's node limit, and then the nearest keypad a
// route heads for is walked to anyway: the director re-paths on the way. The walk ends within
// kStandCm of the keypad, on its level.
bool PickReachableKeypad(void* player, const ue_wrap::FVector& at, DR::DirectorGoal& goal, float& lenOut) {
    struct Cand { void* lock; ue_wrap::FVector pos; float dist; };
    std::vector<Cand> cands;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o) || !PL::IsPasswordLock(o) || !PL::GatedDoor(o)) continue;
        if (coop::keypad_sync::KeypadKey(o).empty()) continue;
        ue_wrap::FVector p{};
        if (!E::TryGetActorLocation(o, p)) continue;
        cands.push_back({o, p, HorizDist(p, at)});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
    if (cands.size() > static_cast<size_t>(kCandidates)) cands.resize(kCandidates);
    float best = 1e30f;
    int noRoute = 0, endsFar = 0;
    const Cand* nearestRouted = nullptr;
    float nearestRoutedLen = 0.f;
    for (const Cand& c : cands) {
        std::vector<ue_wrap::FVector> path;
        if (!E::FindNavPath(player, at, c.pos, path) || path.empty()) { ++noRoute; continue; }
        float len = 0.f;
        for (size_t i = 1; i < path.size(); ++i) len += HorizDist(path[i - 1], path[i]);
        if (!nearestRouted) {  // candidates run nearest first
            nearestRouted = &c;
            nearestRoutedLen = len;
        }
        if (HorizDist(path.back(), c.pos) > kStandCm) { ++endsFar; continue; }
        if (len >= best) continue;
        best = len;
        goal.targetActor = c.lock;
        lenOut = len;
    }
    if (!goal.targetActor && nearestRouted) {
        goal.targetActor = nearestRouted->lock;
        lenOut = nearestRouted->dist > nearestRoutedLen ? nearestRouted->dist : nearestRoutedLen;
        UE_LOGI("[KEYPAD-DRILL] client: every route of %zu stops short of its keypad (%d with no route); walking "
                "to the nearest, %.0fcm off, and re-pathing on the way", cands.size(), noRoute, nearestRouted->dist);
    }
    if (!goal.targetActor) {
        UE_LOGW("[KEYPAD-DRILL] client at (%.0f,%.0f,%.0f): %zu keypad(s) gate a door and are named, none with a "
                "route", at.X, at.Y, at.Z, cands.size());
        return false;
    }
    E::TryGetActorLocation(goal.targetActor, goal.targetPos);
    return true;
}

int WalkSeconds(float routeCm) { return std::clamp(static_cast<int>(routeCm / 100.f) + 60, 90, 900); }

// The client walks to its keypad with the director, as a player would stand at it.
DWORD WINAPI WalkerThread(LPVOID) {
    auto goal = std::make_shared<DR::DirectorGoal>();
    goal->reachCm = kStandCm;
    auto len = std::make_shared<float>(0.f);
    const int picked = GT::RunAndWait([goal, len](std::atomic<int>& done) {
        void* p = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!p || !R::IsLive(p) || !E::GetController(p) || !E::TryGetActorLocation(p, at)) {
            done.store(2);
            return;
        }
        done.store(PickReachableKeypad(p, at, *goal, *len) ? 1 : 2);
    });
    if (picked != 1) {
        g_walkResult.store(2);
        return 0;
    }
    UE_LOGI("[KEYPAD-DRILL] client walks to a keypad at (%.0f,%.0f,%.0f), a %.0fcm route", goal->targetPos.X,
            goal->targetPos.Y, goal->targetPos.Z, *len);
    DR::ControlManager mgr;
    DR::AddWalkToProcesses(mgr, *goal);
    const bool at = mgr.Run(*goal, WalkSeconds(*len)) && goal->reached;
    if (!at) {
        g_walkResult.store(2);
        return 0;
    }
    g_arrivedAt.store(goal->targetActor);
    g_walkResult.store(1);
    return 0;
}

bool DoorActive(bool& on) {
    void* door = PL::GatedDoor(g_lock);
    return door && ue_wrap::door::TryReadActive(door, on);
}

void Done(const char* verdict) {
    g_phase = Phase::Done;
    UE_LOGI("[KEYPAD-DRILL] %s DONE keypad='%ls' %s", Side(), g_key.c_str(), verdict);
}

// Digits, typed through the numpad (the focused keypad's playerAnykey) or pressed on the keys (the
// keypad's own inputNumber, as the E-press reaches it).
void TypeDigits(const std::wstring& digits, bool numpad) {
    static const wchar_t* const kNumPad[10] = {L"NumPadZero", L"NumPadOne", L"NumPadTwo", L"NumPadThree",
                                               L"NumPadFour", L"NumPadFive", L"NumPadSix", L"NumPadSeven",
                                               L"NumPadEight", L"NumPadNine"};
    for (wchar_t c : digits) {
        if (c < L'0' || c > L'9') continue;
        if (numpad) PL::CallPlayerAnykey(g_lock, kNumPad[c - L'0'], true);
        else PL::CallInputNumber(g_lock, static_cast<int32_t>(c - L'0'));
    }
}

// A press off the digit keys, on the accept key or the cancel key as an aim would have it.
void PressKey(bool cancel) {
    PL::WriteHover(g_lock, !cancel, cancel);
    PL::CallPressOffDigits(g_lock);
    PL::WriteHover(g_lock, false, false);
}

std::wstring WrongCode() {
    std::wstring w = g_password;
    if (!w.empty() && w[0] >= L'0' && w[0] <= L'9') w[0] = static_cast<wchar_t>(L'0' + ((w[0] - L'0') + 1) % 10);
    return w;
}

// Types the leg in one tick and reads this copy straight after: the keys went to the host, so it
// must read as before. ACCEPT goes through the numpad, DENY through the keys and the accept key,
// CANCEL through the numpad's digits and, once they show, its cancel key.
void StartLeg(const PL::State& before) {
    const Leg leg = g_legs[g_leg];
    const bool longCode = g_password.size() >= 5;
    switch (leg) {
    case Leg::Accept:
        TypeDigits(g_password, true);
        if (!longCode) PL::CallPlayerAnykey(g_lock, L"Add", true);
        break;
    case Leg::Deny:
        TypeDigits(WrongCode(), false);
        if (!longCode) PressKey(false);
        break;
    case Leg::Cancel:
        TypeDigits(g_password.substr(0, 2), true);
        g_cancelSawDigits = false;
        break;
    }
    g_sawEcho = false;
    PL::State after;
    PL::ReadState(g_lock, after);
    const bool left = after.buffer == before.buffer && after.active == before.active;
    UE_LOGI("[KEYPAD-DRILL] client %s typed on keypad='%ls': own copy buf '%ls' -> '%ls', active %d -> %d (%s)",
            LegName(leg), g_key.c_str(), before.buffer.c_str(), after.buffer.c_str(), before.active ? 1 : 0,
            after.active ? 1 : 0, left ? "left, as it should" : "MOVED LOCALLY");
    if (!left) ++g_failures;
    g_phase = Phase::Landing;
    g_since = Clock::now();
}

// Whether this copy shows the leg's end: the buffer empty on the verdict, and the gated door handed
// the same power.
bool Landed(Leg leg, const PL::State& cur, bool& fail) {
    fail = false;
    if (!cur.buffer.empty()) return false;
    const bool want = leg == Leg::Accept;
    if (cur.active != want) return false;
    bool door = false;
    if (!DoorActive(door)) {
        UE_LOGW("[KEYPAD-DRILL] client %s: the gated door's active is unreadable -- FAIL", LegName(leg));
        fail = true;
    } else if (door != want) {
        UE_LOGW("[KEYPAD-DRILL] client %s: the keypad reads active %d and its door %d -- FAIL", LegName(leg),
                want ? 1 : 0, door ? 1 : 0);
        fail = true;
    }
    return true;
}

void ClientStep(const PL::State& cur, long long ms) {
    const Leg leg = g_legs[g_leg];
    if (g_phase == Phase::Typing) {
        StartLeg(cur);
        return;
    }
    // A leg's end state can equal its start, so it lands only after its own digits came back.
    if (!cur.buffer.empty()) g_sawEcho = true;
    const bool late = Clock::now() - g_since > kLegBound;
    bool ended = false;
    if (leg == Leg::Cancel && !g_cancelSawDigits) {
        // The cancel leg presses its key only once its two digits show on this copy.
        if (cur.buffer == g_password.substr(0, 2)) {
            g_cancelSawDigits = true;
            UE_LOGI("[KEYPAD-DRILL] client CANCEL: the two digits showed after %lld ms; pressing the numpad's cancel",
                    ms);
            PL::CallPlayerAnykey(g_lock, L"Subtract", true);
        } else if (late) {
            UE_LOGW("[KEYPAD-DRILL] client CANCEL: the two digits never showed within %lld s -- FAIL",
                    static_cast<long long>(kLegBound.count()));
            ++g_failures;
            ended = true;
        }
    } else {
        bool fail = false;
        if (g_sawEcho && Landed(leg, cur, fail)) {
            if (fail) ++g_failures;
            UE_LOGI("[KEYPAD-DRILL] client %s landed after %lld ms: buf '' active %d", LegName(leg), ms,
                    cur.active ? 1 : 0);
            ended = true;
        } else if (late) {
            UE_LOGW("[KEYPAD-DRILL] client %s did not land within %lld s (echo seen %d, buf '%ls' active %d) -- FAIL",
                    LegName(leg), static_cast<long long>(kLegBound.count()), g_sawEcho ? 1 : 0, cur.buffer.c_str(),
                    cur.active ? 1 : 0);
            ++g_failures;
            ended = true;
        }
    }
    if (!ended) return;
    ++g_leg;
    g_phase = Phase::Typing;
    if (g_leg >= g_legs.size())
        Done(g_failures == 0 ? "all legs landed on both copies" : "a leg failed -- FAIL");
}

// The host logs every change of every keypad that gates a door.
void HostTick() {
    if (g_watched.empty()) {
        const int32_t n = R::NumObjects();
        for (int32_t i = 0; i < n; ++i) {
            void* o = R::ObjectAt(i);
            if (!o || !R::IsLive(o) || !PL::IsPasswordLock(o) || !PL::GatedDoor(o)) continue;
            std::wstring key = coop::keypad_sync::KeypadKey(o);
            if (key.empty()) continue;
            PL::State st;
            PL::ReadState(o, st);
            g_watched.push_back({o, R::InternalIndexOf(o), std::move(key), st});
        }
        if (g_watched.empty()) return;  // the lane names them on a later pass
        UE_LOGI("[KEYPAD-DRILL] host watches %zu keypad(s) that gate a door", g_watched.size());
        return;
    }
    for (Watched& w : g_watched) {
        if (!R::IsLiveByIndex(w.lock, w.idx)) continue;
        PL::State cur;
        if (!PL::ReadState(w.lock, cur)) continue;
        if (cur.buffer == w.last.buffer && cur.active == w.last.active && cur.isReset == w.last.isReset) continue;
        UE_LOGI("[KEYPAD-DRILL] host keypad='%ls' reads buf '%ls' active %d reset %d", w.key.c_str(),
                cur.buffer.c_str(), cur.active ? 1 : 0, cur.isReset ? 1 : 0);
        w.last = cur;
    }
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::keypad_drill);
    return s;
}

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || g_phase == Phase::Done) return;
    if (!session || !session->connected() || !RoleIsReady()) return;
    if (!PL::EnsureResolved()) return;
    if (coop::roster::LocalIsHost()) {
        HostTick();
        return;
    }
    if (g_phase == Phase::Unpicked) {
        if (!g_walkerStarted.exchange(true)) {
            if (HANDLE h = ::CreateThread(nullptr, 0, &WalkerThread, nullptr, 0, nullptr)) ::CloseHandle(h);
        }
        g_phase = Phase::Walking;
        return;
    }
    if (g_phase == Phase::Walking) {
        const int result = g_walkResult.load();
        if (result == 0) return;
        if (result == 2) {
            UE_LOGW("[KEYPAD-DRILL] client: no keypad reached -- INCONCLUSIVE");
            Done("no keypad reached");
            return;
        }
        g_lock = g_arrivedAt.load();
        g_lockIdx = R::InternalIndexOf(g_lock);
        g_key = coop::keypad_sync::KeypadKey(g_lock);
        PL::ReadState(g_lock, g_last);
        g_password = g_last.password;
        g_haveLast = true;
        UE_LOGI("[KEYPAD-DRILL] client at keypad='%ls' (password of %zu digit(s)): buf '%ls' active %d reset %d",
                g_key.c_str(), g_password.size(), g_last.buffer.c_str(), g_last.active ? 1 : 0,
                g_last.isReset ? 1 : 0);
        if (g_key.empty() || g_password.empty() || g_last.isReset) {
            UE_LOGW("[KEYPAD-DRILL] client: the keypad is unnamed, has no password or is setting one -- INCONCLUSIVE");
            Done("unusable keypad");
            return;
        }
        g_legs = g_last.active ? std::vector<Leg>{Leg::Deny, Leg::Accept, Leg::Cancel}
                               : std::vector<Leg>{Leg::Accept, Leg::Cancel, Leg::Deny};
        g_leg = 0;
        g_phase = Phase::Typing;
        return;
    }
    if (!R::IsLiveByIndex(g_lock, g_lockIdx)) {
        UE_LOGW("[KEYPAD-DRILL] client: keypad='%ls' is gone -- INCONCLUSIVE", g_key.c_str());
        Done("gone");
        return;
    }
    PL::State cur;
    if (!PL::ReadState(g_lock, cur)) return;
    if (!g_haveLast || cur.buffer != g_last.buffer || cur.active != g_last.active || cur.isReset != g_last.isReset) {
        UE_LOGI("[KEYPAD-DRILL] client keypad='%ls' reads buf '%ls' active %d reset %d", g_key.c_str(),
                cur.buffer.c_str(), cur.active ? 1 : 0, cur.isReset ? 1 : 0);
        g_last = cur;
        g_haveLast = true;
    }
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_since).count();
    ClientStep(cur, ms);
}

void OnDisconnect() {
    if (!IsEnabled()) return;
    g_lock = nullptr;
    g_lockIdx = -1;
    g_key.clear();
    g_password.clear();
    g_last = PL::State{};
    g_haveLast = false;
    g_legs.clear();
    g_leg = 0;
    g_cancelSawDigits = false;
    g_sawEcho = false;
    g_failures = 0;
    g_watched.clear();
    // A walker still running finishes its walk; its result is for the session that started it.
    g_walkerStarted.store(false);
    g_walkResult.store(0);
    g_arrivedAt.store(nullptr);
    g_phase = Phase::Unpicked;
}

}  // namespace coop::dev::keypad_drill

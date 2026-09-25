// coop/dev/keypad_drill.cpp -- see coop/dev/keypad_drill.h.

#include "coop/dev/keypad_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/interactables/keypad_sync.h"  // the keypad lane's key
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/save/save_transfer.h"  // WorldTakenFor: the late leg's window opens
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

enum class Leg { Press, Accept, Cancel, Deny, Tail };
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
int          g_doorBefore = -1;   // the press leg: the gated door's open on this copy before the press
std::wstring g_tailDigits;        // the tail leg: the two digits typed after its submit
Clock::time_point g_since{};
int          g_failures = 0;
bool         g_lateDone = false;  // the host's late leg ran, or was found impossible

// The host watches every keypad that gates a door, since the client picks by its own walk.
struct Watched { void* lock; int32_t idx; std::wstring key; PL::State last; };
std::vector<Watched> g_watched;

const char* Side() { return coop::roster::LocalIsHost() ? "host" : "client"; }

const char* LegName(Leg leg) {
    switch (leg) {
    case Leg::Press:  return "PRESS";
    case Leg::Accept: return "ACCEPT";
    case Leg::Cancel: return "CANCEL";
    case Leg::Deny:   return "DENY";
    case Leg::Tail:   return "TAIL";
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

// A code the keys can enter: digits only, as the keypad's inputNumber appends.
bool Typeable(const std::wstring& code) {
    if (code.empty()) return false;
    for (wchar_t c : code)
        if (c < L'0' || c > L'9') return false;
    return true;
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
        PL::State st;  // a code a player can type: the alpha bunker's are letters
        if (!PL::ReadState(o, st) || !Typeable(st.password)) continue;
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

// The keypad the late leg flips: the first, by key, of the named keypads that gate a door and whose
// code the keys cannot type, so no leg of the client's stands at it or its pair (a pair shares one
// code). Null when there is none.
void* LateKeypad(std::wstring& keyOut) {
    void* best = nullptr;
    keyOut.clear();
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o) || !PL::IsPasswordLock(o) || !PL::GatedDoor(o)) continue;
        std::wstring key = coop::keypad_sync::KeypadKey(o);
        PL::State st;
        if (key.empty() || !PL::ReadState(o, st) || Typeable(st.password)) continue;
        if (!best || key < keyOut) {
            best = o;
            keyOut = std::move(key);
        }
    }
    return best;
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

// A keypad's gated door as 1, 0, or -1 when it has none or its active is unreadable.
int DoorOf(void* lock) {
    void* door = PL::GatedDoor(lock);
    bool on = false;
    return door && ue_wrap::door::TryReadActive(door, on) ? (on ? 1 : 0) : -1;
}

// The drilled keypad's gated door's open on this copy, the swing's destination while it moves; -1
// when unreadable.
int DoorOpen() {
    void* door = PL::GatedDoor(g_lock);
    bool open = false;
    return door && ue_wrap::door::TryReadOpenIntent(door, open) ? (open ? 1 : 0) : -1;
}

// Every named keypad that gates a door (the two keypads of a pair share one), the door's active beside
// the keypad's: the keypad's setActive hands its verdict on to the door, so a door that reads otherwise
// opens as its keypad does not say. Sorted by key, seven to a line, so the peers compare entry by entry.
void Census(const char* when) {
    struct Row { std::wstring key; bool keypad; int door; };
    std::vector<Row> rows;
    int unnamed = 0, differ = 0;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o) || !PL::IsPasswordLock(o) || !PL::GatedDoor(o)) continue;
        std::wstring key = coop::keypad_sync::KeypadKey(o);
        PL::State st;
        if (key.empty() || !PL::ReadState(o, st)) { ++unnamed; continue; }
        const int door = DoorOf(o);
        if (door != (st.active ? 1 : 0)) ++differ;
        rows.push_back({std::move(key), st.active, door});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.key < b.key; });
    UE_LOGI("[KEYPAD-DRILL] %s census %s: %zu named keypad(s) gate a door (%d unnamed or unread), %d whose "
            "door's active differs from the keypad's (key:keypad/door, ! where they differ)", Side(), when,
            rows.size(), unnamed, differ);
    for (size_t at = 0; at < rows.size(); at += 7) {
        std::wstring line;
        for (size_t i = at; i < rows.size() && i < at + 7; ++i) {
            const Row& r = rows[i];
            line += L" " + r.key + (r.keypad ? L":1/" : L":0/") + (r.door < 0 ? L"?" : r.door ? L"1" : L"0");
            if (r.door != (r.keypad ? 1 : 0)) line += L"!";
        }
        UE_LOGI("[KEYPAD-DRILL] %s census %s:%ls", Side(), when, line.c_str());
    }
}

// The client's half of the late leg, once every leg is over: the keypad the host flipped after this
// client's world was taken, when only the snapshot could carry it, reads with its door as one.
void CheckLate() {
    std::wstring key;
    void* lock = LateKeypad(key);
    PL::State st;
    if (!lock || !PL::ReadState(lock, st)) {
        UE_LOGI("[KEYPAD-DRILL] client LATE: no keypad the legs cannot type gates a door -- INCONCLUSIVE");
        return;
    }
    const int door = DoorOf(lock);
    const bool same = door == (st.active ? 1 : 0);
    UE_LOGI("[KEYPAD-DRILL] client LATE keypad='%ls': active %d, its door %d (%s)", key.c_str(), st.active ? 1 : 0,
            door, same ? "the door follows its keypad" : "the door DIFFERS -- FAIL");
    if (!same) ++g_failures;
}

void Done(const char* verdict) {
    g_phase = Phase::Done;
    if (!coop::roster::LocalIsHost()) CheckLate();
    Census("at the end");
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

// The press leg: the gated door's own E-press entry, as the player at the keypad would reach it. The
// press runs on the host, so this copy's door must read as before straight after.
void StartPress() {
    void* door = PL::GatedDoor(g_lock);
    void* player = coop::players::Registry::Get().Local();
    g_doorBefore = DoorOpen();
    const bool pressed = door && player && ue_wrap::door::CallPress(door, player, ue_wrap::door::kUseAction);
    const int after = DoorOpen();
    const bool left = g_doorBefore >= 0 && after == g_doorBefore;
    UE_LOGI("[KEYPAD-DRILL] client PRESS on keypad='%ls''s door: dispatched=%d, own copy open %d -> %d (%s)",
            g_key.c_str(), pressed ? 1 : 0, g_doorBefore, after, left ? "left, as it should" : "MOVED LOCALLY or unread");
    if (!pressed || !left) ++g_failures;
    g_phase = Phase::Landing;
    g_since = Clock::now();
}

// Types the leg in one tick and reads this copy straight after: the keys went to the host, so it
// must read as before. ACCEPT goes through the numpad, DENY through the keys and the accept key,
// CANCEL through the numpad's digits and, once they show, its cancel key.
void StartLeg(const PL::State& before) {
    const Leg leg = g_legs[g_leg];
    if (leg == Leg::Press) {
        StartPress();
        return;
    }
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
    case Leg::Tail: {
        // Two digits no prefix of the wrong code can read as, typed after its submit in the same tick.
        const std::wstring wrong = WrongCode();
        TypeDigits(wrong, false);
        if (!longCode) PressKey(false);
        const wchar_t d = static_cast<wchar_t>(L'0' + ((wrong.empty() ? 0 : wrong[0] - L'0') + 1) % 10);
        g_tailDigits = std::wstring(2, d);
        TypeDigits(g_tailDigits, false);
        break;
    }
    case Leg::Press:
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

// Whether this copy shows the leg's end: the buffer on the verdict (empty, or the tail's two digits),
// and the gated door handed the same power.
bool Landed(Leg leg, const PL::State& cur, bool& fail) {
    fail = false;
    if (cur.buffer != (leg == Leg::Tail ? g_tailDigits : std::wstring())) return false;
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
    if (leg == Leg::Press) {
        // The host ran the press; the door lane brings its door's open back to this copy.
        const int open = DoorOpen();
        if (open >= 0 && open != g_doorBefore) {
            UE_LOGI("[KEYPAD-DRILL] client PRESS landed after %lld ms: the door's open %d -> %d on this copy", ms,
                    g_doorBefore, open);
            ended = true;
        } else if (late) {
            UE_LOGW("[KEYPAD-DRILL] client PRESS: the door's open still reads %d after %lld s -- FAIL", open,
                    static_cast<long long>(kLegBound.count()));
            ++g_failures;
            ended = true;
        }
    } else if (leg == Leg::Cancel && !g_cancelSawDigits) {
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
            UE_LOGI("[KEYPAD-DRILL] client %s landed after %lld ms: buf '%ls' active %d", LegName(leg), ms,
                    cur.buffer.c_str(), cur.active ? 1 : 0);
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
    if (g_leg >= g_legs.size()) {
        PL::CallPlayerAnykey(g_lock, L"Subtract", true);  // the tail's two digits cleared, unchecked
        Done(g_failures == 0 ? "all legs landed on both copies" : "a leg failed -- FAIL");
    }
}

// The host's half of the late leg: once slot 1's world is taken for its join, and before slot 1 is
// world-ready, the late keypad's verdict is negated as its own chain leaves it (active, then
// setActive(false), which hands it on to its pair and door). No state of it can reach the joiner
// before it is ready, so only the joiner's snapshot carries the new verdict to that door.
void HostLate(coop::net::Session& s) {
    if (g_lateDone || !coop::save_transfer::WorldTakenFor(1)) return;
    g_lateDone = true;
    if (s.IsSlotWorldReady(1)) {
        UE_LOGW("[KEYPAD-DRILL] host LATE: slot 1 was world-ready when its world was seen taken -- INCONCLUSIVE");
        return;
    }
    std::wstring key;
    void* lock = LateKeypad(key);
    PL::State st;
    if (!lock || !PL::ReadState(lock, st)) {
        UE_LOGI("[KEYPAD-DRILL] host LATE: no keypad the legs cannot type gates a door -- INCONCLUSIVE");
        return;
    }
    const bool flipped = PL::WriteActive(lock, !st.active) && PL::CallSetActive(lock, false);
    UE_LOGI("[KEYPAD-DRILL] host LATE keypad='%ls': active %d -> %d after slot 1's world was taken, before it "
            "was ready (dispatched=%d, its door now %d)", key.c_str(), st.active ? 1 : 0, st.active ? 0 : 1,
            flipped ? 1 : 0, DoorOf(lock));
    Census("after the late flip");
}

// The host logs every change of every keypad that gates a door.
void HostTick(coop::net::Session& s) {
    HostLate(s);
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
        Census("at the start");
        return;
    }
    for (Watched& w : g_watched) {
        if (!R::IsLiveByIndex(w.lock, w.idx)) continue;
        PL::State cur;
        if (!PL::ReadState(w.lock, cur)) continue;
        if (cur.buffer == w.last.buffer && cur.active == w.last.active && cur.isReset == w.last.isReset) continue;
        UE_LOGI("[KEYPAD-DRILL] host keypad='%ls' reads buf '%ls' active %d reset %d door %d", w.key.c_str(),
                cur.buffer.c_str(), cur.active ? 1 : 0, cur.isReset ? 1 : 0, DoorOf(w.lock));
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
        HostTick(*session);
        return;
    }
    if (g_phase == Phase::Unpicked) {
        Census("at the start");
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
        if (g_key.empty() || !Typeable(g_password) || g_last.isReset) {
            UE_LOGW("[KEYPAD-DRILL] client: the keypad is unnamed, has no code the keys can enter, or is setting "
                    "one -- INCONCLUSIVE");
            Done("unusable keypad");
            return;
        }
        // The press comes while the keypad is unlocked: first on one found so, as a joiner meets it.
        g_legs = g_last.active ? std::vector<Leg>{Leg::Press, Leg::Deny, Leg::Accept, Leg::Cancel, Leg::Tail}
                               : std::vector<Leg>{Leg::Accept, Leg::Press, Leg::Cancel, Leg::Deny, Leg::Tail};
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
        UE_LOGI("[KEYPAD-DRILL] client keypad='%ls' reads buf '%ls' active %d reset %d door %d", g_key.c_str(),
                cur.buffer.c_str(), cur.active ? 1 : 0, cur.isReset ? 1 : 0, DoorOf(g_lock));
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
    g_doorBefore = -1;
    g_tailDigits.clear();
    g_failures = 0;
    g_lateDone = false;
    g_watched.clear();
    // A walker still running finishes its walk; its result is for the session that started it.
    g_walkerStarted.store(false);
    g_walkResult.store(0);
    g_arrivedAt.store(nullptr);
    g_phase = Phase::Unpicked;
}

}  // namespace coop::dev::keypad_drill

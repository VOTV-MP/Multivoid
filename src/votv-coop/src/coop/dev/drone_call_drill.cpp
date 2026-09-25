// coop/dev/drone_call_drill.cpp -- see coop/dev/drone_call_drill.h.

#include "coop/dev/drone_call_drill.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "coop/interactables/drone_call_intent.h"  // SentCount: the press went to the host
#include "coop/interactables/drone_sync.h"         // HostActive: the host's word that its drone parked
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/drone.h"
#include "ue_wrap/devices/drone_console.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

namespace coop::dev::drone_call_drill {
namespace {

namespace D  = ue_wrap::drone_console;
namespace DR = ue_wrap::drone;
namespace E  = ue_wrap::engine;
namespace WS = ue_wrap::world_singleton;

// The console's parts by the names its graph gives them, and the sack the drone's own triggerFly
// asks the world for.
const wchar_t* const kKeyboard  = L"button_call";
const wchar_t* const kLidLatch  = L"button_door";
const wchar_t* const kSackClass = L"prop_dronesack_C";

constexpr float    kReachCm         = 150.f;  // the director's stop; the host's reach test is 400
constexpr int      kWalkDeadlineS   = 300;    // the director's own bound on one walk: a save can start 800 m out
constexpr int      kAimFanHalf      = 4;      // the fan: (2 * half + 1)^2 headings around the part
constexpr float    kAimFanStepDeg   = 4.f;
constexpr int      kAimTicksPerPose = 4;      // each heading is held for the trace to answer
constexpr float    kFlewCm          = 2000.f; // a flight covers kilometres
constexpr float    kSettleCm        = 10.f;   // two reads this close apart: the drone is at rest
constexpr int      kMaxPresses      = 2;      // the first can only put a sack aboard; the second flies
constexpr uint64_t kReadEveryMs     = 250;    // the drone and the sack are read at 4 Hz
// Failure bounds only: each leg ends on the state it waits for, and these say it never came.
constexpr uint64_t kParkBoundMs     = 240000;  // a delivery in flight at the load lands within minutes
constexpr uint64_t kSettleBoundMs   = 30000;
constexpr uint64_t kLidBoundMs      = 10000;
constexpr uint64_t kOutcomeBoundMs  = 30000;

// ---- the client's legs -----------------------------------------------------------------------
enum class Step : uint8_t { Arm, Settle, Walk, Aim, Press, LidWait, Outcome, Done };
Step     g_step = Step::Arm;
int      g_stepTicks = 0;
uint64_t g_stepMs = 0;
int      g_aimPose = 0;
bool     g_aimLid = false;       // the part the Aim and Press legs are on: the lid's latch, else the keyboard
int      g_presses = 0;          // keyboard presses that went to the host
bool     g_sackAtPress = false;  // a sack lay in this world when the last press went
bool     g_saidInFlight = false;
uint64_t g_armMs = 0;            // when the legs first could start: the park wait's bound runs from here
int      g_session = 1;          // this process's sessions, counted by their ends: a rehost's is the 2nd
uint64_t g_nextReadMs = 0;
ue_wrap::CachedObjRef g_console;
ue_wrap::FVector      g_droneRest{};
ue_wrap::FVector      g_droneLast{};
bool                  g_haveLast = false;

// The director blocks, so a walk runs on a worker; the step polls the shared state.
struct Walk {
    coop::director::DirectorGoal goal;
    std::atomic<int> state{0};  // 0 walking, 1 reached, 2 failed
};
std::shared_ptr<Walk> g_walk;

DWORD WINAPI WalkThread(LPVOID arg) {
    auto* holder = static_cast<std::shared_ptr<Walk>*>(arg);
    std::shared_ptr<Walk> w = *holder;
    delete holder;
    coop::director::ControlManager mgr;
    coop::director::AddWalkToProcesses(mgr, w->goal);
    mgr.Run(w->goal, kWalkDeadlineS);
    w->state.store(w->goal.reached ? 1 : 2);
    return 0;
}

void StartWalk(const ue_wrap::FVector& to) {
    g_walk = std::make_shared<Walk>();
    g_walk->goal.targetPos = to;
    g_walk->goal.reachCm = kReachCm;
    auto* arg = new std::shared_ptr<Walk>(g_walk);
    if (HANDLE t = ::CreateThread(nullptr, 0, &WalkThread, arg, 0, nullptr)) ::CloseHandle(t);
    else { delete arg; g_walk->state.store(2); }
}

int WalkState() { return g_walk ? g_walk->state.load() : 2; }

// The aim fan, nearest heading first.
const std::vector<std::pair<int, int>>& AimFan() {
    static const std::vector<std::pair<int, int>> fan = [] {
        std::vector<std::pair<int, int>> v;
        for (int p = -kAimFanHalf; p <= kAimFanHalf; ++p)
            for (int y = -kAimFanHalf; y <= kAimFanHalf; ++y) v.emplace_back(p, y);
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second;
        });
        return v;
    }();
    return fan;
}

void Go(Step s) {
    g_step = s;
    g_stepTicks = 0;
    g_stepMs = ::GetTickCount64();
}

void Abandon(const char* why) {
    UE_LOGW("[DRONE-CALL-DRILL] ABANDONED on the client: %s", why);
    g_step = Step::Done;
}

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool SackInWorld() { return WS::Find(kSackClass) != nullptr; }

// The drone's hasSack as this copy holds it: the host's own field, or on a client the gate field its
// mirror is written from the drone's stream.
int HasSack(void* drone) { return drone && (DR::ReadFxBits(drone) & DR::kFxHasSack) ? 1 : 0; }

// The trace's present answer is the part, and the console built that part's action options for it:
// its cursor flags are written only when it builds them, so neither alone says what E would send.
bool AimedAt(void* player, void* console, void* part) {
    if (E::ReadMainPlayerHitActor(player) != console || E::ReadMainPlayerHitComponent(player) != part)
        return false;
    return g_aimLid ? D::IsCursorOnLid(console) : D::IsCursorOnKeyboard(console);
}

void ClientTick(void* player) {
    ++g_stepTicks;
    const uint64_t now = ::GetTickCount64();
    switch (g_step) {
    case Step::Arm: {
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        // A press is measured against a parked drone: one still flying a delivery moves on its own. The
        // mirror's own Active field is the load's, so the host's word is asked.
        if (g_armMs == 0) g_armMs = now;
        const int hostActive = coop::drone_sync::HostActive();
        if (hostActive != 0) {
            if (hostActive == 1 && !g_saidInFlight) {
                g_saidInFlight = true;
                UE_LOGI("[DRONE-CALL-DRILL] client: the host's drone is in flight; the legs wait for it to park");
            }
            if (now - g_armMs > kParkBoundMs)
                Abandon(hostActive < 0 ? "the host never said where its drone is" : "the host's drone did not park");
            return;
        }
        g_haveLast = false;
        g_nextReadMs = 0;
        Go(Step::Settle);
        return;
    }
    case Step::Settle: {
        // The mirror follows the stream through its interpolation, so at the host's word it can still be
        // short of the pose that word carried; nothing streams while the drone is parked, so the mirror
        // comes to rest on it, and the rest is read there.
        if (now < g_nextReadMs) return;
        g_nextReadMs = now + kReadEveryMs;
        void* drone = DR::Find();
        ue_wrap::FVector pos{};
        if (!drone || !E::TryGetActorLocation(drone, pos)) {
            Abandon("this world holds no drone, or its place is unread");
            return;
        }
        const bool still = g_haveLast && Dist(pos, g_droneLast) < kSettleCm;
        g_droneLast = pos;
        g_haveLast = true;
        if (!still) {
            if (now - g_stepMs > kSettleBoundMs) Abandon("this copy's drone did not come to rest");
            return;
        }
        g_droneRest = pos;
        void* consoles[2] = {};
        if (D::LiveConsoles(consoles, 2) <= 0) { Abandon("this world holds no drone console"); return; }
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(consoles[0], at)) { Abandon("the console's place is unread"); return; }
        g_console.Set(consoles[0]);
        UE_LOGI("[DRONE-CALL-DRILL] client: the console at (%.0f, %.0f, %.0f), its lid %s; the drone at rest at "
                "(%.0f, %.0f, %.0f), hasSack=%d on this copy; a sack in this world: %s", at.X, at.Y, at.Z,
                D::IsLidOpen(consoles[0]) ? "open" : "shut", g_droneRest.X, g_droneRest.Y, g_droneRest.Z,
                HasSack(drone), SackInWorld() ? "yes" : "no");
        StartWalk(at);
        Go(Step::Walk);
        return;
    }
    case Step::Walk: {
        if (WalkState() == 0) return;
        if (WalkState() == 2) { Abandon("the walk to the console did not arrive"); return; }
        void* c = g_console.Get();
        if (!c) { Abandon("the console died"); return; }
        g_aimLid = !D::IsLidOpen(c);  // the keyboard takes no press while the lid is shut
        g_aimPose = 0;
        Go(Step::Aim);
        return;
    }
    case Step::Aim: {
        void* c = g_console.Get();
        if (!c) { Abandon("the console died"); return; }
        void* part = D::PartOf(c, g_aimLid ? kLidLatch : kKeyboard);
        if (!part) { Abandon(g_aimLid ? "the console's lid latch is unread" : "the console's keyboard is unread"); return; }
        if (AimedAt(player, c, part)) {
            UE_LOGI("[DRONE-CALL-DRILL] client: the trace took the console's %s at fan pose %d",
                    g_aimLid ? "lid latch" : "keyboard", g_aimPose);
            Go(Step::Press);
            return;
        }
        if (g_stepTicks % kAimTicksPerPose != 1) return;
        const auto& fan = AimFan();
        if (g_aimPose >= static_cast<int>(fan.size())) {
            Abandon(g_aimLid ? "no heading of the fan put the trace on the lid latch"
                             : "no heading of the fan put the trace on the keyboard");
            return;
        }
        ue_wrap::FRotator r = coop::director::LookAt(E::GetCameraLocation(), E::GetComponentLocation(part));
        r.Pitch += kAimFanStepDeg * static_cast<float>(fan[g_aimPose].first);
        r.Yaw   += kAimFanStepDeg * static_cast<float>(fan[g_aimPose].second);
        E::SetControlRotation(E::GetController(player), r);
        ++g_aimPose;
        return;
    }
    case Step::Press: {
        if (!g_console.Get()) { Abandon("the console died"); return; }
        if (g_aimLid) {
            if (!E::CallMainPlayerUseSelectedAction(player)) { Abandon("useSelectedAction did not dispatch"); return; }
            UE_LOGI("[DRONE-CALL-DRILL] client pressed the lid latch");
            Go(Step::LidWait);
            return;
        }
        // A flight measured after the press must be the press's: a drone that took off on its own since
        // the legs began (its leave timer, a delivery) leaves nothing to measure.
        if (coop::drone_sync::HostActive() != 0) { Abandon("the host's drone took off before the press"); return; }
        const uint64_t sent0 = coop::drone_call_intent::SentCount();
        g_sackAtPress = SackInWorld();
        if (!E::CallMainPlayerUseSelectedAction(player)) { Abandon("useSelectedAction did not dispatch"); return; }
        if (coop::drone_call_intent::SentCount() == sent0) {
            UE_LOGW("[DRONE-CALL-DRILL] FAIL on the client: the trace was on the keyboard with the lid open, "
                    "and the press sent the host nothing");
            g_step = Step::Done;
            return;
        }
        ++g_presses;
        UE_LOGI("[DRONE-CALL-DRILL] client press #%d on the keyboard went to the host (a sack in this world: %s)",
                g_presses, g_sackAtPress ? "yes" : "no");
        g_nextReadMs = 0;
        Go(Step::Outcome);
        return;
    }
    case Step::LidWait: {
        void* c = g_console.Get();
        if (!c) { Abandon("the console died"); return; }
        if (D::IsLidOpen(c)) {
            UE_LOGI("[DRONE-CALL-DRILL] client: the lid is open on this copy");
            g_aimLid = false;
            g_aimPose = 0;
            Go(Step::Aim);
            return;
        }
        if (now - g_stepMs > kLidBoundMs) Abandon("the lid did not open on this copy after its press");
        return;
    }
    case Step::Outcome: {
        if (now < g_nextReadMs) return;
        g_nextReadMs = now + kReadEveryMs;
        void* drone = DR::Find();
        ue_wrap::FVector at{};
        // A flight is the host's word that its drone is active again and this copy's drone away from its
        // rest: either alone could be the stream's last catch-up or a word with no motion behind it.
        if (coop::drone_sync::HostActive() == 1 && drone && E::TryGetActorLocation(drone, at) &&
            Dist(at, g_droneRest) >= kFlewCm) {
            UE_LOGI("[DRONE-CALL-DRILL] client DONE in session %d: the drone flew on this copy, %.0f cm from its "
                    "rest after press #%d, the host's word active -- PASS", g_session, Dist(at, g_droneRest),
                    g_presses);
            g_step = Step::Done;
            return;
        }
        // The drone's own triggerFly, with no sack aboard and none elsewhere: a press puts one aboard
        // and is refused, and the next one flies. A sack that lay here before the press is refused
        // every time, so a second press is made only for a sack the first one brought.
        if (g_presses < kMaxPresses && !g_sackAtPress && SackInWorld()) {
            UE_LOGI("[DRONE-CALL-DRILL] client: a sack came to this copy after press #%d (hasSack=%d on this "
                    "copy); pressing again", g_presses, HasSack(drone));
            g_aimPose = 0;
            Go(Step::Aim);
            return;
        }
        if (now - g_stepMs > kOutcomeBoundMs) {
            UE_LOGW("[DRONE-CALL-DRILL] client DONE in session %d: the drone did not move on this copy after press "
                    "#%d (a sack in this world at that press: %s; hasSack=%d on this copy) -- INCONCLUSIVE",
                    g_session, g_presses, g_sackAtPress ? "yes" : "no", HasSack(drone));
            g_step = Step::Done;
        }
        return;
    }
    case Step::Done:
        return;
    }
}

// ---- the host's watch ------------------------------------------------------------------------
// The host's drone as the lane's presses leave it, each change said once. A flight is measured from
// where the drone last came to rest: its Active drops while it still glides, so a flight it was
// already on at the load, or that glide, is not counted.
struct HostWatch {
    bool             seen = false, restKnown = false;
    ue_wrap::FVector rest{}, last{};
    bool             active = false, hasSack = false, sack = false, flew = false;
    uint64_t         nextReadMs = 0;
};
HostWatch g_host;

void HostTick() {
    const uint64_t now = ::GetTickCount64();
    if (now < g_host.nextReadMs) return;
    g_host.nextReadMs = now + kReadEveryMs;
    void* drone = DR::Find();
    ue_wrap::FVector at{};
    if (!drone || !E::TryGetActorLocation(drone, at)) return;
    const bool active  = DR::IsActive(drone);
    const bool hasSack = HasSack(drone) != 0;
    const bool sack    = SackInWorld();
    if (!g_host.seen) {
        g_host.seen = true;
        g_host.last = at;
        g_host.active = active;
        g_host.hasSack = hasSack;
        g_host.sack = sack;
        UE_LOGI("[DRONE-CALL-DRILL] host: the drone at (%.0f, %.0f, %.0f) active=%d hasSack=%d; a sack in this "
                "world: %s", at.X, at.Y, at.Z, active ? 1 : 0, hasSack ? 1 : 0, sack ? "yes" : "no");
        return;
    }
    if (sack != g_host.sack) {
        UE_LOGI("[DRONE-CALL-DRILL] host: a sack in this world %s", sack ? "appeared" : "is gone");
        g_host.sack = sack;
    }
    if (hasSack != g_host.hasSack) {
        UE_LOGI("[DRONE-CALL-DRILL] host: the drone's hasSack %d -> %d", g_host.hasSack ? 1 : 0, hasSack ? 1 : 0);
        g_host.hasSack = hasSack;
    }
    if (active != g_host.active) {
        UE_LOGI("[DRONE-CALL-DRILL] host: the drone's active %d -> %d at (%.0f, %.0f, %.0f)", g_host.active ? 1 : 0,
                active ? 1 : 0, at.X, at.Y, at.Z);
        g_host.active = active;
    }
    if (!active && Dist(at, g_host.last) < kSettleCm && (!g_host.restKnown || g_host.flew)) {
        g_host.restKnown = true;  // at rest: the next flight is measured from here
        g_host.flew = false;
        g_host.rest = at;
        UE_LOGI("[DRONE-CALL-DRILL] host: the drone is at rest at (%.0f, %.0f, %.0f)", at.X, at.Y, at.Z);
    }
    g_host.last = at;
    if (g_host.restKnown && !g_host.flew && active && Dist(at, g_host.rest) >= kFlewCm) {
        g_host.flew = true;
        UE_LOGI("[DRONE-CALL-DRILL] host: the drone flew, %.0f cm from its rest", Dist(at, g_host.rest));
    }
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::drone_call_drill);
    return s;
}

void Tick(coop::net::Session* s) {
    if (!IsEnabled() || !s || !s->connected()) return;
    if (s->role() == coop::net::Role::Host) { HostTick(); return; }
    if (g_step == Step::Done) return;
    if (void* player = coop::players::Registry::Get().Local()) ClientTick(player);
}

void OnDisconnect() {
    ++g_session;
    if (g_walk) {  // the worker still holds it: the director's run ends at its next tick
        g_walk->goal.failed = true;
        g_walk->goal.failReason = "session ended";
    }
    g_armMs = 0;
    g_step = Step::Arm;
    g_stepTicks = 0;
    g_aimPose = 0;
    g_aimLid = false;
    g_presses = 0;
    g_sackAtPress = false;
    g_saidInFlight = false;
    g_haveLast = false;
    g_console.Reset();
    g_walk.reset();
    g_host = HostWatch{};
}

}  // namespace coop::dev::drone_call_drill

// coop/dev/container_view_drill.cpp -- see coop/dev/container_view_drill.h.

#include "coop/dev/container_view_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/director.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/container_openers.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace coop::dev::container_view_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace CV = ue_wrap::container_view;
namespace DR = coop::director;
namespace OI = ue_wrap::object_index;
using ue_wrap::FVector;

constexpr float kStandCm    = 150.f;  // this near the ATV, a player stands where it can open it
constexpr float kLeaveCm    = 600.f;  // where the walk back must end past the reach, or it proves nothing
constexpr int   kCandidates = 8;

enum class Phase { Unpicked, WalkingTo, Opened, WalkingAway, Done };
enum class Walk : int { ToAtv = 0, Back = 1 };

// What a walker thread hands the game thread, under one lock with the session generation the walk was
// started in: a walker still running at a disconnect finishes, and publishes nothing into the next
// session, since the check and the stores are one step.
struct WalkOutcome {
    uint32_t gen = 0;
    int result = 0;              // 0 walking, 1 arrived, 2 no ATV or no arrival
    void* arrivedAt = nullptr;
    int32_t pickedIdx = -1;      // the ATV's slot, read at the pick, while it was surely live
};
std::mutex g_walkMu;
WalkOutcome g_walk;  // guarded by g_walkMu

void Publish(uint32_t gen, int result, void* arrivedAt, int32_t pickedIdx) {
    std::lock_guard<std::mutex> lk(g_walkMu);
    if (g_walk.gen != gen) return;
    g_walk.result = result;
    g_walk.arrivedAt = arrivedAt;
    g_walk.pickedIdx = pickedIdx;
}

WalkOutcome ReadWalk() {
    std::lock_guard<std::mutex> lk(g_walkMu);
    return g_walk;
}

Phase   g_phase = Phase::Unpicked;
FVector g_start{};
void*   g_atv = nullptr;
int32_t g_atvIdx = -1;
void*   g_container = nullptr;
int     g_openWithin = 0;  // ticks the view was open with the ATV within reach
int     g_openBeyond = 0;  // ticks the view was open with the ATV past it
float   g_lastMargin = 0.f;

bool RoleIsReady() {
    if (coop::roster::LocalIsHost()) return true;
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

float Dist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

float HorizDist(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y;
    return std::sqrt(dx * dx + dy * dy);
}

// How far the camera stands past the reach -- the player's live arm length plus the ATV's reach sphere,
// the close rule's own measure. Negative within reach.
bool Margin(float& out) {
    FVector eye{}, c{};
    float r = 0.f, arm = 0.f;
    if (!R::IsLiveByIndex(g_atv, g_atvIdx)) return false;
    void* const player = coop::players::Registry::Get().Local();
    if (!E::ReadMainPlayerCameraLocation(player, eye) || !E::ReadMainPlayerArmLength(player, arm)) return false;
    if (!E::ActorReachSphere(g_atv, c, r)) return false;
    out = Dist(eye, c) - (arm + r);
    return true;
}

void Done(const char* verdict) {
    g_phase = Phase::Done;
    UE_LOGI("[CVIEW-DRILL] client DONE %s (open %d tick(s) within reach, %d past it)", verdict, g_openWithin,
            g_openBeyond);
}

// The ATV this peer's navmesh reaches by the shortest walk, as the keypad drill picks its keypad.
bool PickAtv(void* player, const FVector& at, DR::DirectorGoal& goal, float& lenOut, int32_t& idxOut) {
    void* const cls = OI::ClassByName(L"ATV_C");
    if (!cls) return false;
    struct Cand { void* atv; FVector pos; float dist; };
    struct Gather { const FVector* at; std::vector<Cand> cands; } g{&at, {}};
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t index) {
        Gather& x = *static_cast<Gather*>(ctx);
        if (R::SlotFlags(index) & (R::slot_flags::Dying | R::slot_flags::NotYetReadable)) return;
        if (!ue_wrap::container_openers::Opens(obj)) return;
        FVector p{};
        if (!E::TryGetActorLocation(obj, p)) return;
        x.cands.push_back({obj, p, HorizDist(p, *x.at)});
    }, &g);
    std::sort(g.cands.begin(), g.cands.end(), [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
    if (g.cands.size() > static_cast<size_t>(kCandidates)) g.cands.resize(kCandidates);
    float best = 1e30f;
    for (const Cand& c : g.cands) {
        std::vector<FVector> path;
        if (!E::FindNavPath(player, at, c.pos, path) || path.empty()) continue;
        if (HorizDist(path.back(), c.pos) > kStandCm) continue;
        float len = 0.f;
        for (size_t i = 1; i < path.size(); ++i) len += HorizDist(path[i - 1], path[i]);
        if (len >= best) continue;
        best = len;
        goal.targetActor = c.atv;
        goal.targetPos = c.pos;
        lenOut = len;
        idxOut = R::InternalIndexOf(c.atv);
    }
    if (!goal.targetActor)
        UE_LOGW("[CVIEW-DRILL] client at (%.0f,%.0f,%.0f): %zu ATV(s), none with a route that ends beside it", at.X,
                at.Y, at.Z, g.cands.size());
    return goal.targetActor != nullptr;
}

int WalkSeconds(float routeCm) { return std::clamp(static_cast<int>(routeCm / 100.f) + 60, 90, 900); }

// What StartWalk hands its walker: the walk, the generation it belongs to, and the director's walk epoch read on
// the game thread as it started, so the walk ends with the session that asked for it.
struct WalkArg {
    Walk     walk;
    uint32_t gen;
    uint32_t epoch;
};

// The walks, with the director: to the ATV, then back to where the client started.
DWORD WINAPI WalkerThread(LPVOID arg) {
    const WalkArg a = *std::unique_ptr<WalkArg>(static_cast<WalkArg*>(arg));
    const Walk walk = a.walk;
    const uint32_t gen = a.gen;
    auto goal = std::make_shared<DR::DirectorGoal>();
    goal->reachCm = kStandCm;
    goal->epoch = a.epoch;
    auto len = std::make_shared<float>(0.f);
    auto idx = std::make_shared<int32_t>(-1);
    if (walk == Walk::ToAtv) {
        const int picked = GT::RunAndWait([goal, len, idx](std::atomic<int>& done) {
            void* p = coop::players::Registry::Get().Local();
            FVector at{};
            if (!p || !R::IsLive(p) || !E::GetController(p) || !E::TryGetActorLocation(p, at)) {
                done.store(2);
                return;
            }
            done.store(PickAtv(p, at, *goal, *len, *idx) ? 1 : 2);
        });
        if (picked != 1) {
            Publish(gen, 2, nullptr, -1);
            return 0;
        }
    } else {
        goal->targetPos = g_start;  // written by the game thread before this thread started
    }
    UE_LOGI("[CVIEW-DRILL] client walks %s (%.0f,%.0f,%.0f)", walk == Walk::ToAtv ? "to the ATV at" : "back to",
            goal->targetPos.X, goal->targetPos.Y, goal->targetPos.Z);
    DR::ControlManager mgr;
    DR::AddWalkToProcesses(mgr, *goal);
    // The walk back is bounded as a long route: its length is known only to the navmesh.
    const bool arrived = mgr.Run(*goal, WalkSeconds(walk == Walk::ToAtv ? *len : 30000.f)) && goal->reached;
    Publish(gen, arrived ? 1 : 2, goal->targetActor, *idx);
    return 0;
}

void StartWalk(Walk walk) {
    uint32_t gen = 0;
    {
        std::lock_guard<std::mutex> lk(g_walkMu);
        g_walk.result = 0;
        gen = g_walk.gen;
    }
    auto* arg = new WalkArg{walk, gen, DR::WalkEpoch()};
    HANDLE h = ::CreateThread(nullptr, 0, &WalkerThread, arg, 0, nullptr);
    if (!h) {
        delete arg;
        // No walker, no walk: the outcome says so at once, rather than the drill waiting on a result
        // nothing will publish.
        UE_LOGW("[CVIEW-DRILL] client: the walker thread did not start (error %lu)", ::GetLastError());
        Publish(gen, 2, nullptr, -1);
        return;
    }
    ::CloseHandle(h);
}

void ClientTick() {
    if (g_phase == Phase::Unpicked) {
        if (!E::TryGetActorLocation(coop::players::Registry::Get().Local(), g_start)) {
            Done("the player's place is unreadable -- INCONCLUSIVE");
            return;
        }
        StartWalk(Walk::ToAtv);
        g_phase = Phase::WalkingTo;
        return;
    }
    if (g_phase == Phase::WalkingTo) {
        const WalkOutcome w = ReadWalk();
        if (w.result == 0) return;
        if (w.result == 2) {
            Done("no ATV reached -- INCONCLUSIVE");
            return;
        }
        g_atv = w.arrivedAt;
        g_atvIdx = w.pickedIdx;
        if (!R::IsLiveByIndex(g_atv, g_atvIdx)) {
            Done("the ATV is gone after the walk -- INCONCLUSIVE");
            return;
        }
        g_container = ue_wrap::container_openers::Opens(g_atv);
        float m = 0.f;
        if (!g_container || !Margin(m)) {
            Done("the ATV opens no readable container -- FAIL");
            return;
        }
        if (!CV::Open(g_container)) {
            Done("openPropInv did not dispatch -- FAIL");
            return;
        }
        UE_LOGI("[CVIEW-DRILL] client opened %ls through the ATV, standing %.0f uu inside the reach",
                R::ClassNameOf(g_container).c_str(), -m);
        g_phase = Phase::Opened;
        return;
    }
    float m = 0.f;
    const bool haveMargin = Margin(m);
    const bool open = CV::Viewed() == g_container;
    if (g_phase == Phase::Opened) {
        if (!open) {
            Done("the screen does not show the ATV's container after openPropInv -- FAIL");
            return;
        }
        if (!haveMargin || m > 0.f) {
            Done("the ATV is not within reach where the view was opened -- INCONCLUSIVE");
            return;
        }
        StartWalk(Walk::Back);
        g_phase = Phase::WalkingAway;
        return;
    }
    if (open) {
        if (haveMargin && m > 0.f) ++g_openBeyond;
        else ++g_openWithin;
        g_lastMargin = haveMargin ? m : g_lastMargin;
        if (ReadWalk().result == 0) return;
        if (haveMargin && m <= kLeaveCm) {
            Done("the walk back ended within the reach's neighbourhood, the view open -- INCONCLUSIVE");
            return;
        }
        Done("the walk back ended past the reach with the view still open -- FAIL");
        return;
    }
    // Closed. The close rule ticks after this drill in the same frame, so the view reads open on at most
    // one tick past the reach: the one on which the rule then closes it.
    if (!haveMargin) {
        Done("the view closed, the reach unreadable at that tick -- INCONCLUSIVE");
        return;
    }
    if (m <= 0.f) {
        UE_LOGW("[CVIEW-DRILL] client: the view closed %.0f uu inside the reach", -m);
        Done("closed while the ATV was within reach -- FAIL");
        return;
    }
    UE_LOGI("[CVIEW-DRILL] client: the view closed %.0f uu past the reach (last open tick %.0f uu)", m, g_lastMargin);
    Done(g_openBeyond <= 1 ? "closed on the tick the ATV left the reach -- PASS"
                           : "closed ticks after the ATV left the reach -- FAIL");
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::container_view_drill);
    return s;
}

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || g_phase == Phase::Done) return;
    if (!session || !session->connected() || !RoleIsReady()) return;
    if (coop::roster::LocalIsHost()) return;
    ClientTick();
}

void OnDisconnect() {
    if (!IsEnabled()) return;
    // A walker still running finishes its walk; its result is dropped (the generation moves on).
    {
        std::lock_guard<std::mutex> lk(g_walkMu);
        ++g_walk.gen;
        g_walk.result = 0;
        g_walk.arrivedAt = nullptr;
        g_walk.pickedIdx = -1;
    }
    g_phase = Phase::Unpicked;
    g_atv = nullptr;
    g_atvIdx = -1;
    g_container = nullptr;
    g_openWithin = g_openBeyond = 0;
    g_lastMargin = 0.f;
}

}  // namespace coop::dev::container_view_drill

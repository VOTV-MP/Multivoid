// coop/dev/container_intent_drill.cpp -- see coop/dev/container_intent_drill.h.

#include "coop/dev/container_intent_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/container_act_host.h"
#include "coop/props/container_act_request.h"
#include "coop/props/container_act_state.h"
#include "coop/props/container_contents_sync.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::container_intent_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace CV = ue_wrap::container_view;
namespace SR = ue_wrap::save_record;
namespace DR = coop::director;
namespace CC = coop::props::container_contents_sync;
namespace AR = coop::props::container_act_request;
namespace AH = coop::props::container_act_host;
namespace AS = coop::props::container_act_state;
using ue_wrap::FVector;

constexpr float    kStandCm      = 150.f;   // this near the container, a player stands where it can open it
constexpr uint64_t kPickEveryMs  = 1000;
constexpr uint64_t kPickFailMs   = 60000;
constexpr uint64_t kOpenFailMs   = 10000;   // the screen shows the container within this after openPropInv
constexpr uint64_t kCommitFailMs = 15000;   // the take's commit goes out within this after the press
constexpr uint64_t kSettleMs     = 1000;    // the counts are read this long after the commit
constexpr uint64_t kHostFailMs   = 90000;   // the host sees the take land within this
constexpr uint64_t kHostQuietMs  = 3000;    // no hold stands this long after the count fell
constexpr uint64_t kHoldsEveryMs = 1000;
constexpr int      kMinRecords   = 2;

enum class Phase { Picking, Walking, Opening, Pressed, Settling, Watching, Done };

Phase    g_phase = Phase::Picking;
uint64_t g_pickStartMs = 0;
uint64_t g_nextPickMs = 0;

// The target, picked alike on both peers.
uint32_t g_eid = 0;
void*    g_actor = nullptr;
int32_t  g_actorIdx = -1;
FVector  g_pos{};
int      g_records0 = 0;

// The client.
uint64_t g_phaseStartMs = 0;
uint64_t g_pressMs = 0;
uint64_t g_commitMs = 0;
int      g_containerBefore = -1;
int      g_ownBefore = -1;
uint32_t g_takesBefore = 0;
uint32_t g_committed = 0;

// The host.
uint64_t g_readyMs = 0;     // the first tick with a world-ready peer
int      g_lastCount = -1;
uint64_t g_fallMs = 0;
uint64_t g_nextHoldsMs = 0;
std::string g_lastHolds;

// What a walker thread hands the game thread, under one lock with the session generation the walk was started
// in: a walker still running at a disconnect finishes, and publishes nothing into the next session.
struct WalkOutcome {
    uint32_t gen = 0;
    int result = 0;   // 0 walking, 1 arrived, 2 no route, 3 no arrival
};
std::mutex g_walkMu;
WalkOutcome g_walk;  // guarded by g_walkMu

void Publish(uint32_t gen, int result) {
    std::lock_guard<std::mutex> lk(g_walkMu);
    if (g_walk.gen != gen) return;
    g_walk.result = result;
}

int ReadWalk() {
    std::lock_guard<std::mutex> lk(g_walkMu);
    return g_walk.result;
}

bool RoleIsReady() {
    if (coop::roster::LocalIsHost()) return true;
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle;
}

const char* Role() { return coop::roster::LocalIsHost() ? "host" : "client"; }

void Finish() {
    g_phase = Phase::Done;
    UE_LOGI("[CI-DRILL] %s DONE", Role());
}

void Fail(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    UE_LOGW("[CI-DRILL] %s take FAIL: %s", Role(), buf);
    Finish();
}

int CountOf(uint32_t eid) {
    int32_t n = -1;
    float vol = 0.f;
    return CC::ContentsDigest(eid, n, vol) ? n : -1;
}

void Collect(uint32_t eid, void* ctx) { static_cast<std::vector<uint32_t>*>(ctx)->push_back(eid); }

// The smallest element id holding two records or more that is not the player's own inventory. `seen` is the
// containers walked, `qualified` those holding enough.
bool TryPick(int& seen, int& qualified) {
    static std::vector<uint32_t> eids;
    eids.clear();
    CC::ForEachWorldContainer(&Collect, &eids);
    seen = static_cast<int>(eids.size());
    qualified = 0;
    uint32_t best = 0;
    for (const uint32_t eid : eids) {
        if (CountOf(eid) < kMinRecords) continue;
        void* const actor = coop::element::LivePropActor(eid);
        if (!actor || CV::IsOwnInventory(actor)) continue;
        ++qualified;
        if (!best || eid < best) best = eid;
    }
    if (!best) return false;
    void* const actor = coop::element::LivePropActor(best);
    FVector at{};
    if (!actor || !E::TryGetActorLocation(actor, at)) return false;
    g_eid = best;
    g_actor = actor;
    g_actorIdx = R::InternalIndexOf(actor);
    g_pos = at;
    g_records0 = CountOf(best);
    UE_LOGI("[CI-DRILL] %s target eid=%u records=%d class=%ls at (%.0f,%.0f,%.0f)", Role(), best, g_records0,
            R::ClassNameOf(actor).c_str(), at.X, at.Y, at.Z);
    return true;
}

// The inventory screen: the gamemode's propInventory.
void* Screen() {
    void* gm = ue_wrap::world_singleton::Gamemode();
    const int32_t off = gm ? R::FindPropertyOffset(R::ClassOf(gm), L"propInventory") : -1;
    void* ui = off >= 0 ? *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(gm) + off) : nullptr;
    return ui && R::IsLive(ui) ? ui : nullptr;
}

// The screen's first container slot, as the take probe reads slots_prop[0].
void* FirstSlot() {
    void* const screen = Screen();
    const int32_t off = screen ? R::FindPropertyOffset(R::ClassOf(screen), L"slots_prop") : -1;
    if (off < 0) return nullptr;
    const SR::Arr arr = SR::ReadArr(screen, off);
    void* slot = nullptr;
    if (arr.num > 0 && arr.data) std::memcpy(&slot, arr.data, sizeof(slot));
    return slot && R::IsLive(slot) ? slot : nullptr;
}

int WalkSeconds(float routeCm) { return std::clamp(static_cast<int>(routeCm / 100.f) + 60, 90, 900); }

struct WalkArg {
    uint32_t gen;
    uint32_t epoch;
    void*    actor;
    FVector  pos;
    uint32_t eid;
};

// The walk, with the director: to the container.
DWORD WINAPI WalkerThread(LPVOID arg) {
    const WalkArg a = *std::unique_ptr<WalkArg>(static_cast<WalkArg*>(arg));
    auto goal = std::make_shared<DR::DirectorGoal>();
    goal->reachCm = kStandCm;
    goal->epoch = a.epoch;
    goal->targetActor = a.actor;
    goal->targetPos = a.pos;
    auto len = std::make_shared<float>(0.f);
    const int routed = GT::RunAndWait([goal, len](std::atomic<int>& done) {
        void* p = coop::players::Registry::Get().Local();
        FVector at{};
        if (!p || !R::IsLive(p) || !E::GetController(p) || !E::TryGetActorLocation(p, at)) {
            done.store(2);
            return;
        }
        done.store(DR::RouteInLegs(p, at, goal->targetPos, kStandCm, len.get()) ? 1 : 2);
    });
    if (routed != 1) {
        Publish(a.gen, 2);
        return 0;
    }
    UE_LOGI("[CI-DRILL] client walks to eid=%u at (%.0f,%.0f,%.0f), route %.0f cm", a.eid, a.pos.X, a.pos.Y, a.pos.Z,
            static_cast<double>(*len));
    DR::ControlManager mgr;
    DR::AddWalkToProcesses(mgr, *goal);
    const bool arrived = mgr.Run(*goal, WalkSeconds(*len)) && goal->reached;
    Publish(a.gen, arrived ? 1 : 3);
    return 0;
}

void StartWalk() {
    uint32_t gen = 0;
    {
        std::lock_guard<std::mutex> lk(g_walkMu);
        g_walk.result = 0;
        gen = g_walk.gen;
    }
    auto* arg = new WalkArg{gen, DR::WalkEpoch(), g_actor, g_pos, g_eid};
    HANDLE h = ::CreateThread(nullptr, 0, &WalkerThread, arg, 0, nullptr);
    if (!h) {
        delete arg;
        UE_LOGW("[CI-DRILL] client: the walker thread did not start (error %lu)", ::GetLastError());
        Publish(gen, 2);
        return;
    }
    ::CloseHandle(h);
}

// The press: the screen's first slot is read, the counts before are taken, and the slot is pressed as a click does.
void PressFirstSlot(uint64_t now) {
    void* const slot = FirstSlot();
    CV::Slot s{};
    if (!slot || !CV::ReadSlot(slot, s)) {
        Fail("the open screen has no readable first container slot");
        return;
    }
    UE_LOGI("[CI-DRILL] client first slot id=%d", s.id);
    g_containerBefore = CountOf(g_eid);
    g_ownBefore = ue_wrap::inventory::LivePersonalStoreCount();
    g_takesBefore = AR::dev::LastTake().count;
    g_pressMs = now;
    const bool pressed = CV::PressSlot(slot);
    const uint16_t pending = AR::dev::PendingId();
    UE_LOGI("[CI-DRILL] client pressed=%d pending id=%u container %d own %d", pressed ? 1 : 0,
            static_cast<unsigned>(pending), g_containerBefore, g_ownBefore);
    if (!pressed) {
        Fail("the slot's pressButton did not dispatch");
        return;
    }
    if (!pending && AR::dev::LastTake().count == g_takesBefore) {
        Fail("the press opened no act (the seat did not cancel it, or the ask was refused)");
        return;
    }
    g_phase = Phase::Pressed;
}

void ClientTick() {
    const uint64_t now = coop::net::NowMs();
    switch (g_phase) {
    case Phase::Picking: {
        if (!g_pickStartMs) g_pickStartMs = now;
        if (now < g_nextPickMs) return;
        g_nextPickMs = now + kPickEveryMs;
        int seen = 0, qualified = 0;
        if (TryPick(seen, qualified)) {
            StartWalk();
            g_phase = Phase::Walking;
            return;
        }
        if (now - g_pickStartMs >= kPickFailMs)
            Fail("no world container holds %d records or more after 60 s (%d candidates of %d containers)",
                 kMinRecords, qualified, seen);
        return;
    }
    case Phase::Walking: {
        const int w = ReadWalk();
        if (w == 0) return;
        if (w != 1) {
            Fail(w == 2 ? "no route reaches the container, or the player is not readable"
                        : "the walk to the container did not arrive");
            return;
        }
        if (!R::IsLiveByIndex(g_actor, g_actorIdx)) {
            Fail("the container is gone after the walk");
            return;
        }
        if (!CV::Open(g_actor)) {
            Fail("openPropInv did not dispatch");
            return;
        }
        g_phaseStartMs = now;
        g_phase = Phase::Opening;
        return;
    }
    case Phase::Opening:
        if (CV::Viewed() == g_actor) {
            UE_LOGI("[CI-DRILL] client opened eid=%u after %llu ms", g_eid,
                    static_cast<unsigned long long>(now - g_phaseStartMs));
            PressFirstSlot(now);
            return;
        }
        if (now - g_phaseStartMs >= kOpenFailMs) Fail("the screen does not show the container 10 s after openPropInv");
        return;
    case Phase::Pressed: {
        const AR::dev::Take t = AR::dev::LastTake();
        if (t.count != g_takesBefore) {
            g_committed = t.committed;
            g_commitMs = now;
            g_phase = Phase::Settling;
            return;
        }
        if (now - g_pressMs >= kCommitFailMs)
            Fail("no commit 15 s after the press (pending id=%u)", static_cast<unsigned>(AR::dev::PendingId()));
        return;
    }
    case Phase::Settling: {
        if (now - g_commitMs < kSettleMs) return;
        const int containerAfter = CountOf(g_eid);
        const int ownAfter = ue_wrap::inventory::LivePersonalStoreCount();
        UE_LOGI("[CI-DRILL] client take committed=%u ms=%llu container %d -> %d own %d -> %d unread=%u", g_committed,
                static_cast<unsigned long long>(g_commitMs - g_pressMs), g_containerBefore, containerAfter,
                g_ownBefore, ownAfter, AR::dev::TakeDoneUnread());
        const bool fell = g_containerBefore >= 0 && containerAfter >= 0 && g_containerBefore - containerAfter == 1;
        const bool rose = g_ownBefore >= 0 && ownAfter >= 0 && ownAfter - g_ownBefore == 1;
        if (g_committed == 1 && fell && rose) {
            UE_LOGI("[CI-DRILL] client take PASS");
            Finish();
            return;
        }
        Fail("committed=%u, the container went %d -> %d, the own inventory %d -> %d", g_committed, g_containerBefore,
             containerAfter, g_ownBefore, ownAfter);
        return;
    }
    default:
        return;
    }
}

// The act holds and the host's tallies as one line; printed when it changes.
std::string HoldsLine() {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "holds=%zu", AS::Live().Count());
    std::string line = buf;
    for (int i = 0; i < static_cast<int>(AH::Tally::kCount); ++i) {
        const uint32_t n = AH::TallyOf(static_cast<AH::Tally>(i));
        if (!n) continue;
        std::snprintf(buf, sizeof(buf), " %s=%u", AH::TallyName(static_cast<AH::Tally>(i)), n);
        line += buf;
    }
    return line;
}

bool AnyPeerWorldReady(const coop::net::Session& s) {
    for (int slot = 1; slot < coop::net::kMaxPeers; ++slot)
        if (s.IsSlotWorldReady(slot)) return true;
    return false;
}

void HostTick(const coop::net::Session& session) {
    const uint64_t now = coop::net::NowMs();
    if (g_phase == Phase::Picking) {
        if (!g_pickStartMs) g_pickStartMs = now;
        if (now < g_nextPickMs) return;
        g_nextPickMs = now + kPickEveryMs;
        int seen = 0, qualified = 0;
        if (TryPick(seen, qualified)) {
            g_lastCount = g_records0;
            g_phase = Phase::Watching;
            return;
        }
        if (now - g_pickStartMs >= kPickFailMs)
            Fail("no world container holds %d records or more after 60 s (%d candidates of %d containers)",
                 kMinRecords, qualified, seen);
        return;
    }
    if (g_phase != Phase::Watching) return;
    if (!g_readyMs && AnyPeerWorldReady(session)) g_readyMs = now;

    const int count = CountOf(g_eid);
    if (count >= 0 && count != g_lastCount) {
        UE_LOGI("[CI-DRILL] host count eid=%u %d -> %d at ms=%llu", g_eid, g_lastCount, count,
                static_cast<unsigned long long>(now));
        g_lastCount = count;
        g_fallMs = count < g_records0 ? now : 0;
    }
    if (now >= g_nextHoldsMs) {
        g_nextHoldsMs = now + kHoldsEveryMs;
        std::string line = HoldsLine();
        if (line != g_lastHolds) {
            UE_LOGI("[CI-DRILL] host acts at ms=%llu: %s", static_cast<unsigned long long>(now), line.c_str());
            g_lastHolds = std::move(line);
        }
    }
    if (count >= 0 && count < g_records0 - 1) {
        Fail("the count fell by %d (%d -> %d)", g_records0 - count, g_records0, count);
        return;
    }
    if (g_fallMs && now - g_fallMs >= kHostQuietMs && AS::Live().Count() == 0) {
        UE_LOGI("[CI-DRILL] host take PASS");
        Finish();
        return;
    }
    if (g_readyMs && now - g_readyMs >= kHostFailMs)
        Fail("no take landed within 90 s of the peer's world (count %d of %d, holds %zu)", g_lastCount, g_records0,
             AS::Live().Count());
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveEnum(::coop::config_registry::rows::container_intent_drill) == "take";
    return s;
}

void Tick(coop::net::Session* session) {
    if (!IsEnabled() || g_phase == Phase::Done) return;
    if (!session || !session->connected() || !RoleIsReady()) return;
    if (coop::roster::LocalIsHost()) HostTick(*session);
    else ClientTick();
}

void OnDisconnect() {
    if (!IsEnabled()) return;
    // A walker still running finishes its walk; its result is dropped (the generation moves on).
    {
        std::lock_guard<std::mutex> lk(g_walkMu);
        ++g_walk.gen;
        g_walk.result = 0;
    }
    g_phase = Phase::Picking;
    g_pickStartMs = g_nextPickMs = 0;
    g_eid = 0;
    g_actor = nullptr;
    g_actorIdx = -1;
    g_pos = FVector{};
    g_records0 = 0;
    g_phaseStartMs = g_pressMs = g_commitMs = 0;
    g_containerBefore = g_ownBefore = -1;
    g_takesBefore = g_committed = 0;
    g_readyMs = 0;
    g_lastCount = -1;
    g_fallMs = g_nextHoldsMs = 0;
    g_lastHolds.clear();
}

}  // namespace coop::dev::container_intent_drill

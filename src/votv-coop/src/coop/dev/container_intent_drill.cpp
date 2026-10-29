// coop/dev/container_intent_drill.cpp -- see coop/dev/container_intent_drill.h.

#include "coop/dev/container_intent_drill.h"
#include "container_intent_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/roster.h"
#include "coop/props/container_act_host.h"
#include "coop/props/container_act_request.h"
#include "coop/props/container_apply_hold.h"
#include "coop/props/container_contents_sync.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace coop::dev::container_intent_drill {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;
namespace CV = ue_wrap::container_view;
namespace DR = coop::director;
namespace CC = coop::props::container_contents_sync;
namespace CAH = coop::props::container_apply_hold;
namespace AR = coop::props::container_act_request;
namespace AH = coop::props::container_act_host;
using ue_wrap::FVector;

namespace detail {

namespace {
struct ArmRow {
    const char* name;
    Arm arm;
};
constexpr ArmRow kArms[] = {{"take", Arm::Take},     {"settle", Arm::Settle}, {"abort", Arm::Abort},
                            {"deadline", Arm::Deadline}, {"double", Arm::Double}, {"ghost", Arm::Ghost},
                            {"held", Arm::Held},     {"race", Arm::Race},     {"two", Arm::Two},
                            {"sale", Arm::Sale},     {"depart", Arm::Depart}, {"room", Arm::Room},
                            {"putrecord", Arm::PutRecord}, {"puthand", Arm::PutHand}, {"putfood", Arm::PutFood},
                            {"puthidden", Arm::PutHidden}, {"putnested", Arm::PutNested}, {"putempty", Arm::PutEmpty},
                            {"puthover", Arm::PutHover}, {"putreserved", Arm::PutReserved}, {"putfull", Arm::PutFull},
                            {"putfullhand", Arm::PutFullHand}, {"putdepart", Arm::PutDepart},
                            {"putdeadline", Arm::PutDeadline}, {"putowed", Arm::PutOwed}, {"putjoin", Arm::PutJoin},
                            {"throwhand", Arm::ThrowHand}, {"throwgrab", Arm::ThrowGrab}, {"throwhostgrab", Arm::ThrowHostGrab},
                            {"throwclamp", Arm::ThrowClamp}};
}  // namespace

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::container_intent_drill);
        for (const ArmRow& r : kArms)
            if (v == r.name) return r.arm;
        return Arm::Off;
    }();
    return a;
}

const char* ArmName() {
    for (const ArmRow& r : kArms)
        if (r.arm == ArmOf()) return r.name;
    return "off";
}

bool NamesR() {
    const Arm a = ArmOf();
    return a == Arm::Race || a == Arm::Two || a == Arm::Sale || a == Arm::Room;
}

Phase    g_phase = Phase::Picking;
uint64_t g_pickStartMs = 0;
uint64_t g_nextPickMs = 0;

// The target, picked alike on both peers.
uint32_t g_eid = 0;
void*    g_actor = nullptr;
int32_t  g_actorIdx = -1;
FVector  g_pos{};
int      g_records0 = 0;
uint64_t g_r0 = 0;
bool     g_r0Unique = false;
ItemKey  g_r0Key;

const char* Role() { return coop::roster::LocalIsHost() ? "host" : "client"; }

// Every knob an arm armed, put back: a parked replay runs its checks and commits (the host's escrow ends), the kept
// publications apply, the held reply goes out. Each call is inert on the role that armed nothing.
void Disarm() {
    AR::dev::HoldReplay(false);
    AR::dev::DoubleAbort(false);
    AR::dev::ReleaseReplay();
    CAH::Release();
    AH::dev::HoldReplies(false);
    AH::dev::ReleaseReplies();
    AH::dev::NoEscrow(false);
    AH::dev::NoReservation(false);   // the put arms' knobs and counters, whichever way the arm ended
    AH::dev::NoLapse(false);
    AH::dev::InsertNotReady(0);
    AH::dev::CountPutJudgements(false);
    AR::dev::DoubleDone(false);
    AR::dev::NoHoverGuard(false);
    AR::dev::NoHandTable(false);
    RoomRestore();
}

void Finish() {
    Disarm();
    g_phase = Phase::Done;
    UE_LOGI("[CI-DRILL] %s DONE", Role());
}

void Fail(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    UE_LOGW("[CI-DRILL] %s %s FAIL: %s", Role(), ArmName(), buf);
    Finish();
}

int CountOf(uint32_t eid) {
    int32_t n = -1;
    float vol = 0.f;
    return CC::ContentsDigest(eid, n, vol) ? n : -1;
}

}  // namespace detail

using namespace detail;

namespace {

constexpr uint64_t kOpenFailMs   = 10000;   // the screen shows the container within this after openPropInv
constexpr uint64_t kCommitFailMs = 15000;   // the take's commit goes out within this after the press
constexpr uint64_t kSettleMs     = 1000;    // the counts are read this long after the commit
constexpr uint64_t kSettlePressMs = 5000;   // settle and two: the press comes this long after the screen opened
constexpr uint64_t kSecondTakerPressMs = 10000;  // two: the higher client's press, after the lower one's grant
constexpr uint64_t kBackMs       = 2000;    // a mirror is back at the host's count within this
constexpr uint64_t kHeldHoldMs   = 20000;   // held: the parked replay is released this long after it parked
constexpr uint64_t kDeadlineHoldMs = 32000; // deadline: this long, past the host's 30 s hold
constexpr uint64_t kRaceHoldMs   = 8000;    // race: the parked replay waits this long for the host's press (the client cannot see it)
constexpr uint64_t kRaceSettleMs = 3000;    // race and two: the own inventory is read this long after the commit or the refusal
constexpr float    kNearCm       = 300.f;   // the props counted around the player
constexpr int      kGhostTries   = 30;      // nearest props tried as the ghost's record

// The arms that park the grant's replay.
bool Parks() {
    const Arm a = ArmOf();
    return a == Arm::Abort || a == Arm::Double || a == Arm::Deadline || a == Arm::Held || a == Arm::Race ||
           a == Arm::Depart;
}

// The arms that keep the host's contents publications away from the mirror: while the replay is parked, or (sale)
// from the press until the commit, so the replay runs on the copy from before the grant.
bool HoldsApplies() { return ArmOf() == Arm::Held || ArmOf() == Arm::Race || ArmOf() == Arm::Sale; }

// Two: the mirror keeps the pre-take contents from the pick until the answer, so both clients name the same record.
bool HoldsFromPick() { return ArmOf() == Arm::Two; }

// Two and sale: the kept publications apply at the answer, a commit or a refusal.
bool ReleasesAtAnswer() { return ArmOf() == Arm::Two || ArmOf() == Arm::Sale; }

// The arms whose client verdict is the record R in the own inventory.
bool JudgesR() { return ArmOf() == Arm::Race || ArmOf() == Arm::Two || ArmOf() == Arm::Sale; }

// The client.
uint64_t g_phaseStartMs = 0;
uint64_t g_pressMs = 0;
uint64_t g_commitMs = 0;
uint64_t g_parkedMs = 0;    // the grant's replay parked
uint64_t g_waitFromMs = 0;  // the commit's bound counts from here
int      g_containerBefore = -1;
int      g_ownBefore = -1;
int      g_propsBefore = -1;
int      g_ghostSlotId = -1;
uint32_t g_takesBefore = 0;
uint32_t g_committed = 0;
uint64_t g_rHash = 0;       // race and two: the hash of the record the press names, as the container holds it
ItemKey  g_rKey;            // its class and name
int      g_ownRBefore = -1; // and how many of the own inventory's records have them before the press
uint8_t  g_refusal = 0;     // two: the reason of the refusal that ended the press, 0 when the host granted
uint64_t g_bothMs = 0;      // two: the first tick both clients were present
coop::net::Session* g_session = nullptr;   // the last session Tick saw, for the disconnect's host rule


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

// Two: the host and two clients are in the roster, the local one world-ready (RoleIsReady) and the other one's puppet
// alive here, which spawns on its first pose after its own world is up.
bool BothClientsPresent() {
    coop::roster::Snapshot r;
    coop::roster::GetSnapshot(r);
    bool host = false;
    int clients = 0;
    for (int i = 0; i < r.count; ++i) {
        const coop::roster::Row& row = r.rows[i];
        if (!row.connected) continue;
        if (row.isHost) host = true;
        else if (row.isLocal) ++clients;
        else if (row.slot >= 1 && coop::players::Registry::Get().LifeOf(static_cast<uint8_t>(row.slot)) ==
                                      coop::players::PuppetLife::Alive)
            ++clients;
    }
    return host && clients >= 2;
}

// Two: the local client has the lowest slot among the connected clients.
bool IsLowerClient() {
    coop::roster::Snapshot r;
    coop::roster::GetSnapshot(r);
    int mine = -1, lowest = -1;
    for (int i = 0; i < r.count; ++i) {
        const coop::roster::Row& row = r.rows[i];
        if (!row.connected || row.isHost || row.slot < 1) continue;
        if (row.isLocal) mine = row.slot;
        if (lowest < 0 || row.slot < lowest) lowest = row.slot;
    }
    return mine >= 0 && mine == lowest;
}

// The live props within kNearCm of the player: what a take that spawned an item would have grown.
int PropsNearPlayer() {
    static std::vector<coop::element::Registry::ActorIdPair> pairs;
    void* const p = coop::players::Registry::Get().Local();
    FVector at{};
    if (!p || !E::TryGetActorLocation(p, at)) return -1;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    int n = 0;
    for (const auto& a : pairs) {
        FVector pos{};
        if (!a.actor || !R::IsLiveByIndex(a.actor, a.internalIdx) || !E::TryGetActorLocation(a.actor, pos)) continue;
        const float dx = pos.X - at.X, dy = pos.Y - at.Y, dz = pos.Z - at.Z;
        if (dx * dx + dy * dy + dz * dz <= kNearCm * kNearCm) ++n;
    }
    return n;
}

// The ghost: the nearest keyed world props to the player, in order, until one's record is appended to the mirror.
bool AppendGhost() {
    static std::vector<coop::element::Registry::ActorIdPair> pairs;
    void* const p = coop::players::Registry::Get().Local();
    FVector at{};
    if (!p || !E::TryGetActorLocation(p, at)) return false;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    std::vector<std::pair<float, void*>> cand;
    for (const auto& a : pairs) {
        FVector pos{};
        if (!a.actor || a.actor == g_actor || !R::IsLiveByIndex(a.actor, a.internalIdx) ||
            !E::TryGetActorLocation(a.actor, pos))
            continue;
        const float dx = pos.X - at.X, dy = pos.Y - at.Y, dz = pos.Z - at.Z;
        cand.emplace_back(dx * dx + dy * dy + dz * dz, a.actor);
    }
    std::sort(cand.begin(), cand.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
    for (int i = 0; i < kGhostTries && i < static_cast<int>(cand.size()); ++i)
        if (CC::DevRawAppendActor(g_eid, cand[static_cast<size_t>(i)].second, /*publish=*/false)) return true;
    return false;
}

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

// Presses the slot as a click does. False, with the failure printed, when no act opened.
bool PressAndCheck(void* slot) {
    const bool pressed = CV::PressSlot(slot);
    const uint16_t pending = AR::dev::PendingId();
    UE_LOGI("[CI-DRILL] client pressed=%d pending id=%u container %d own %d", pressed ? 1 : 0,
            static_cast<unsigned>(pending), g_containerBefore, g_ownBefore);
    if (!pressed) {
        Fail("the slot's pressButton did not dispatch");
        return false;
    }
    if (!pending && AR::dev::LastTake().count == g_takesBefore) {
        Fail("the press opened no act (the seat did not cancel it, or the ask was refused)");
        return false;
    }
    return true;
}

// The press of the screen's first slot, with the counts before taken first. The arms that park the grant's replay
// hold it before the press.
void PressFirstSlot(uint64_t now) {
    void* const slot = SlotById(-1);
    CV::Slot s{};
    if (!slot || !CV::ReadSlot(slot, s)) {
        Fail("the open screen has no readable first container slot");
        return;
    }
    UE_LOGI("[CI-DRILL] client first slot id=%d", s.id);
    g_containerBefore = CountOf(g_eid);
    g_ownBefore = ue_wrap::inventory::LivePersonalStoreCount();
    g_propsBefore = PropsNearPlayer();
    g_takesBefore = AR::dev::LastTake().count;
    g_pressMs = g_waitFromMs = now;
    if (JudgesR()) {
        std::vector<uint64_t> hashes;
        std::vector<ItemKey> keys;
        if (!ContainerHashes(g_actor, hashes) || !ContainerKeys(g_actor, keys) || s.id < 0 ||
            static_cast<size_t>(s.id) >= hashes.size() || hashes.size() != keys.size()) {
            Fail("the record the first slot names does not read (slot id %d)", s.id);
            return;
        }
        g_rHash = hashes[static_cast<size_t>(s.id)];
        g_rKey = keys[static_cast<size_t>(s.id)];
        g_ownRBefore = OwnHolds(g_rKey);
        UE_LOGI("[CI-DRILL] client %s presses R=%016llx class=%ls name=%ls, the own inventory holds it %d times",
                ArmName(), static_cast<unsigned long long>(g_rHash), g_rKey.cls.c_str(), g_rKey.name.c_str(),
                g_ownRBefore);
    }
    if (Parks()) {
        AR::dev::HoldReplay(true);
        AR::dev::DoubleAbort(ArmOf() == Arm::Double);
    }
    if (HoldsApplies()) CAH::Hold(g_eid);
    if (ArmOf() == Arm::Room && !RoomFill()) {
        Fail("the own inventory's capacity does not resolve");
        return;
    }
    if (!PressAndCheck(slot)) return;
    g_phase = Parks() ? Phase::Parked : Phase::Pressed;
}

// The ghost: a record the host never had is appended to the mirror, the screen redrawn, and the slot that names it
// pressed in the next phase.
void StartGhost(uint64_t now) {
    g_containerBefore = CountOf(g_eid);
    if (g_containerBefore < 0) {
        Fail("the mirror's count does not read");
        return;
    }
    if (!AppendGhost()) {
        Fail("no nearby prop's record could be appended to the mirror");
        return;
    }
    const int grown = CountOf(g_eid);
    UE_LOGI("[CI-DRILL] client ghost appended: mirror %d -> %d", g_containerBefore, grown);
    if (grown != g_containerBefore + 1) {
        Fail("the mirror holds %d after the append, expected %d", grown, g_containerBefore + 1);
        return;
    }
    if (!CV::Redraw(g_actor)) {
        Fail("the screen did not redraw");
        return;
    }
    g_ghostSlotId = g_containerBefore;
    g_phaseStartMs = now;
    g_phase = Phase::GhostFind;
}

void ClientTick() {
    const uint64_t now = coop::net::NowMs();
    switch (g_phase) {
    case Phase::Picking: {
        if (!g_pickStartMs && ArmOf() == Arm::Depart && DepartRejoined()) {
            UE_LOGI("[CI-DRILL] client depart: rejoined, idle");
            g_phase = Phase::Done;
            return;
        }
        // Two: neither client picks until both are in, so both pick the same untouched contents and the presses meet.
        if (ArmOf() == Arm::Two && !g_bothMs) {
            if (!BothClientsPresent()) return;
            g_bothMs = now;
            UE_LOGI("[CI-DRILL] client two: both clients present at ms=%llu", static_cast<unsigned long long>(now));
        }
        if (!g_pickStartMs) g_pickStartMs = now;
        if (now < g_nextPickMs) return;
        g_nextPickMs = now + kPickEveryMs;
        int seen = 0, qualified = 0;
        if (TryPick(seen, qualified)) {
            if (HoldsFromPick()) CAH::Hold(g_eid);
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
            if (ArmOf() == Arm::Ghost) {
                StartGhost(now);
            } else if (ArmOf() == Arm::Settle || ArmOf() == Arm::Two) {
                g_phaseStartMs = now;
                g_phase = Phase::SettleWait;
            } else {
                PressFirstSlot(now);
            }
            return;
        }
        if (now - g_phaseStartMs >= kOpenFailMs) Fail("the screen does not show the container 10 s after openPropInv");
        return;
    case Phase::SettleWait:
        if (now - g_phaseStartMs >= (ArmOf() == Arm::Two && !IsLowerClient() ? kSecondTakerPressMs : kSettlePressMs))
            PressFirstSlot(now);
        return;
    case Phase::Pressed: {
        const AR::dev::Take t = AR::dev::LastTake();
        if (t.count != g_takesBefore) {
            g_committed = t.committed;
            g_commitMs = now;
            // Sale: the replay ran on the copy from before the grant; the publications kept behind it apply now.
            if (ReleasesAtAnswer()) CAH::Release();
            g_phase = Phase::Settling;
            return;
        }
        // The second taker of two may be told Gone: the host's answer, and no commit follows.
        if (ArmOf() == Arm::Two && AR::dev::LastRefusal()) {
            g_refusal = AR::dev::LastRefusal();
            g_committed = 0;
            g_commitMs = now;
            CAH::Release();
            g_phase = Phase::Settling;
            return;
        }
        if (now - g_waitFromMs >= kCommitFailMs)
            Fail("no commit 15 s after the press (pending id=%u)", static_cast<unsigned>(AR::dev::PendingId()));
        return;
    }
    case Phase::Parked:
        if (AR::dev::ReplayParked()) {
            g_parkedMs = now;
            UE_LOGI("[CI-DRILL] client grant parked after %llu ms", static_cast<unsigned long long>(now - g_pressMs));
            if (ArmOf() == Arm::Depart) {
                // The rig drops this client on the next line and relaunches it; nothing more runs in this life.
                DepartMarkParked();
                UE_LOGI("[CI-DRILL] client depart parked");
                g_phase = Phase::Done;
            } else if (ArmOf() == Arm::Abort || ArmOf() == Arm::Double) {
                // The screen closes under the parked replay, so its check aborts: the click is lost, never the record.
                CV::Close();
                AR::dev::ReleaseReplay();
                g_waitFromMs = now;
                g_phase = Phase::Pressed;
            } else {
                g_phase = Phase::Holding;
            }
            return;
        }
        if (now - g_pressMs >= kCommitFailMs)
            Fail("the grant did not park within 15 s of the press (pending id=%u)",
                 static_cast<unsigned>(AR::dev::PendingId()));
        return;
    case Phase::Holding: {
        // The one clock of these arms: the host's hold deadline is the thing measured.
        const uint64_t hold = ArmOf() == Arm::Deadline ? kDeadlineHoldMs
                              : ArmOf() == Arm::Race   ? kRaceHoldMs
                                                       : kHeldHoldMs;
        if (now - g_parkedMs < hold) return;
        UE_LOGI("[CI-DRILL] client releases the parked replay after %llu ms",
                static_cast<unsigned long long>(now - g_parkedMs));
        AR::dev::ReleaseReplay();
        // The replay runs before the publications kept behind it are applied: the order of the join's drain.
        if (HoldsApplies()) CAH::Release();
        g_waitFromMs = now;
        g_phase = Phase::Pressed;
        return;
    }
    case Phase::Settling: {
        if (JudgesR()) {
            if (now - g_commitMs < kRaceSettleMs) return;
            const int containerAfter = CountOf(g_eid);
            const int ownAfter = OwnHolds(g_rKey);
            const int gained = g_ownRBefore >= 0 && ownAfter >= 0 ? ownAfter - g_ownRBefore : -1;
            UE_LOGI("[CI-DRILL] client %s committed=%u refusal=%u R=%016llx own holds R %d -> %d container %d -> %d",
                    ArmName(), g_committed, static_cast<unsigned>(g_refusal), static_cast<unsigned long long>(g_rHash),
                    g_ownRBefore, ownAfter, g_containerBefore, containerAfter);
            // The record the press named is in the own inventory exactly when the take committed.
            // Two: the refused taker is told Gone, and once its kept publications apply its mirror equals the host's.
            bool ok = g_committed == 1
                          ? gained == 1
                          : (ArmOf() == Arm::Two &&
                             g_refusal == static_cast<uint8_t>(coop::net::ContainerActReason::Gone) && gained == 0 &&
                             containerAfter == g_containerBefore - 1);
            // Sale: the host's clear reaches the mirror once the kept publications apply.
            if (ArmOf() == Arm::Sale) ok = ok && containerAfter == 0;
            if (ok) {
                UE_LOGI("[CI-DRILL] client %s PASS", ArmName());
                Finish();
                return;
            }
            Fail("committed=%u refusal=%u, the own inventory holds R %d -> %d, the container %d -> %d", g_committed,
                 static_cast<unsigned>(g_refusal), g_ownRBefore, ownAfter, g_containerBefore, containerAfter);
            return;
        }
        // Room: the item the failed addObject left a world prop is keyed and counted after the race's wait.
        if (now - g_commitMs < (ArmOf() == Arm::Room ? kRaceSettleMs : kSettleMs)) return;
        const bool backArm = ArmOf() == Arm::Abort || ArmOf() == Arm::Double;
        const int containerAfter = CountOf(g_eid);
        // An abort's mirror is back at the host's count within kBackMs of the commit.
        if (backArm && containerAfter != g_containerBefore && now - g_commitMs < kBackMs) return;
        const int ownAfter = ue_wrap::inventory::LivePersonalStoreCount();
        const int propsAfter = PropsNearPlayer();
        const uint32_t stale = AR::dev::StaleGrantAborts();
        UE_LOGI("[CI-DRILL] client %s committed=%u ms=%llu container %d -> %d own %d -> %d unread=%u stale=%u "
                "props %d -> %d",
                ArmName(), g_committed, static_cast<unsigned long long>(g_commitMs - g_pressMs), g_containerBefore,
                containerAfter, g_ownBefore, ownAfter, AR::dev::TakeDoneUnread(), stale, g_propsBefore, propsAfter);
        if (ArmOf() == Arm::Settle)
            UE_LOGI("[CI-DRILL] client settle reply_ms=%llu", static_cast<unsigned long long>(g_commitMs - g_pressMs));
        const bool fell = g_containerBefore >= 0 && containerAfter >= 0 && g_containerBefore - containerAfter == 1;
        const bool rose = g_ownBefore >= 0 && ownAfter >= 0 && ownAfter - g_ownBefore == 1;
        const bool ownSame = g_ownBefore >= 0 && ownAfter == g_ownBefore;
        bool ok = false;
        switch (ArmOf()) {
        case Arm::Take:
        case Arm::Settle: ok = g_committed == 1 && fell && rose; break;
        case Arm::Held:   ok = g_committed == 1 && fell && rose && stale == 0; break;
        case Arm::Deadline: ok = g_committed == 0 && stale == 1 && ownSame; break;
        case Arm::Abort:
        case Arm::Double: ok = g_committed == 0 && ownSame && containerAfter == g_containerBefore &&
                               propsAfter == g_propsBefore; break;
        case Arm::Room:   ok = g_committed == 1 && fell && ownSame && propsAfter - g_propsBefore == 1; break;
        default: break;
        }
        if (ok) {
            UE_LOGI("[CI-DRILL] client %s PASS", ArmName());
            Finish();
            return;
        }
        Fail("committed=%u stale=%u, the container went %d -> %d, the own inventory %d -> %d, the props near %d -> %d",
             g_committed, stale, g_containerBefore, containerAfter, g_ownBefore, ownAfter, g_propsBefore, propsAfter);
        return;
    }
    case Phase::GhostFind: {
        void* const slot = SlotById(g_ghostSlotId);
        if (slot) {
            g_takesBefore = AR::dev::LastTake().count;
            g_pressMs = g_waitFromMs = now;
            if (!PressAndCheck(slot)) return;
            g_phase = Phase::GhostWait;
            return;
        }
        if (now - g_phaseStartMs >= kOpenFailMs) Fail("the screen shows no slot for the new record 10 s after the redraw");
        return;
    }
    case Phase::GhostWait: {
        const uint8_t reason = AR::dev::LastRefusal();
        if (reason) {
            g_committed = reason;   // the reason is kept here
            g_commitMs = now;
            g_phase = Phase::GhostBack;
            return;
        }
        if (AR::dev::LastTake().count != g_takesBefore) {
            Fail("the host granted the ghost (a take committed %u)", AR::dev::LastTake().committed);
            return;
        }
        if (now - g_waitFromMs >= kCommitFailMs)
            Fail("no refusal 15 s after the press (pending id=%u)", static_cast<unsigned>(AR::dev::PendingId()));
        return;
    }
    case Phase::GhostBack: {
        const int count = CountOf(g_eid);
        if (count != g_containerBefore) {
            if (now - g_commitMs >= kBackMs)
                Fail("the mirror holds %d, the host's %d, %llu ms after the refusal", count, g_containerBefore,
                     static_cast<unsigned long long>(now - g_commitMs));
            return;
        }
        UE_LOGI("[CI-DRILL] client ghost refusal=%u mirror back to %d after %llu ms", g_committed, count,
                static_cast<unsigned long long>(now - g_commitMs));
        if (g_committed != static_cast<uint32_t>(coop::net::ContainerActReason::Gone)) {
            Fail("the ghost was refused with reason %u, expected Gone (%u)", g_committed,
                 static_cast<unsigned>(coop::net::ContainerActReason::Gone));
            return;
        }
        UE_LOGI("[CI-DRILL] client ghost PASS");
        Finish();
        return;
    }
    default:
        return;
    }
}

}  // namespace

bool IsEnabled() { return ArmOf() != Arm::Off; }

void Tick(coop::net::Session* session) {
    if (!IsEnabled()) return;
    g_session = session;
    if (g_phase == Phase::Done) return;
    if (!session || !session->connected() || !RoleIsReady()) return;
    if (IsPutArm(ArmOf())) {
        if (coop::roster::LocalIsHost()) PutHostTick(*session);
        else PutClientTick();
        return;
    }
    if (IsThrowArm(ArmOf())) {
        if (coop::roster::LocalIsHost()) ThrowHostTick(*session);
        else ThrowClientTick(*session);
        return;
    }
    if (coop::roster::LocalIsHost()) HostTick(*session);
    else ClientTick();
}

void OnDisconnect() {
    if (!IsEnabled()) return;
    // A host whose last client left while the session lives on keeps its target and its verdict (the act host's rule).
    if (g_session && g_session->running() && g_session->role() == coop::net::Role::Host) return;
    // A walker still running finishes its walk; its result is dropped (the generation moves on).
    {
        std::lock_guard<std::mutex> lk(g_walkMu);
        ++g_walk.gen;
        g_walk.result = 0;
    }
    RoomRestore();
    PutReset();
    ThrowReset();
    g_phase = Phase::Picking;
    g_pickStartMs = g_nextPickMs = 0;
    g_eid = 0;
    g_actor = nullptr;
    g_actorIdx = -1;
    g_pos = FVector{};
    g_records0 = 0;
    g_phaseStartMs = g_pressMs = g_commitMs = g_parkedMs = g_waitFromMs = 0;
    g_containerBefore = g_ownBefore = g_propsBefore = g_ghostSlotId = -1;
    g_takesBefore = g_committed = 0;
    g_r0 = g_rHash = 0;
    g_r0Key = g_rKey = ItemKey{};
    g_r0Unique = false;
    g_ownRBefore = -1;
    g_refusal = 0;
    g_bothMs = 0;
    HostReset();
}

}  // namespace coop::dev::container_intent_drill

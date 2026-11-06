// coop/dev/mannequin_walker_steps_b.cpp -- see coop/dev/mannequin_walker_steps_b.h.
//
// THE PEERS. Step 3 runs on walker A, steps 6, 5 and 7 on walker B, and the order of the marks is the order of the steps:
//   host: keep_go ................ client: keep_near          (3: the client stands nearer; the host then reads the target for 5 s)
//   host: hit_go ................. client: hit_done           (6: "ok ..." or "nohit <cm> ...", the host judges which it is)
//   host: loss_go ................ client: left, host: loss_seen   (5: the client leaves; the host reads the target)
//   client (second life): rejoin_ready, host: join_expect, client: rejoin_done   (7: the row the client holds for B's key)
// A mark counts only when written since the reader's own start (peer_signal), which is what lets the second life, a new process
// with a new start, read nothing the first life's host wrote before it.

#include "coop/dev/mannequin_walker_steps_b.h"

#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/creatures/walker_state_client.h"
#include "coop/creatures/walker_target.h"
#include "coop/dev/director/director.h"
#include "coop/dev/mannequin_walker_geometry.h"
#include "coop/player/player_hit.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>

namespace coop::dev::mannequin_walker_drill::steps_b {
namespace {

namespace AC  = ue_wrap::ambient_creature;
namespace DIR = coop::director;
namespace E   = ue_wrap::engine;
namespace PS  = harness::autotest::peer_signal;
namespace V   = ue_wrap::vitals;
namespace WSC = coop::walker_state_client;
namespace WT  = coop::walker_target;
using ue_wrap::FVector;

// Step 3: how much nearer to the walker than the host the client must stand (the walker picks the nearest at a read, so a margin
// that a puppet's lag cannot cancel), how long the host reads the target after that, and the most the client's walk toward the
// walker takes (its director walk's own wait).
constexpr float kNearerCm = 2000.f;
constexpr int   kKeepS = 5;
constexpr int   kNearerBoundS = kWalkWaitS;

// Step 6: how long walker B has to reach the client and hit it, and how close to the client it counts as having reached it (a hit
// is an overlap, so a walker farther than this after the bound never got there: that is no measurement).
constexpr int   kHitBoundS = 90;
constexpr float kHitReachCm = 500.f;

// Step 5: how long the host gets, after the client left, to stop hunting it, and how long the client's leaving takes.
constexpr int kLossBoundS = 10;
constexpr int kLeaveBoundS = 15;

// Step 7: the second life from its launch to its world, the state row from that world, and the slack of a peer's own wait on
// the other's mark.
constexpr int kRejoinBoundS = 400;
constexpr int kRowBoundS = 30;
constexpr int kSlackS = 30;

constexpr const char* kRejoinEnv = "VOTVCOOP_MANNEQUIN_DRILL_REJOIN";

// ---- step 3 ----

// The distances the keep step judges, read on the game thread of the peer that holds `walker`: this player's and the other's.
struct Nearness {
    float mine = -1.f;
    float other = -1.f;
    bool  Nearer(float marginCm) const { return mine >= 0.f && other >= 0.f && mine + marginCm <= other; }
};

}  // namespace

bool HostKeep(void* a) {
    PS::Mark("keep_go");
    if (!AwaitClient("keep_near", kNearerBoundS + kSlackS)) return false;
    // The client says it stands nearer; the host's own reads (the ones the walker's pick is made on) must say so too.
    Nearness n;
    OnGameThread([&] {
        Aim(a, false);
        n = Nearness{ClientTo(a), PlayerTo(a)};
    });
    UE_LOGI("mannequin-drill: step 3: the client stands %.0f cm from walker A, the host %.0f cm (the host's reads)", n.mine, n.other);
    if (!n.Nearer(0.f)) {
        char why[200];
        std::snprintf(why, sizeof(why), "the client is not nearer to walker A than the host on the host's reads (%.0f cm against %.0f, "
                      "or unread)", n.mine, n.other);
        return Invalid(why);
    }
    // Every read of the host's walker for kKeepS: its target stays the host. A walker that chooses again at its next read of the
    // players takes the nearest, which is now the client.
    uint8_t seen = WT::TargetOf(a);
    const bool changed = WaitGT("walker A keeps its target while the client stands nearer", kKeepS, [&] {
        Aim(a, false);
        seen = WT::TargetOf(a);
        return seen != 0;
    }, false);
    char what[200];
    std::snprintf(what, sizeof(what), "walker A keeps hunting the host while the client stands nearer to it (target %d)",
                  seen == WT::kNoTarget ? -1 : static_cast<int>(seen));
    if (!Check(!changed, "3", what)) return Fail("3", what);
    OnGameThread([&] { n = Nearness{ClientTo(a), PlayerTo(a)}; });
    if (!n.Nearer(0.f)) return Invalid("the client stopped being nearer to walker A than the host during the keep window");
    return true;
}

bool ClientKeep(const std::string& keyA) {
    if (!PS::Await("keep_go", kWalkWaitS)) return false;
    FVector to{};
    OnGameThread([&] {
        if (void* copy = ByKey(keyA)) E::TryGetActorLocation(copy, to);
    });
    // The walk ends as soon as the client stands nearer to the copy than the host does by kNearerCm, read while it walks (the host
    // puppet's place against this player's), or where it gives up: at the copy's flat reach of its place when the walk began.
    std::shared_ptr<DIR::BackgroundWalk> walk;
    OnGameThread([&] { walk = DIR::StartBackgroundWalk(to, kApproachEndCm, kWalkDeadlineS); });
    Nearness n;
    PS::WaitFor("this client toward walker A, nearer than the host", kNearerBoundS, [&] {
        OnGameThread([&] {
            void* copy = ByKey(keyA);
            n = Nearness{PlayerTo(copy), PeerTo(coop::players::kPeerIdHost, copy)};
        });
        return n.Nearer(kNearerCm) || walk->state.load() != 0;
    }, false);
    DIR::CancelBackgroundWalk(*walk);
    PS::WaitFor("the walk toward walker A lets go", kHuntGraceS, [&] { return walk->state.load() != 0; }, false);
    OnGameThread([&] {
        void* copy = ByKey(keyA);
        n = Nearness{PlayerTo(copy), PeerTo(coop::players::kPeerIdHost, copy)};
    });
    UE_LOGI("mannequin-drill: step 3: this client stands %.0f cm from walker A's copy, the host's puppet %.0f cm", n.mine, n.other);
    if (!n.Nearer(kNearerCm)) {
        char why[220];
        std::snprintf(why, sizeof(why), "this client could not get %.0f cm nearer to walker A than the host: %.0f cm from it, the host's "
                      "puppet %.0f cm (or unread)", kNearerCm, n.mine, n.other);
        return Invalid(why);
    }
    PS::Mark("keep_near");
    return true;
}

namespace {

// ---- step 6 ----

bool HostHit(void* b) {
    uint8_t target = WT::kNoTarget;
    OnGameThread([&] { target = WT::TargetOf(b); });
    if (target != kClientSlot) return Invalid("walker B does not hunt the client when its hit is awaited");
    PS::Mark("hit_go");
    std::string word;
    if (!AwaitClient("hit_done", kHitBoundS + kSlackS, &word)) return false;
    char what[240];
    if (word.rfind("ok", 0) == 0) {
        std::snprintf(what, sizeof(what), "a walker's hit on the client lands on the client (%s)", word.c_str() + 2);
        Check(true, "6", what);
        return true;
    }
    // "nohit <cm> ...": the client waited kHitBoundS and counted none. A walker at the client's shoulder that hit is a hit lost on
    // the way; a walker that never got there is no measurement.
    float cm = -1.f;
    {
        std::istringstream in(word);
        std::string head;
        if (!(in >> head >> cm)) cm = -1.f;
    }
    if (cm >= 0.f && cm < kHitReachCm) {
        std::snprintf(what, sizeof(what), "a walker's hit on the client lands on the client (walker B stood %.0f cm from it for %d s and "
                      "none was counted: %s)", cm, kHitBoundS, word.c_str());
        Check(false, "6", what);
        return Fail("6", what);
    }
    std::snprintf(what, sizeof(what), "walker B did not reach the client within %d s: %.0f cm from it (%s)", kHitBoundS, cm, word.c_str());
    return Invalid(what);
}

bool ClientHit(const std::string& keyB) {
    if (!PS::Await("hit_go", 120)) return false;
    // The view stays away from the copy and the client stands: the walker walks to it, and its hit is the event waited on.
    const uint32_t before = coop::player_hit::AppliedWalkerCount();
    uint32_t hits = before;
    float    health0 = -1.f, lowest = -1.f, cm = -1.f;
    OnGameThread([&] { V::Read(V::Field::Health, &health0); });
    lowest = health0;
    const bool hit = WaitGT("walker B hits this client", kHitBoundS, [&] {
        if (void* c = ByKey(keyB)) {
            Aim(c, false);
            cm = PlayerTo(c);
        }
        float h = -1.f;
        if (V::Read(V::Field::Health, &h) && h >= 0.f) lowest = lowest < 0.f ? h : (std::min)(lowest, h);
        hits = coop::player_hit::AppliedWalkerCount();
        return hits > before && health0 >= 0.f && lowest < health0;
    }, false);
    char word[160];
    if (!hit) {
        std::snprintf(word, sizeof(word), "nohit %.0f hits=%u health=%.0f->%.0f", cm, hits - before, health0, lowest);
        UE_LOGW("mannequin-drill: step 6: no walker hit was counted on this client: %s", word);
        PS::Mark("hit_done", word);
        return true;
    }
    std::snprintf(word, sizeof(word), "ok hits=%u health=%.0f->%.0f", hits - before, health0, lowest);
    UE_LOGI("mannequin-drill: step 6: this client counted the walker's hit: %s", word);
    PS::Mark("hit_done", word);
    return true;
}

// ---- step 5 ----

bool HostLoss(void* b) {
    PS::Mark("loss_go");
    if (!PS::Await("left", kLeaveBoundS + kSlackS)) return false;
    // The client is gone from the session: the walker that hunted it chooses again (walker_target::OnPeerLeft).
    const bool lost = WaitGT("walker B stops hunting the client that left", kLossBoundS, [&] { return WT::TargetOf(b) != kClientSlot; }, false);
    char what[160];
    std::snprintf(what, sizeof(what), "walker B stops hunting the client within %d s of its leaving", kLossBoundS);
    if (!Check(lost, "5", what)) return Fail("5", what);
    PS::Mark("loss_seen");
    return true;
}

// Leaves as a player's quit does, network first, so the host sees the slot empty at once; the rig restarts this process on the
// marker printed after the host has judged the loss.
bool ClientLeave() {
    if (!PS::Await("loss_go", 120)) return false;
    OnGameThread([] {
        auto& s = harness::session_runtime::Session();
        if (s.running()) coop::net_pump::FleeAfterNativeMenuTravel(s, "mannequin drill: life 1 leaves for the rejoin");
    });
    PS::WaitFor("this life's session stopped", kLeaveBoundS, [] { return !harness::session_runtime::Session().running(); }, false);
    PS::Mark("left");
    if (!PS::Await("loss_seen", kLossBoundS + kSlackS)) return false;
    UE_LOGI("mannequin-drill: REJOIN MARKER this client has left walker B's hunt and is restarted by the rig");
    return true;
}

// ---- step 7 ----

bool HostJoin(void* b, const std::string& keyB, int& clientFails) {
    if (!PS::Await("rejoin_ready", kRejoinBoundS)) return false;
    // The joiner is in its world: its state for B went with its ready edge. What the walker says now is what it said then (nobody
    // is near it, so neither its target nor its `looking` has an edge to move them).
    uint8_t target = WT::kNoTarget;
    bool    looking = false;
    OnGameThread([&] {
        target = WT::TargetOf(b);
        looking = AC::ReadWalkerLooking(b);
    });
    PS::Mark("join_expect", keyB);
    std::string word;
    if (!PS::AwaitText("rejoin_done", word, kRowBoundS + kSlackS)) return false;
    int fails = 0, has = 0, rowTarget = 0, rowLooking = 0;
    std::istringstream in(word);
    if (!(in >> fails >> has >> rowTarget >> rowLooking)) return Invalid("the rejoined client's word for step 7 is not a reading");
    clientFails = fails;
    char what[240];
    if (!has) {
        std::snprintf(what, sizeof(what), "the rejoined client holds a state row for walker B (it holds none; the host's walker: target %d "
                      "looking %d)", target == WT::kNoTarget ? -1 : static_cast<int>(target), looking ? 1 : 0);
        Check(false, "7", what);
        return Fail("7", what);
    }
    const bool same = rowTarget == static_cast<int>(target) && (rowLooking != 0) == looking;
    std::snprintf(what, sizeof(what), "the rejoined client's state row for walker B equals the host's walker (row: target %d looking %d; "
                  "host: target %d looking %d)", rowTarget == 0xFF ? -1 : rowTarget, rowLooking, target == WT::kNoTarget ? -1 : static_cast<int>(target),
                  looking ? 1 : 0);
    if (!Check(same, "7", what)) return Fail("7", what);
    return true;
}

}  // namespace

bool HostB(void* b, const std::string& keyB, int& clientFails) {
    return HostHit(b) && HostLoss(b) && HostJoin(b, keyB, clientFails);
}

bool ClientB(const std::string& keyB) { return ClientHit(keyB) && ClientLeave(); }

bool IsRejoinedLife() { return coop::config::ReadEnv(kRejoinEnv) == "1"; }

// The client's second life: a new process, a new load, and the host's state for B sent at its ready edge. It reads the row it holds
// for B's key (the host names the key at its mark) and reports it; the host judges.
void RunRejoin() {
    if (!HostPuppetUp()) {
        Invalid("this rejoined client's world or the host's puppet never came up");
        return;
    }
    PS::Mark("rejoin_ready");
    std::string key;
    if (!PS::AwaitText("join_expect", key, kRowBoundS + kSlackS)) return;
    uint8_t target = 0xFF;
    bool    looking = false;
    const bool has = WaitGT("the joiner holds a state row for walker B", kRowBoundS, [&] { return WSC::RowOf(Widen(key), target, looking); }, false);
    UE_LOGI("mannequin-drill: REJOIN: this client's state row for walker '%s': %s, target %d looking %d", key.c_str(), has ? "held" : "none",
            target == 0xFF ? -1 : static_cast<int>(target), looking ? 1 : 0);
    char word[64];
    std::snprintf(word, sizeof(word), "%d %d %d %d", FailCount(), has ? 1 : 0, static_cast<int>(target), looking ? 1 : 0);
    PS::Mark("rejoin_done", word);
    UE_LOGI("mannequin-drill: REJOIN DONE fails=%d", FailCount());
}

}  // namespace coop::dev::mannequin_walker_drill::steps_b

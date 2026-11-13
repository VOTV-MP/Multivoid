// harness/autotest/autotest_desk_short.cpp -- TEST-ONLY: the base computer's short circuit on both peers.
//
// Every verdict is decided at the event that decides it; a bound passing with none is INVALID.
// Armed by the row desk_short_drill on both peers (the peers' folder is VOTVCOOP_CAN_DIR).
//   client / host  The named peer shorts its own desk as a liquid's hit does (the desk's own shortCircuit); a client
//                  first walks beside the desk with the director. Each peer waits for its desk's `shortCircuiting` to
//                  rise and fall, and reads the power panel's lever mask. The host says `rig: VERDICT desk-short
//                  PASS|FAIL|INVALID`: PASS when both desks shorted and both panels read the same changed mask; FAIL
//                  at once when the actor's short was not announced (red: desk_short_off on the acting peer) or the
//                  host refused the client's ask, and the host then runs no later leg.
//   join           With the rig's --rejoin. The client leaves once its join is over; the host keeps its desk
//                  shorting until the client's world is ready again; the rejoined client
//                  (VOTVCOOP_DESK_SHORT_DRILL_REJOIN=1) must have had its desk shorted at that edge: `rig: VERDICT desk-short-join`. Red: desk_short_replay_off on the host.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/desk_short_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/desk_short.h"
#include "ue_wrap/devices/power_control.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace harness::autotest {
namespace {

namespace PS = harness::autotest::peer_signal;
namespace PC = ue_wrap::power_control;
namespace RV = coop::rig_verdict;
using disc_drill::OnGameThread;

constexpr const char* kTag = "desk-short";
constexpr const char* kTagJoin = "desk-short-join";
constexpr int kStepSec = 120;
constexpr int kRiseSec = 5;     // INVALID bound: a short the lane announced, or the host's refusal, reaching the other desk
constexpr int kMaskSec = 10;    // INVALID bound: the power panel reading the change after the short ended
constexpr int kEndSec = 40;     // INVALID bound: the ten seconds of sparks, and the load behind them
constexpr int kRejoinSec = 20;  // INVALID bound: the replay the host sent, reaching the rejoined client
constexpr float kWalkCm = 250.f;   // the client's walk ends this near the desk's root; the director's stop
constexpr float kStandCm = 600.f;  // the host reads the client's puppet this near, inside the host's judged reach

std::string Arm() { return coop::config::ResolveEnum(coop::config_registry::rows::desk_short_drill); }
bool HostActs() { return Arm() == "host"; }

void* Desk() { return ue_wrap::console_desk::EnsureResolved() ? ue_wrap::console_desk::Instance() : nullptr; }

int Shorting() {
    int v = -1;
    OnGameThread([&] {
        bool on = false;
        if (ue_wrap::desk_short::ReadShortCircuiting(Desk(), on)) v = on ? 1 : 0;
    });
    return v;
}

int PowerMask() {
    int m = -1;
    OnGameThread([&] {
        uint8_t mask = 0;
        void* p = PC::EnsureResolved() ? PC::Panel() : nullptr;
        if (p && PC::ReadPress(p, mask)) m = mask;
    });
    return m;
}

// What the lane did here: the shorts it announced (the host's sent, a client's asked), the requests the host refused,
// and the host's "no" a client heard.
uint32_t Announced() {
    uint32_t n = 0;
    OnGameThread([&] {
        const coop::desk_short_sync::Counts c = coop::desk_short_sync::GetCounts();
        n = c.sent + c.asked;
    });
    return n;
}
uint32_t Refused() {
    uint32_t n = 0;
    OnGameThread([&] { n = coop::desk_short_sync::GetCounts().refused; });
    return n;
}
uint32_t Heard() {
    uint32_t n = 0;
    OnGameThread([&] { n = coop::desk_short_sync::GetCounts().heard; });
    return n;
}

// The client walks beside the desk with the director (a liquid is thrown from the room; the host checks the sender's
// reach from the pose it holds of the client). False: it could not, and has said its INVALID.
bool WalkBesideDesk() {
    ue_wrap::FVector at{};
    bool read = false;
    OnGameThread([&] { read = ue_wrap::engine::TryGetActorLocation(Desk(), at); });
    if (!read) {
        RV::Say(kTag, RV::Outcome::Invalid, "the client's desk position did not read");
        return false;
    }
    if (!meet::WalkToPoint(kTag, at, kWalkCm, "the base computer")) {
        RV::Say(kTag, RV::Outcome::Invalid, "the client could not walk to the base computer");
        return false;
    }
    return true;
}

enum class ActResult : uint8_t { Announced, NotAnnounced, Stopped };

// The actor's short, by the desk's own event: whether the lane announced it, told to the other peer. Stopped: the run
// ended before the short (the walk failed or the host never confirmed the pose), and a verdict was said.
ActResult Act(const char* me, bool client) {
    if (client) {
        const bool walked = WalkBesideDesk();
        PS::Mark("stood", walked ? "ok" : "fail");
        if (!walked) return ActResult::Stopped;
        if (!PS::Await("pose_ok", kStepSec)) return ActResult::Stopped;
    }
    const uint32_t n0 = Announced();
    bool ran = false;
    OnGameThread([&] { ran = ue_wrap::desk_short::CallShortCircuit(Desk()); });
    const bool announced = Announced() > n0;
    UE_LOGI("desk-short: %hs shorted its desk as a liquid's hit does (call=%d), announced=%d", me, ran ? 1 : 0,
            announced ? 1 : 0);
    PS::Mark("acted", announced ? "1" : "0");
    return announced ? ActResult::Announced : ActResult::NotAnnounced;
}

// HOST, when the client acts: its puppet stands near the desk, so the reach check has the pose.
bool AwaitClientPose() {
    std::string stood;
    if (!PS::AwaitText("stood", stood, kStepSec) || stood != "ok") return false;
    const bool beside = PS::WaitFor("the client's puppet beside the desk", 30, [] {
        bool in = false;
        OnGameThread([&] {
            coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1);
            void* body = rp && rp->valid() ? rp->GetActor() : nullptr;
            ue_wrap::FVector at{}, by{};
            if (body && ue_wrap::engine::TryGetActorLocation(Desk(), at) && ue_wrap::engine::TryGetActorLocation(body, by)) {
                const float dx = at.X - by.X, dy = at.Y - by.Y, dz = at.Z - by.Z;
                in = dx * dx + dy * dy + dz * dz <= kStandCm * kStandCm;
            }
        });
        return in;
    });
    if (beside) PS::Mark("pose_ok");
    return beside;
}

// This peer's desk after a short that was announced: waits for the event that decides it, the desk rising or this peer's
// word that it will not (a client hears the host's "no"; the host sees its own refusal). Returns 1 shorted, 0 not (the
// event that decided it), -1 no event in the bound (INVALID).
int AwaitRise(bool hostJudges, uint32_t refused0, uint32_t heard0) {
    const bool event = PS::WaitFor("the desk short", kRiseSec, [hostJudges, refused0, heard0] {
        return Shorting() == 1 || (hostJudges ? Refused() > refused0 : Heard() > heard0);
    }, false);
    return !event ? -1 : Shorting() == 1 ? 1 : 0;
}

// A shorted desk's fall and its panel's change: the mask after, or -1 when a bound passed first.
int AwaitMask(int before) {
    if (!PS::WaitFor("the desk short's end", kEndSec, [] { return Shorting() == 0; }, false)) return -1;
    return PS::WaitFor("the power panel's change", kMaskSec, [before] { return PowerMask() != before; }, false) ? PowerMask() : -1;
}

// The client's end: its result goes to the host, and it waits for the host's word that the run is done.
void FinishClient(int shorted, int before, int after) {
    UE_LOGI("desk-short: CLIENT desk shorted=%d, power mask before=%d after=%d", shorted, before, after);
    PS::Mark("client_result", std::to_string(shorted) + " " + std::to_string(before) + " " + std::to_string(after));
    PS::Await("host_done", kStepSec);
    UE_LOGI("desk-short: DONE on client");
}

void RunClient() {
    if (!disc_drill::HostPuppetUp() || !disc_drill::ClientJoined()) return;
    const int before = PowerMask();
    PS::Mark("client_ready");
    if (!PS::Await("host_ready", kStepSec)) return;
    const uint32_t heard0 = Heard();
    bool announced = true;
    if (!HostActs()) {
        const ActResult r = Act("CLIENT", true);
        if (r == ActResult::Stopped) return;
        announced = r == ActResult::Announced;
    } else {
        std::string text;
        if (!PS::AwaitText("acted", text, kStepSec)) return;
        announced = text == "1";
    }
    if (!announced) {
        // The host decides FAIL at once; this desk is read as it stands, with no wait.
        FinishClient(Shorting(), before, PowerMask());
        return;
    }
    const int shorted = AwaitRise(false, 0, heard0);
    int after = -1;
    if (shorted == 1) after = AwaitMask(before);
    else if (shorted == 0) after = PowerMask();
    FinishClient(shorted, before, after);
}

// The host ends a decided FAIL at once: it says it, releases the client and runs no later leg.
void HostFails(const char* why) {
    RV::Say(kTag, RV::Outcome::Fail, "%s", why);
    PS::Mark("host_done");
    UE_LOGI("desk-short: DONE on host");
}

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    // A short begun while the slot is not world-ready reaches it by no send: the host acts only once the client's world is up.
    if (!PS::WaitFor("the client's slot world-ready", 120, [] {
            bool ready = false;
            OnGameThread([&] { ready = harness::session_runtime::Session().IsSlotWorldReady(1); });
            return ready;
        }))
        return;
    const int before = PowerMask();
    const uint32_t refused0 = Refused();
    PS::Mark("host_ready");
    if (HostActs()) {
        if (Act("HOST", false) != ActResult::Announced) {
            HostFails("the host's short was not announced: no client was told");
            return;
        }
    } else {
        if (!AwaitClientPose()) return;
        std::string text;
        if (!PS::AwaitText("acted", text, kStepSec)) return;
        if (text != "1") {
            HostFails("the client's short was not asked of the host: the host's desk was never told");
            return;
        }
    }
    const int hs = HostActs() ? Shorting() : AwaitRise(true, refused0, 0);
    if (hs < 0 || (hs == 0 && HostActs())) {
        RV::Say(kTag, RV::Outcome::Invalid, "the host's desk neither shorted nor refused in time");
        return;
    }
    if (hs == 0) {
        HostFails("the host refused the client's ask: its desk was never shorted");
        return;
    }
    const int ha = AwaitMask(before);
    UE_LOGI("desk-short: HOST desk shorted=%d, power mask before=%d after=%d", hs, before, ha);
    std::string theirs;
    if (!PS::AwaitText("client_result", theirs, kStepSec)) return;
    PS::Mark("host_done");
    int cs = 0, cb = -1, ca = -1;
    if (ha < 0 || sscanf_s(theirs.c_str(), "%d %d %d", &cs, &cb, &ca) != 3 || cs < 0 || ca < 0) {
        RV::Say(kTag, RV::Outcome::Invalid, "a peer's desk or power panel did not read in time ('%s')", theirs.c_str());
        return;
    }
    const bool pass = cs && ha == ca && ha != before;
    RV::Say(kTag, pass ? RV::Outcome::Pass : RV::Outcome::Fail,
            "the %s's short: the host's desk shorted, the client's=%d; power mask host %d -> %d, client %d -> %d",
            HostActs() ? "host" : "client", cs, before, ha, cb, ca);
    UE_LOGI("desk-short: DONE on host");
}

// ---- join: a client that arrives while the host's desk shorts ----

void RunJoinHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_done", 480)) return;
    const uint32_t life1Gen = harness::session_runtime::Session().peerGenerationForSlot(1);
    PS::Mark("host_ack");
    if (!PS::WaitFor("the client's life 1 gone from slot 1", 300, [life1Gen] {
            auto& s = harness::session_runtime::Session();
            return !s.IsSlotConnected(1) || s.peerGenerationForSlot(1) != life1Gen;
        }))
        return;
    // The desk shorts again as each ten seconds end, until the rejoined client's world is up: the host's connect
    // replay then finds it shorting. A short begun while the slot is not world-ready reaches it by no send.
    int atEdge = -1;
    uint32_t replayed = 0;
    if (!PS::WaitFor("the rejoined client's world ready", 400, [life1Gen, &atEdge, &replayed] {
            bool ready = false;
            OnGameThread([&] {
                auto& s = harness::session_runtime::Session();
                ready = s.IsSlotWorldReady(1) && s.peerGenerationForSlot(1) != life1Gen;
                bool on = false;
                void* desk = Desk();
                if (!ue_wrap::desk_short::ReadShortCircuiting(desk, on)) return;
                if (ready) {
                    atEdge = on ? 1 : 0;
                    replayed = coop::desk_short_sync::GetCounts().replayed;   // the connect replay ran in the edge's own task
                } else if (!on) ue_wrap::desk_short::CallShortCircuit(desk);
            });
            return ready;
        }))
        return;
    UE_LOGI("desk-short: HOST the rejoined client's world is up; the host's desk shorts=%d, replays sent=%u", atEdge, replayed);
    PS::Mark("replay_decision", std::to_string(replayed));
    if (atEdge != 1) {
        RV::Say(kTagJoin, RV::Outcome::Invalid, "the host's desk did not short at the client's ready edge");
        return;
    }
    // The host's decision is made at the edge: a desk shorting with nothing sent cannot be told to the joiner later.
    if (replayed == 0) {
        RV::Say(kTagJoin, RV::Outcome::Fail,
                "the host's desk shorted at the client's ready edge and the host sent the joiner no short");
        return;
    }
    std::string theirs;
    if (!PS::AwaitText("rejoin_done", theirs, 120)) return;
    int applied = 0, timedOut = 0;
    if (sscanf_s(theirs.c_str(), "applied=%d timeout=%d", &applied, &timedOut) != 2 || timedOut) {
        RV::Say(kTagJoin, RV::Outcome::Invalid, "the host's replay did not reach the rejoined client in time ('%s')", theirs.c_str());
        return;
    }
    RV::Say(kTagJoin, applied >= 1 ? RV::Outcome::Pass : RV::Outcome::Fail,
            "the host's desk shorted at the client's ready edge and sent %u replay(s); the rejoined client's desk was shorted by "
            "the host %d time(s)", replayed, applied);
    UE_LOGI("desk-short: JOIN DONE on host");
}

void RunJoinClient() {
    if (!disc_drill::HostPuppetUp() || !disc_drill::ClientJoined()) return;
    PS::Mark("client_done");
    PS::Await("host_ack", 60);
    OnGameThread([] {
        auto& s = harness::session_runtime::Session();
        if (s.running()) coop::net_pump::FleeAfterNativeMenuTravel(s, "desk short drill: life 1 leaves for the rejoin");
    });
    PS::WaitFor("this life's session stopped", 15, [] { return !harness::session_runtime::Session().running(); }, false);
    UE_LOGI("desk-short: DONE on client");
}

void RunRejoin() {
    if (!disc_drill::HostPuppetUp()) return;
    // The host says at the edge whether it sent a short: when it sent none, nothing can arrive and the answer is read
    // at once; when it sent one, its arrival is the event, and a bound passing without it is INVALID (timeout=1).
    std::string decision;
    if (!PS::AwaitText("replay_decision", decision, 120)) return;
    int applied = 0;
    auto read = [&applied] {
        OnGameThread([&] { applied = static_cast<int>(coop::desk_short_sync::GetCounts().applied); });
        return applied >= 1;
    };
    const bool sent = decision != "0";
    const bool arrived = !sent || PS::WaitFor("the host's short applied", kRejoinSec, read, false);
    read();
    UE_LOGI("desk-short: REJOIN the host's short applied %d time(s) (the host sent %s)", applied, sent ? "one" : "none");
    PS::Mark("rejoin_done", "applied=" + std::to_string(applied) + " timeout=" + std::to_string(arrived ? 0 : 1));
    UE_LOGI("desk-short: REJOIN DONE");
}

}  // namespace

DWORD WINAPI DeskShortThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    const bool join = Arm() == "join";
    if (!IsClientRole()) {
        if (join) RunJoinHost();
        else RunHost();
    } else if (!join) {
        RunClient();
    } else if (coop::config::ReadEnv("VOTVCOOP_DESK_SHORT_DRILL_REJOIN") == "1") {
        RunRejoin();
    } else {
        RunJoinClient();
    }
    return 0;
}

}  // namespace harness::autotest

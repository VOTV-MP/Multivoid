// harness/autotest/autotest_desk_short.cpp -- TEST-ONLY: the base computer's short circuit on both peers.
//
// Armed by the row desk_short_drill on both peers (the peers' folder is VOTVCOOP_CAN_DIR).
//   client / host  The named peer steps beside the desk and shorts its own the way a liquid's hit does: the desk's own
//                  shortCircuit, as grimeProjectile_C and the sponge bucket's pour call it. Each peer waits for its
//                  desk's `shortCircuiting` to rise, then to fall (the base goes on solar power at the end of the ten
//                  seconds), and reads the power panel's lever mask. The host says `rig: VERDICT desk-short
//                  PASS|FAIL|INVALID`: PASS when both desks shorted and both panels read the same mask, changed from
//                  the mask before. Red: desk_short_off on the acting peer.
//   join           With the rig's --rejoin. The client leaves; the host keeps its desk shorting until the client's world
//                  is ready again; the rejoined client (VOTVCOOP_DESK_SHORT_DRILL_REJOIN=1) must have had its desk
//                  shorted at that edge. The host says `rig: VERDICT desk-short-join PASS|FAIL|INVALID`. Red:
//                  desk_short_replay_off on the host.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
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
constexpr int kRiseSec = 5;     // a short on the other desk after the one that made it
constexpr int kEndSec = 40;     // the ten seconds of sparks, and the load behind them
constexpr float kStandCm = 1200.f;  // the host's reach for a client's short is 1500

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

// The actor's short, by the desk's own event, from beside the desk (a liquid is thrown from the room; the host checks
// the sender's reach, from the pose it holds of the client).
void Short(const char* me, bool client) {
    bool moved = false;
    OnGameThread([&] {
        void* local = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (local && ue_wrap::engine::TryGetActorLocation(Desk(), at))
            moved = ue_wrap::engine::TeleportTo(local, ue_wrap::FVector{at.X + 150.f, at.Y, at.Z + 60.f}, ue_wrap::FRotator{});
    });
    UE_LOGI("desk-short: %hs stands beside the desk (teleport=%d)", me, moved ? 1 : 0);
    if (client) {
        PS::Mark("stood");
        if (!PS::Await("pose_ok", kStepSec)) return;
    }
    bool ran = false;
    OnGameThread([&] { ran = ue_wrap::desk_short::CallShortCircuit(Desk()); });
    UE_LOGI("desk-short: %hs shorted its desk as a liquid's hit does (call=%d)", me, ran ? 1 : 0);
}

// HOST, when the client acts: its puppet stands near the desk, so the reach check has the pose.
bool AwaitClientPose() {
    if (!PS::Await("stood", kStepSec)) return false;
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

// "shorted before after" on this peer: whether the desk shorted, and the power mask before and after the short ended.
std::string Watch(const char* me, bool acts, bool client, int before) {
    if (acts) Short(me, client);
    else if (!PS::Await("acted", kStepSec)) return "0 -1 -1";
    if (acts) PS::Mark("acted");
    const bool rose = PS::WaitFor("the desk short", kRiseSec, [] { return Shorting() == 1; }, false);
    UE_LOGI("desk-short: %hs desk shorted=%d", me, rose ? 1 : 0);
    if (rose) PS::WaitFor("the desk short's end", kEndSec, [] { return Shorting() == 0; }, false);
    PS::WaitFor("the power panel's change", 10, [before] { return PowerMask() != before; }, false);
    const int after = PowerMask();
    UE_LOGI("desk-short: %hs power mask before=%d after=%d", me, before, after);
    return std::to_string(rose ? 1 : 0) + " " + std::to_string(before) + " " + std::to_string(after);
}

void RunClient() {
    if (!disc_drill::HostPuppetUp()) return;
    const int before = PowerMask();
    PS::Mark("client_ready");
    if (!PS::Await("host_ready", kStepSec)) return;
    PS::Mark("client_result", Watch("CLIENT", !HostActs(), true, before));
    PS::Await("host_done", kStepSec);
    UE_LOGI("desk-short: DONE on client");
}

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    const int before = PowerMask();
    PS::Mark("host_ready");
    if (!HostActs() && !AwaitClientPose()) return;
    const std::string mine = Watch("HOST", HostActs(), false, before);
    std::string theirs;
    PS::AwaitText("client_result", theirs, kStepSec);
    PS::Mark("host_done");
    int hs = 0, hb = -1, ha = -1, cs = 0, cb = -1, ca = -1;
    if (sscanf_s(mine.c_str(), "%d %d %d", &hs, &hb, &ha) != 3 || sscanf_s(theirs.c_str(), "%d %d %d", &cs, &cb, &ca) != 3 ||
        ha < 0 || ca < 0) {
        RV::Say(kTag, RV::Outcome::Invalid, "a peer's desk or power panel did not read");
        return;
    }
    const bool pass = hs && cs && ha == ca && ha != hb;
    RV::Say(kTag, pass ? RV::Outcome::Pass : RV::Outcome::Fail,
            "the %s's short: the host's desk shorted=%d, the client's=%d; power mask host %d -> %d, client %d -> %d",
            HostActs() ? "host" : "client", hs, cs, hb, ha, cb, ca);
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
    if (!PS::WaitFor("the rejoined client's world ready", 400, [life1Gen, &atEdge] {
            bool ready = false;
            OnGameThread([&] {
                auto& s = harness::session_runtime::Session();
                ready = s.IsSlotWorldReady(1) && s.peerGenerationForSlot(1) != life1Gen;
                bool on = false;
                void* desk = Desk();
                if (!ue_wrap::desk_short::ReadShortCircuiting(desk, on)) return;
                if (ready) atEdge = on ? 1 : 0;
                else if (!on) ue_wrap::desk_short::CallShortCircuit(desk);
            });
            return ready;
        }))
        return;
    UE_LOGI("desk-short: HOST the rejoined client's world is up; the host's desk shorts=%d", atEdge);
    std::string theirs;
    if (!PS::AwaitText("rejoin_done", theirs, 120)) return;
    int applied = 0, shorting = 0;
    if (atEdge != 1) {
        RV::Say(kTagJoin, RV::Outcome::Invalid, "the host's desk did not short at the client's ready edge");
        return;
    }
    if (sscanf_s(theirs.c_str(), "applied=%d shorting=%d", &applied, &shorting) != 2) {
        RV::Say(kTagJoin, RV::Outcome::Invalid, "the rejoined client's reading was not understood: '%s'", theirs.c_str());
        return;
    }
    RV::Say(kTagJoin, applied >= 1 ? RV::Outcome::Pass : RV::Outcome::Fail,
            "the host's desk shorted at the client's ready edge; the rejoined client's desk was shorted by the host %d "
            "time(s), shorting now=%d", applied, shorting);
    UE_LOGI("desk-short: JOIN DONE on host");
}

void RunJoinClient() {
    if (!disc_drill::HostPuppetUp()) return;
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
    int applied = 0;
    PS::WaitFor("the host's short applied", 10, [&applied] {
        OnGameThread([&] { applied = static_cast<int>(coop::desk_short_sync::GetCounts().applied); });
        return applied >= 1;
    }, false);
    const int shorting = Shorting();
    UE_LOGI("desk-short: REJOIN the host's short applied %d time(s), the desk shorts=%d", applied, shorting);
    PS::Mark("rejoin_done", "applied=" + std::to_string(applied) + " shorting=" + std::to_string(shorting));
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

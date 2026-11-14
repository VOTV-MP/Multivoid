// harness/autotest/autotest_sky_tp.cpp -- TEST-ONLY: the earth teleport's black hole crosses from the host's sky to a
// client's. A verdict is decided by an event; a bound passing with none is INVALID. Armed by the row sky_tp_drill on
// both peers (VOTVCOOP_CAN_DIR). The black hole is the sky's flag (skyVisibility and its light); a SkyState carries it.
//   1     The host runs the sky's `tp`: its hole shows 20 s in and goes 66 s later. The client decides on its lane's
//         own events: the first SkyState carrying the hole must leave the client's sky showing it, and the first
//         state after it, with the flag fallen, must leave it gone. The state's receipt and its apply are one task, so
//         a flag left unapplied is final where the state is counted: `rig: VERDICT sky-tp PASS|FAIL|INVALID`.
//         Red: sky_flag_apply_off (client).
//   join  With the rig's --rejoin. The host holds its hole (the sky's own flag applied, no `tp`), the client leaves and
//         comes back; the rejoined client's (VOTVCOOP_SKY_TP_DRILL_REJOIN=1) first state, the connect snapshot, must
//         carry the hole and leave its sky showing it: `rig: VERDICT sky-tp-join ...`. Red: sky_flag_apply_off (client).

#include "harness/autotest.h"
#include "disc_drills.h"      // co-located private headers (src tree, not include/)
#include "gt_wait.h"
#include "rejoin_steps.h"
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/session/rig_verdict.h"
#include "coop/world/sky_sync.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/world/skysphere.h"

#include <windows.h>

#include <string>

namespace harness::autotest {
namespace {

namespace PS  = harness::autotest::peer_signal;
namespace RV  = coop::rig_verdict;
namespace SS  = coop::sky_sync;
namespace SKY = ue_wrap::skysphere;
using disc_drill::OnGameThread;

constexpr const char* kTag = "sky-tp";
constexpr const char* kTagJoin = "sky-tp-join";
constexpr int kStepSec = 120;
constexpr int kHoleSec = 60;     // INVALID bound: tp's hole shows 20 s in
constexpr int kFallSec = 180;    // INVALID bound: and goes 66 s later

std::string Arm() { return coop::config::ResolveEnum(coop::config_registry::rows::sky_tp_drill); }

bool Hole() {
    bool on = false;
    return SKY::ReadBlackHole(on) && on;
}

// ---- 1 ----

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    if (!WaitGT("the client's slot world-ready", 120, [] { return harness::session_runtime::Session().IsSlotWorldReady(1); })) return;
    bool ran = false;
    OnGameThread([&] {
        void* sky = SKY::Sky();
        ran = sky && ue_wrap::component_calls::CallParamlessNamed(sky, L"tp");
    });
    UE_LOGI("sky-tp-drill: HOST the sky's tp ran=%d", ran ? 1 : 0);
    if (!ran) {
        RV::Say(kTag, RV::Outcome::Invalid, "the host's sky has no tp to run");
        return;
    }
    PS::Mark("tp_fired");
    if (!WaitGT("the host's black hole", kHoleSec, [] { return Hole(); })) return;
    UE_LOGI("sky-tp-drill: HOST the black hole is up");
    PS::Mark("hole_on");
    if (!WaitGT("the host's black hole gone", kFallSec, [] { return !Hole(); })) return;
    UE_LOGI("sky-tp-drill: HOST the black hole is gone");
    PS::Mark("hole_off");
    std::string done;
    if (!PS::AwaitText("client_done", done, kStepSec)) return;
    if (done[0] != 'P') return;   // the client said its FAIL where it was decided
    RV::Say(kTag, RV::Outcome::Pass, "the client's sky showed the host's black hole and turned it back");
    UE_LOGI("sky-tp-drill: DONE on host");
}

void RunClient() {
    if (!disc_drill::HostPuppetUp() || !disc_drill::ClientJoined()) return;
    PS::Mark("client_ready");
    if (!PS::Await("tp_fired", kStepSec)) return;
    // The first state carrying the hole: its apply ran in the task that counted it.
    if (!WaitGT("a SkyState carrying the black hole", kHoleSec + kStepSec, [] { return SS::Read().holeOnReceived >= 1; })) return;
    if (!WaitGT("the sky read", 5, [] { bool on; return SKY::ReadBlackHole(on); }, false)) {
        RV::Say(kTag, RV::Outcome::Invalid, "the client's sky has no black hole flag to read");
        return;
    }
    bool shown = false;
    OnGameThread([&] { shown = Hole(); });
    if (!shown) {
        RV::Say(kTag, RV::Outcome::Fail, "a SkyState carrying the host's black hole arrived and the client's sky does not show it");
        PS::Mark("client_done", "F");
        return;
    }
    UE_LOGI("sky-tp-drill: CLIENT the black hole shows");
    // The flag falling: the state after the hole's end.
    if (!WaitGT("a SkyState after the hole's end", kFallSec + kStepSec, [] { return SS::Read().holeFalls >= 1; })) return;
    bool still = true;
    OnGameThread([&] { still = Hole(); });
    if (still) {
        RV::Say(kTag, RV::Outcome::Fail, "a SkyState with the hole's flag fallen arrived and the client's sky still shows it");
        PS::Mark("client_done", "F");
        return;
    }
    PS::Mark("client_done", "P");
    UE_LOGI("sky-tp-drill: DONE on client");
}

// ---- join ----

void RunJoinHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    bool held = false;
    OnGameThread([&] { held = SKY::ApplyBlackHole(true) && Hole(); });
    if (!held) {
        RV::Say(kTagJoin, RV::Outcome::Invalid, "the host's sky could not be given a black hole");
        return;
    }
    UE_LOGI("sky-tp-drill: HOST holds the black hole");
    PS::Mark("held");
    if (!PS::Await("client_done", kStepSec)) return;
    const uint32_t gen = rejoin::FirstLifeGen();
    PS::Mark("host_ack");
    if (!rejoin::AwaitNextLifeReady(gen, [] {})) return;
    UE_LOGI("sky-tp-drill: HOST the rejoined client's world is up");
    PS::Mark("edge");
    std::string done;
    const bool got = PS::AwaitText("rejoin_done", done, kStepSec);
    OnGameThread([] { SKY::ApplyBlackHole(false); });   // the host's sky back as it was
    if (!got) return;
    if (done[0] == 'P') RV::Say(kTagJoin, RV::Outcome::Pass, "the rejoined client's first SkyState carried the hole and its sky shows it");
    UE_LOGI("sky-tp-drill: JOIN DONE on host");
}

void RunJoinClient() {
    if (!disc_drill::HostPuppetUp() || !disc_drill::ClientJoined()) return;
    PS::Mark("client_ready");
    if (!PS::Await("held", kStepSec)) return;
    PS::Mark("client_done");
    PS::Await("host_ack", 60);
    rejoin::LeaveForRejoin("sky tp drill: life 1 leaves for the rejoin");
    UE_LOGI("sky-tp-drill: DONE on client");
}

void RunRejoin() {
    if (!disc_drill::HostPuppetUp()) return;
    if (!PS::Await("edge", kStepSec)) return;
    if (!WaitGT("the connect snapshot's SkyState", kStepSec, [] { return SS::Read().statesReceived >= 1; })) return;
    SS::Counters c;
    bool shown = false;
    OnGameThread([&] {
        c = SS::Read();
        shown = Hole();
    });
    const bool ok = c.firstHole == 1 && shown;
    UE_LOGI("sky-tp-drill: REJOIN the first state carried the hole=%u, the sky shows it=%d", static_cast<unsigned>(c.firstHole), shown ? 1 : 0);
    if (!ok)
        RV::Say(kTagJoin, RV::Outcome::Fail, "the joiner's first SkyState carried the hole=%u and its sky shows it=%d",
                static_cast<unsigned>(c.firstHole), shown ? 1 : 0);
    PS::Mark("rejoin_done", ok ? "P" : "F");
    UE_LOGI("sky-tp-drill: REJOIN DONE");
}

}  // namespace

DWORD WINAPI SkyTpDrillThread(LPVOID /*arg*/) {
    const bool join = Arm() == "join";
    if (!PS::Begin(join ? kTagJoin : kTag)) return 0;
    if (!IsClientRole()) {
        if (join) RunJoinHost();
        else RunHost();
    } else if (!join) {
        RunClient();
    } else if (coop::config::ReadEnv("VOTVCOOP_SKY_TP_DRILL_REJOIN") == "1") {
        RunRejoin();
    } else {
        RunJoinClient();
    }
    return 0;
}

}  // namespace harness::autotest

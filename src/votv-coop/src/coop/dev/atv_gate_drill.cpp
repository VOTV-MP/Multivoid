// coop/dev/atv_gate_drill.cpp -- the sequencer of the ATV gate drill; see coop/dev/atv_gate_drill.h. The acts are
// atv_gate_drill_acts.cpp.

#include "coop/dev/atv_gate_drill.h"

#include "atv_gate_drill_detail.h"  // co-located private header (src tree, not include/)

#include "harness/autotest.h"   // IsClientRole
#include "harness/autotest/peer_signal.h"

#include "coop/dev/atv_gate_probe.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/core/log.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::atv_gate_drill {
namespace {

namespace PS = harness::autotest::peer_signal;
namespace PR = coop::dev::atv_gate_probe;
namespace RV = coop::rig_verdict;
using namespace detail;

constexpr int kJoinSec = 480;     // the client's boot, join and world load
constexpr int kSignalSec = 240;   // a step of the other peer
constexpr int kLiveSec = 120;     // the probe's watches settle with the world
constexpr int kWalkSec = 420;     // the other peer's director walk

struct Step {
    const char* name;
    ActFn       run;
};

const Step kClientSteps[] = {
    {"can", &PourCan}, {"toolbox", &UseToolbox}, {"fit", &FitModule}, {"take", &TakeModuleOff}, {"sit", &Sit},
    {"lights", &Lights}, {"brake", &Brake}, {"turbo", &Turbo}, {"honk", &Honk}, {"leave", &Leave},
};
const Step kHostSteps[] = {
    {"host_can", &PourCan}, {"host_toolbox", &UseToolbox}, {"host_sit", &Sit}, {"host_lights", &Lights},
    {"host_brake", &Brake}, {"host_turbo", &Turbo}, {"host_honk", &Honk},
};

void Invalid(const std::string& why) { RV::Say(kTag, RV::Outcome::Invalid, "%s", why.c_str()); }

// "fn:n,fn:n" of what rose between two snapshots on one seam, or "-".
std::string Delta(const PR::Counts& a, const PR::Counts& b, bool gate) {
    std::string out;
    for (int i = 0; i < PR::kFnCount; ++i) {
        const uint32_t n = gate ? b.gate[i] - a.gate[i] : b.pe[i] - a.pe[i];
        if (!n) continue;
        out += std::string(out.empty() ? "" : ",") + PR::NameOf(static_cast<PR::Fn>(i)) + ":" + std::to_string(n);
    }
    return out.empty() ? "-" : out;
}

std::string Move(float a, float b) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.1f>%.1f", static_cast<double>(a), static_cast<double>(b));
    return buf;
}

// `actor` is this peer's step to do; `issued` is whether the act was issued (the actor's word, for an observer); `acted`
// is both, this peer having done it. An act that was not issued says why.
void Measure(const char* step, bool actor, bool issued, const std::string& why, const PR::Counts& c0, const PR::Counts& c1,
             const Vitals& v0, const Vitals& v1) {
    UE_LOGI("[ATVG-DRILL] MEASURE %s role=%s actor=%d acted=%d issued=%d sg=%s pe=%s fuel=%s health=%s modules=%d>%d "
            "seated=%d%s%s", step, harness::autotest::IsClientRole() ? "client" : "host", actor ? 1 : 0,
            actor && issued ? 1 : 0, issued ? 1 : 0, Delta(c0, c1, true).c_str(), Delta(c0, c1, false).c_str(),
            Move(v0.fuel, v1.fuel).c_str(), Move(v0.health, v1.health).c_str(), v0.modules, v1.modules,
            v1.seated ? 1 : 0, issued ? "" : " why=", issued ? "" : why.c_str());
}

PR::Counts Counts() {
    PR::Counts c;
    OnGt([&] { c = PR::Snapshot(); });
    return c;
}

constexpr const char kFailPrefix[] = "fail: ";

// One step. The actor does the act, then signals "ok" or "fail: <why>"; the other peer reads its own counts at the
// signal. Both print the MEASURE line for the window. An act that could not be issued is measured and the run goes on:
// its step name is added to `unissued`. False only when a signal never came (the verdict is said by peer_signal).
bool RunStep(const Step& s, bool iAct, const std::wstring& key, std::string& unissued) {
    const std::string done = std::string(s.name) + "_done";
    const std::string seen = std::string(s.name) + "_seen";
    const PR::Counts c0 = Counts();
    const Vitals v0 = ReadVitals(key);
    std::string why;
    bool issued = false;
    if (iAct) {
        issued = s.run(key, why);
        Measure(s.name, true, issued, why, c0, Counts(), v0, ReadVitals(key));
        PS::Mark(done.c_str(), issued ? std::string("ok") : kFailPrefix + why);
        if (!PS::Await(seen.c_str(), kSignalSec)) return false;
    } else {
        std::string result;
        if (!PS::AwaitText(done.c_str(), result, kSignalSec)) return false;
        issued = result == "ok";
        if (!issued) why = result.compare(0, sizeof(kFailPrefix) - 1, kFailPrefix) == 0 ? result.substr(sizeof(kFailPrefix) - 1) : result;
        Measure(s.name, false, issued, why, c0, Counts(), v0, ReadVitals(key));
        PS::Mark(seen.c_str());
    }
    if (!issued) unissued += std::string(unissued.empty() ? "" : ", ") + s.name;
    return true;
}

// The announced world and the host's puppet exist here.
bool WorldUp() {
    if (!PS::WaitFor("the announced world", kJoinSec, [] {
            bool ready = false;
            OnGt([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
            return ready;
        }))
        return false;
    return PS::WaitFor("the host's puppet", 60, [] {
        bool have = false;
        OnGt([&] {
            coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
            have = p && p->valid();
        });
        return have;
    });
}

bool ProbeLive() {
    if (!PR::Enabled()) {
        Invalid("atv_gate_probe is off on this peer: the drill has no instrument");
        return false;
    }
    return PS::WaitFor("the probe's watches live", kLiveSec, [] {
        bool live = false;
        OnGt([&] { live = PR::AllLive(); });
        return live;
    });
}

void RunHost() {
    if (!ProbeLive()) return;
    if (!PS::Await("client_ready", kJoinSec)) return;
    const std::wstring key = ChooseAtv();
    if (key.empty()) {
        Invalid("the world holds no ATV a route from the host reaches");
        return;
    }
    UE_LOGI("[ATVG-DRILL] HOST ATV key='%ls'", key.c_str());
    PS::Mark("atv_key", std::string(key.begin(), key.end()));
    if (!PS::Await("client_has_atv", kSignalSec)) return;
    std::string unissued;
    for (const Step& s : kClientSteps)
        if (!RunStep(s, false, key, unissued)) return;
    std::string why;
    if (!WalkToAtv(key, why)) {
        PS::Mark("host_at_atv", "fail");
        Invalid(why + (unissued.empty() ? "" : "; steps not issued before it: " + unissued));
        return;
    }
    PS::Mark("host_at_atv", "ok");
    for (const Step& s : kHostSteps)
        if (!RunStep(s, true, key, unissued)) return;
    UE_LOGI("[ATVG-DRILL] DONE on host, steps not issued: %s", unissued.empty() ? "none" : unissued.c_str());
    if (unissued.empty()) RV::Say(kTag, RV::Outcome::Pass);
    else Invalid("steps not issued: " + unissued + " (the reasons are on the MEASURE lines)");
}

void RunClient() {
    if (!ProbeLive() || !WorldUp()) return;
    PS::Mark("client_ready");
    std::string keyText;
    if (!PS::AwaitText("atv_key", keyText, kSignalSec)) return;
    const std::wstring key(keyText.begin(), keyText.end());
    if (!CopyResolves(key)) return;
    PS::Mark("client_has_atv");
    std::string why;
    if (!WalkToAtv(key, why)) {
        Invalid(why);
        return;
    }
    std::string unissued;
    for (const Step& s : kClientSteps)
        if (!RunStep(s, true, key, unissued)) return;
    std::string hostWalk;
    if (!PS::AwaitText("host_at_atv", hostWalk, kWalkSec)) return;
    if (hostWalk != "ok") {
        UE_LOGW("[ATVG-DRILL] the host's walk to the ATV failed: its steps are not run");
        return;
    }
    for (const Step& s : kHostSteps)
        if (!RunStep(s, false, key, unissued)) return;
    UE_LOGI("[ATVG-DRILL] DONE on client, steps not issued: %s", unissued.empty() ? "none" : unissued.c_str());
}

}  // namespace

DWORD WINAPI Thread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (harness::autotest::IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace coop::dev::atv_gate_drill

// coop/dev/cremator_drill_g1.cpp -- the G1 and G4 arms of the cremator drill: the host pulls the machine's lever and
// the client's copy follows (G1), then the client leaves and comes back into the running machine (G4). See
// coop/dev/cremator_drill.h for the arms and their lines.

#include "cremator_drill_g2.h"  // co-located private header (src tree, not include/)

#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/net/session.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/cremator.h"

#include <cstdint>
#include <string>

namespace coop::dev::cremator_drill {

namespace C  = ue_wrap::cremator;
namespace PS = harness::autotest::peer_signal;
using namespace detail;
using PS::Await;
using PS::AwaitText;
using PS::Mark;

namespace detail {

// Shuts the door and pulls the lever on the host's copy, the machine's own verb; game thread.
bool Pull(const std::wstring& key) {
    bool ran = false, wasOpened = false;
    OnGameThread([&] {
        void* c = Resolve(key);
        ran = c && C::ShutDoor(c, wasOpened) && C::CallUseLever(c);
    });
    UE_LOGI("cremator-drill: HOST pulled the lever ran=%d doorWasOpened=%d", ran ? 1 : 0, wasOpened ? 1 : 0);
    return ran;
}

}  // namespace detail

void RunHost() {
    if (!Await("client_ready", kJoinSec)) return;
    const std::wstring key = HostFindKey();
    if (key.empty()) return;
    if (!PS::WaitFor("the host's handle at rest, run off", kHostRunSec, [&] { return CopyAt(key, 0); })) return;
    Mark("key", Narrow(key));

    // G1.
    if (!Pull(key)) { Abort("the host's lever did not run"); return; }
    if (!PS::WaitFor("the host's run on", kHostRunSec, [&] { return CopyAt(key, 1); })) return;
    Mark("g1_on");
    std::string on;
    if (!AwaitText("g1_on_seen", on, kSignalSec)) return;
    const Reading ron = Parse(on);
    Verdict(ron.active == 1 && ron.closed == 1, "g1-on", "the client's copy after the host's pull: " + on);

    if (!Pull(key)) { Abort("the host's lever did not run"); return; }
    if (!PS::WaitFor("the host's run off", kHostRunSec, [&] { return CopyAt(key, 0); })) return;
    Mark("g1_off");
    std::string off;
    if (!AwaitText("g1_off_seen", off, kSignalSec)) return;
    const Reading roff = Parse(off);
    Verdict(roff.active == 0, "g1-off", "the client's copy after the host's pull back: " + off);
    UE_LOGI("cremator-drill: G1 DONE fails=%d", Fails());

    // G4. The run stays on; the client leaves, and comes back into it.
    if (!Pull(key)) { Abort("the host's lever did not run"); return; }
    if (!PS::WaitFor("the host's run on", kHostRunSec, [&] { return CopyAt(key, 1); })) return;
    // Life 1's occupancy of slot 1: the wait is for it to change, never for the slot to read closed -- a fast rejoin
    // closes and refills the slot between two reads.
    const uint32_t life1Gen = harness::session_runtime::Session().peerGenerationForSlot(1);
    Mark("g4_on");
    std::string seen;
    if (!AwaitText("client_done", seen, kSignalSec)) return;
    UE_LOGI("cremator-drill: HOST the client's first life saw the run: %s", seen.c_str());
    Mark("host_ack");  // now watching slot 1: life 1 may end
    if (!PS::WaitFor("the client's life 1 gone from slot 1", 300, [life1Gen] {
            auto& s = harness::session_runtime::Session();
            return !s.IsSlotConnected(1) || s.peerGenerationForSlot(1) != life1Gen;
        }))
        return;
    // The second life leaves rejoin_ready once its world is up, and only then is the key written, so it reads it as
    // written since its own start.
    if (!Await("rejoin_ready", 400)) return;
    Mark("e_key", Narrow(key));
    std::string again;
    if (!AwaitText("rejoin_done", again, 400)) return;
    const Reading rj = Parse(again);
    Verdict(rj.active == 1 && rj.closed == 1, "g4", "the rejoined client's copy after its world-ready: " + again);
    UE_LOGI("cremator-drill: DONE on host fails=%d", Fails());
}

// ---- CLIENT ----------------------------------------------------------------------------------------------

void RunClient() {
    if (!HostPuppetUp()) return;
    Mark("client_ready");
    std::string keyText;
    if (!AwaitText("key", keyText, kSignalSec)) return;
    const std::wstring key = Widen(keyText);
    if (!CopyResolves(key)) return;

    if (!Await("g1_on", kSignalSec)) return;
    ReportCopy(key, 1, "g1_on_seen");
    if (!Await("g1_off", kSignalSec)) return;
    ReportCopy(key, 0, "g1_off_seen");
    UE_LOGI("cremator-drill: G1 DONE on client");

    if (!Await("g4_on", kSignalSec)) return;
    ReportCopy(key, 1, "client_done");
    // The DONE line is the rig's cue to kill this life: printed only once the host watches slot 1.
    Await("host_ack", 60);
    // A killed process only stops sending, and the host learns of it when the next life connects: life 1 leaves as a
    // player's quit does (network first), so the host sees slot 1 empty at once.
    OnGameThread([] {
        auto& s = harness::session_runtime::Session();
        if (s.running()) coop::net_pump::FleeAfterNativeMenuTravel(s, "cremator drill: life 1 leaves for the rejoin");
    });
    PS::WaitFor("this life's session stopped", 15, [] { return !harness::session_runtime::Session().running(); }, false);
    UE_LOGI("cremator-drill: DONE on client");
}

void RunRejoin() {
    if (!HostPuppetUp()) return;
    Mark("rejoin_ready");
    std::string keyText;
    if (!AwaitText("e_key", keyText, 120)) return;
    const std::wstring key = Widen(keyText);
    if (!CopyResolves(key)) return;
    ReportCopy(key, 1, "rejoin_done");
    UE_LOGI("cremator-drill: REJOIN DONE");
}

}  // namespace coop::dev::cremator_drill

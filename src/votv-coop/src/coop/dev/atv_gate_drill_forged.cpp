// coop/dev/atv_gate_drill_forged.cpp -- the ATV drill's rejoin leg. After the steps of the first life the client leaves the
// session and the rig restarts it (mp.py --rejoin); the second life holds its ClientWorldReady announce (coop/dev/announce_hold),
// makes a forged request in that window, and then lets the announce go. One step is decided there, on events:
//   16 the host PARKS the request until the sender's world is ready and answers it after: decided on the host's park events
//      (a request answered while the slot is not ready, FAIL; answered after a park, PASS).
// The marks: client left, rejoin_up, atv_key2 (the host's key), req16_sent, rejoin_c_done.

#include "atv_gate_drill_detail.h"

#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/announce_hold.h"
#include "coop/interactables/atv_drill_events.h"
#include "coop/interactables/atv_gate.h"
#include "coop/interactables/atv_host_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/log.h"

#include <cstdint>
#include <string>

namespace coop::dev::atv_gate_drill {
namespace {

namespace PS = harness::autotest::peer_signal;
namespace AG = coop::atv_gate;
namespace H = coop::atv_host_sync;
namespace DE = coop::atv_drill_events;

constexpr int kLeaveS = 15;      // the client's leaving takes this
constexpr int kRejoinS = 400;    // the second life's boot and join
constexpr const char* kRejoinEnv = "VOTVCOOP_ATV_DRILL_REJOIN";

std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }

}  // namespace

namespace detail {

bool IsRejoinedLife() { return coop::config::ReadEnv(kRejoinEnv) == "1"; }

void LeaveForRejoin() {
    OnGt([] {
        auto& s = harness::session_runtime::Session();
        if (s.running()) coop::net_pump::FleeAfterNativeMenuTravel(s, "atv drill: the client leaves for the rejoin");
    });
    PS::WaitFor("this life's session stopped", kLeaveS, [] { return !harness::session_runtime::Session().running(); }, false);
    PS::Mark("client_left");
    UE_LOGI("atv-drill: REJOIN MARKER the client has left and is restarted by the rig");
}

// HOST: step 16 on its park events.
bool HostRejoin(const std::wstring& key) {
    std::string t;
    if (!AwaitMark("client_left", t, kLeaveS + 240) || !AwaitMark("rejoin_up", t, kRejoinS)) return false;
    Ctx c;
    c.me = kHost;
    c.cur[DE::kPark] = DE::Count(DE::kPark);
    PS::Mark("atv_key2", Narrow(key));
    bool parked = false;
    DE::Rec r;
    const int w = WaitRec(c, DE::kPark, [&](const DE::Rec& e) {
        if (e.i0 != kClient) return false;
        if (e.i1 == 0) parked = true;
        return e.i1 != 0;
    }, r);
    // An answer with the slot not ready (phase 2) is the run-wide FAIL Aborted() says, at whatever join it comes.
    if (w != 1) return false;
    if (!parked) {
        End('I', "step 16: the request was answered after a park the host never recorded");
        return false;
    }
    UE_LOGI("atv-drill: step 16 PASS: the host parked the joiner's request and answered it after the slot's world was ready");
    return AwaitMark("rejoin_c_done", t, kRejoinS);
}

// CLIENT, second life: the held announce, the forged request.
void RunRejoinedClient() {
    std::string t;
    const bool up = WaitUntil("this life's player, slot and session", 240, [] {
        bool ok = false;
        OnGt([&] {
            ok = coop::players::Registry::Get().Local() != nullptr &&
                 coop::players::Registry::Get().LocalPeerId() != coop::players::kPeerIdUnknown && H::LiveSession() != nullptr;
        });
        return ok;
    });
    if (!up) return;
    PS::Mark("rejoin_up");
    std::string keyText;
    if (!AwaitMark("atv_key2", keyText, kRejoinS)) return;
    const std::wstring key(keyText.begin(), keyText.end());
    uint32_t id = 0;
    OnGt([&] { id = AG::SendForDrill(key, coop::net::atv_host_op::kRefuel, 1.f, 0); });
    coop::dev::announce_hold::Set(false);
    if (!id) {
        End('I', "step 16: the forged request at the world-ready edge was not sent");
        return;
    }
    PS::Mark("req16_sent");
    PS::Mark("rejoin_c_done");
}

}  // namespace detail

// The second life holds its announce from the session's start, so the forged request can reach the host before it.
void Install(coop::net::Session* session) {
    static bool done = false;
    if (done || !session || session->role() != coop::net::Role::Client) return;
    if (coop::config::ResolveEnum(::coop::config_registry::rows::atv_gate_drill) == "off" || !detail::IsRejoinedLife()) return;
    done = true;
    coop::dev::announce_hold::Set(true);
    UE_LOGI("atv-drill: this rejoined client holds its announce until its request is sent");
}

}  // namespace coop::dev::atv_gate_drill

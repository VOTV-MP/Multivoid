// coop/dev/atv_gate_drill_forged.cpp -- the ATV drill's rejoin legs. After step 15 the client leaves the session and the rig
// restarts it (mp.py --rejoin); the second life holds its ClientWorldReady announce (coop/dev/announce_hold), makes a forged
// request in that window, and then lets the announce go. Two steps are decided there, each on events:
//   16 the host PARKS the request until the sender's world is ready and answers it after: decided on the host's park events
//      (a request answered while the slot is not ready, FAIL; answered after a park, PASS);
//   10 the joiner's copy gets the host's state: the state arrived while the copy was not yet adopted and is applied at its
//      adoption. Decided on the copy's adoption (INVALID when the state did not arrive before it).
// The marks: client left, rejoin_up, atv_h10 / atv_key2 (the host's state and key), req16_sent, rejoin_c_done.

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

#include <cmath>
#include <cstdint>
#include <sstream>
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

bool SameKey(const DE::Rec& r, int32_t tag) { return r.i1 == tag; }
bool UnadoptedOf(const DE::Rec& r, int32_t tag) { return r.i2 == tag; }

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

// HOST: step 16 on its park events, and the state the rejoined client's copy must show at step 10.
bool HostRejoin(const std::wstring& key) {
    std::string t;
    if (!AwaitMark("client_left", t, kLeaveS + 240) || !AwaitMark("rejoin_up", t, kRejoinS)) return false;
    Vitals v = ReadVitals(key);
    uint32_t seq = 0;
    OnGt([&] { seq = H::CurrentStateSeq(key); });
    Ctx c;
    c.me = kHost;
    c.cur[DE::kPark] = DE::Count(DE::kPark);
    std::ostringstream h10;
    h10 << seq << ' ' << v.fuel << ' ' << v.health << ' ' << v.modules;
    PS::Mark("atv_h10", h10.str());
    PS::Mark("atv_key2", Narrow(key));
    bool parked = false;
    DE::Rec r;
    const int w = WaitRec(c, DE::kPark, [&](const DE::Rec& e) {
        if (e.i0 != kClient) return false;
        if (e.i1 == 0) parked = true;
        return e.i1 != 0;
    }, r);
    if (w != 1) return false;
    if (r.i1 == 2) {
        UE_LOGI("atv-drill: step 16 FAIL: the host answered a request from a slot whose world was not ready (it was not parked)");
        End('F', "step 16 (a request at the joiner's world-ready edge is parked) on the host: answered before the slot's world was ready");
        return false;
    }
    if (!parked) {
        End('I', "step 16: the request was answered after a park the host never recorded");
        return false;
    }
    UE_LOGI("atv-drill: step 16 PASS: the host parked the joiner's request and answered it after the slot's world was ready");
    return AwaitMark("rejoin_c_done", t, kRejoinS);
}

// CLIENT, second life: the held announce, the forged request, the adoption.
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
    std::string keyText, h10;
    if (!AwaitMark("atv_key2", keyText, kRejoinS) || !AwaitMark("atv_h10", h10, kRejoinS)) return;
    const std::wstring key(keyText.begin(), keyText.end());
    std::istringstream in(h10);
    uint32_t seq = 0;
    float fuel = 0.f, health = 0.f;
    int modules = 0;
    in >> seq >> fuel >> health >> modules;
    uint32_t id = 0;
    OnGt([&] { id = AG::SendForDrill(key, coop::net::atv_host_op::kRefuel, 1.f, 0); });
    coop::dev::announce_hold::Set(false);
    if (!id) {
        End('I', "step 16: the forged request at the world-ready edge was not sent");
        return;
    }
    PS::Mark("req16_sent");
    // Step 10: the copy's adoption, with the state that arrived before it.
    Ctx c;
    c.me = kClient;
    DE::Rec r;
    const int32_t tag = DE::KeyTag(key);
    if (WaitRec(c, DE::kAdopted, [&](const DE::Rec& a) { return SameKey(a, tag); }, r) != 1) return;
    if (!DE::AnyBefore(DE::kUnadopted, 0, r.ord, &UnadoptedOf, tag)) {
        UE_LOGI("atv-drill: step 10 INVALID: the host's state did not arrive before the copy was adopted");
        End('I', "step 10: the joiner's mirror was adopted before the host's state arrived");
        return;
    }
    if (!r.i0) {
        UE_LOGI("atv-drill: step 10 FAIL: the copy was adopted holding no state of the host's");
        End('F', "step 10 (a joiner's state before its mirror is adopted) on the client: the state that arrived before the copy was lost");
        return;
    }
    if (std::fabs(r.f0 - fuel) > 0.5f || std::fabs(r.f1 - health) > 0.5f || static_cast<int>(r.f2) != modules) {
        UE_LOGI("atv-drill: step 10 FAIL: the adopted copy shows fuel %.1f health %.1f modules %d, the host's %.1f %.1f %d", r.f0, r.f1,
                static_cast<int>(r.f2), fuel, health, modules);
        End('F', "step 10 on the client: the adopted copy does not hold the host's state");
        return;
    }
    UE_LOGI("atv-drill: step 10 PASS: the copy was adopted with the host's state");
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

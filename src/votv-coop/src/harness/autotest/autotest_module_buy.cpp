// harness/autotest/autotest_module_buy.cpp -- TEST-ONLY: the upgrade panel's module rows across two peers.
//
// The client holds the laptop and presses a module row's own buy button: the 400-credit one with a level, then it
// again (a full row), then one with no store. The host judges what it reads: the balance charged once, the level
// raised, and the client's copy converged on it. Gated by VOTVCOOP_RUN_MODULE_BUY; the peers' folder is
// VOTVCOOP_CAN_DIR; the verdict is `rig: VERDICT module-buy PASS|FAIL|INVALID`. Red arm: the client's
// module_intent_off, whose press runs on that machine alone, so the host's balance never moves and the verdict
// is FAIL.
//
// The second press (a full row, free) cannot be waited on, as nothing changes. It is judged by the third, which
// follows it in the one queue the host takes a sender's purchases from: the balance after the third is the sum of
// what the first three charged. The denial arms (VOTVCOOP_MODULE_BUY_ARM) are described at Arm().

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/interactables/device_occupancy.h"
#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/laptop_widget.h"
#include "ue_wrap/world/economy.h"
#include "ue_wrap/world/upgrades.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace OI = ue_wrap::object_index;
namespace PS = harness::autotest::peer_signal;
namespace R = ue_wrap::reflection;
namespace UP = ue_wrap::upgrades;
using disc_drill::OnGameThread;
using PS::Await;
using PS::AwaitText;
using PS::Mark;

constexpr const char* kTag = "module-buy";
constexpr int kTriangleIndex = 27;   // the module numbered like the triangle level; it writes that level at 400
constexpr int kStorelessIndex = 23;  // a row whose switch case returns a temporary: no level, and 30 credits
constexpr int kLevelIndex = 3;       // a LEVEL row's index (processSpeed): no module row carries it
constexpr int kTrianglePrice = 400;
constexpr int kStorelessPrice = 30;
constexpr int kBalance = 5000;
constexpr int kStepSec = 60;         // the bound on one step of the other peer's
constexpr int kEffectSec = 30;       // the bound on the host's balance moving after a press

// The denial arm both peers run (VOTVCOOP_MODULE_BUY_ARM), empty for the plain drill. An arm sends a request the host
// must refuse, by hand, as a client whose panel gate did not stop it would: `nobit` names a level row's index as a module
// (dir 2 and 3, index 3: no module row carries the bit there), `reach` asks for the 400-credit module while this client
// does not hold the laptop. Nothing changes on a refusal, so the sentinel judges it: the storeless row's buy follows in
// the same queue, and the balance after it is the fixture less its 30 alone, with the 400-credit row still at level 0.
// The client's copy must read the host's. Red arm `valid`: the same request from the laptop's holder, which the host
// buys, so the verdict is FAIL.
std::string Arm() { return coop::config::ReadEnv("VOTVCOOP_MODULE_BUY_ARM"); }
bool KnownArm(const std::string& arm) { return arm.empty() || arm == "nobit" || arm == "reach" || arm == "valid"; }

struct Row {
    void* widget = nullptr;
    int32_t index = -1;
    bool module = false;
};

std::vector<Row> Rows() {
    std::vector<Row> out;
    void* const cls = R::FindClass(L"uicomp_upgradeSlot_C");
    if (!cls) return out;
    struct Ctx { std::vector<Row>* out; void* cls; } ctx{&out, cls};
    OI::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        auto* x = static_cast<Ctx*>(c);
        if (!obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        Row r;
        r.widget = obj;
        const int32_t off = R::FindPropertyOffset(x->cls, L"index");
        r.index = off < 0 ? -1 : *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(obj) + off);
        int32_t byte = -1;
        uint8_t mask = 0;
        if (R::FindBoolProperty(x->cls, L"module", byte, mask)) r.module = (static_cast<const uint8_t*>(obj)[byte] & mask) != 0;
        x->out->push_back(r);
    }, &ctx);
    return out;
}

// The row of the live panel: a template of the panel stands beside it, and its gamemode was never set.
void* LiveModuleRow(int index) {
    void* const cls = R::FindClass(L"uicomp_upgradeSlot_C");
    const int32_t offGamemode = cls ? R::FindPropertyOffset(cls, L"gamemode") : -1;
    if (offGamemode < 0) return nullptr;
    for (const Row& r : Rows())
        if (r.module && r.index == index && *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(r.widget) + offGamemode)) return r.widget;
    return nullptr;
}

// The way the game's enter ends: the laptop's widget becomes the player's active interface, and the occupancy poll claims it.
bool EnterLaptop() {
    void* player = coop::players::Registry::Get().Local();
    void* widget = ue_wrap::laptop_widget::Widget();
    void* const fn = player ? R::FindDispatchFunctionCached(R::ClassOf(player), L"setActiveInterface") : nullptr;
    if (!fn || !widget) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"activeInterface", widget);
    f.Set<bool>(L"isActiveINterface3D", true);
    return ue_wrap::Call(player, f);
}

int32_t Points() {
    int32_t points = -1;
    OnGameThread([&] { ue_wrap::economy::ReadPoints(&points); });
    return points;
}

int32_t Level(int index) {
    int32_t level = -1;
    OnGameThread([&] { UP::ReadLevel(index, &level); });
    return level;
}

// The levels and the balance as one line.
std::string State() {
    std::string s;
    OnGameThread([&] {
        int32_t levels[UP::kLevelCount] = {};
        int32_t points = 0;
        UP::ReadLevels(levels);
        ue_wrap::economy::ReadPoints(&points);
        s = "points=" + std::to_string(points) + " levels=";
        for (int i = 0; i < UP::kLevelCount; ++i) s += std::to_string(levels[i]) + (i + 1 < UP::kLevelCount ? "," : "");
    });
    return s;
}

bool Press(int index) {
    bool pressed = false;
    OnGameThread([&] {
        if (void* row = LiveModuleRow(index)) pressed = ue_wrap::component_calls::CallParamlessNamed(row, UP::kBuyButtonFn);
        else UE_LOGW("module-buy: no live module row %d", index);
    });
    return pressed;
}

// One request by hand, as the client's panel gate would have sent it.
void SendIntent(int index, uint8_t dir) {
    coop::net::UpgradeIntentPayload p{};
    p.panelIndex = static_cast<uint8_t>(index);
    p.dir = dir;
    OnGameThread([&] { harness::session_runtime::Session().SendReliable(coop::net::ReliableKind::UpgradeIntent, &p, sizeof(p)); });
}

// The request of a denial arm: a buy, and for the refused arms a sell too (dir 2 and 3).
void SendCrafted(const std::string& arm) {
    const int index = arm == "nobit" ? kLevelIndex : kTriangleIndex;
    SendIntent(index, coop::net::upgrade_intent::kModuleBuy);
    if (arm != "valid") SendIntent(index, coop::net::upgrade_intent::kModuleSell);
    UE_LOGI("module-buy: arm %hs: sent the request for index %d by hand", arm.c_str(), index);
}

// The host's word, in the shape every drill ends on: said once, on the host.
void Fail(const char* what, const std::string& state) {
    UE_LOGW("module-buy: FAIL %hs -- host %hs", what, state.c_str());
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Fail, "%s", what);
}

// The ending both kinds of run share: the host's word, and the client's copy reading the host's levels and balance.
// False after a verdict was said (or the client's word never came).
bool ClientReadsHost(const std::string& hostState) {
    Mark("host_state", hostState);
    std::string client;
    if (!AwaitText("client_seen", client, kStepSec)) return false;
    if (client.empty() || client[0] != '1') {
        Fail("the client's copy does not read the host's levels and balance", client);
        return false;
    }
    return true;
}

// A denial arm, host side: the refused request changed nothing, and the sentinel behind it in the queue was served.
void JudgeDenial(const std::string& arm) {
    if (!Await("pressed_s", kStepSec)) return;
    const bool served = PS::WaitFor("the sentinel's charge alone", kEffectSec, [] { return Points() == kBalance - kStorelessPrice; }, false);
    const std::string hostState = State();
    UE_LOGI("module-buy: arm %hs: host %hs", arm.c_str(), hostState.c_str());
    if (!served || Level(kTriangleIndex) != 0)
        return Fail("a request the host must refuse charged the balance or raised the 400-credit row, or the sentinel was not served", hostState);
    if (!ClientReadsHost(hostState)) return;
    UE_LOGI("module-buy: DONE on host");
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Pass, "arm %s: refused, nothing charged but the sentinel's 30; %s", arm.c_str(),
                           hostState.c_str());
}

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    // The fixture: the row the module writes starts empty, as on a fresh save, and the group can pay.
    OnGameThread([] { UP::WriteLevel(kTriangleIndex, 0); ue_wrap::economy::WritePoints(kBalance); });
    if (!PS::WaitFor("the fixture written", 15, [] { return Points() == kBalance && Level(kTriangleIndex) == 0; })) return;
    if (!Await("client_ready", 480)) return;
    Mark("host_ready");
    if (!Await("client_in", kStepSec)) return;
    if (!Arm().empty()) return JudgeDenial(Arm());

    // The first press: the module with a level.
    if (!Await("pressed_a", kStepSec)) return;
    const bool a = PS::WaitFor("the first press's charge", kEffectSec, [] {
        return Points() == kBalance - kTrianglePrice && Level(kTriangleIndex) > 0;
    }, false);
    UE_LOGI("module-buy: step a: host %hs", State().c_str());
    if (!a) return Fail("the 400-credit module did not charge the host's balance and raise its level", State());
    Mark("ack_a");

    // The second press (free) and the third (a row with no store): the balance after the third holds both.
    if (!Await("pressed_bc", kStepSec)) return;
    const int32_t levelAfterA = Level(kTriangleIndex);
    const bool bc = PS::WaitFor("the third press's charge", kEffectSec, [] {
        return Points() == kBalance - kTrianglePrice - kStorelessPrice;
    }, false);
    const std::string hostState = State();
    UE_LOGI("module-buy: steps b and c: host %hs", hostState.c_str());
    if (!bc) return Fail("a second press of the full row was not free, or the storeless row did not charge 30", hostState);
    if (Level(kTriangleIndex) != levelAfterA) return Fail("the full row's second press raised its level", hostState);

    // The client's copy reads what the host reads.
    if (!ClientReadsHost(hostState)) return;
    UE_LOGI("module-buy: DONE on host");
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Pass, "400 charged once, the full row free, 30 charged by a storeless row; %s",
                           hostState.c_str());
}

// The client's ending: its copy converges on the host's word.
void ClientEnds() {
    std::string host;
    if (!AwaitText("host_state", host, kStepSec)) return;
    const bool same = PS::WaitFor("this client reading the host's levels and balance", kEffectSec, [&] { return State() == host; }, false);
    UE_LOGI("module-buy: client %hs, host %hs", State().c_str(), host.c_str());
    Mark("client_seen", same ? std::string("1") : "0 " + State());
    UE_LOGI("module-buy: DONE on client");
}

void RunClient() {
    if (!disc_drill::HostPuppetUp()) return;
    if (!meet::WalkToHost(kTag)) return;
    Mark("client_ready");
    if (!Await("host_ready", kStepSec)) return;
    const std::string arm = Arm();
    // Not at the laptop yet: the host's reach test refuses this one, and the fixture it would have bought is written.
    if (arm == "reach") SendCrafted(arm);
    // Into the laptop: the host accepts a purchase from the player holding it.
    bool entered = false;
    OnGameThread([&] { entered = EnterLaptop(); });
    if (!entered) return coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "the laptop could not be entered");
    if (!PS::WaitFor("the laptop held here", 15, [] {
            uint8_t holder = 0xFF;
            OnGameThread([&] { holder = coop::device_occupancy::HolderOf(L"laptop"); });
            return holder != 0xFF;
        }))
        return;
    if (!PS::WaitFor("the host's fixture read here", kStepSec, [] { return Points() == kBalance && Level(kTriangleIndex) == 0; })) return;
    if (!PS::WaitFor("the live module rows", kStepSec, [] {
            bool both = false;
            OnGameThread([&] { both = LiveModuleRow(kTriangleIndex) && LiveModuleRow(kStorelessIndex); });
            return both;
        }))
        return;
    Mark("client_in");

    if (!arm.empty()) {
        // The refused request, then the sentinel behind it in the one queue the host serves this sender from.
        if (arm != "reach") SendCrafted(arm);
        if (!Press(kStorelessIndex)) return coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "the sentinel press did not run");
        Mark("pressed_s");
        return ClientEnds();
    }
    if (!Press(kTriangleIndex)) return coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "the first press did not run");
    Mark("pressed_a");
    if (!Await("ack_a", kStepSec)) return;
    const bool b = Press(kTriangleIndex);
    const bool c = Press(kStorelessIndex);
    if (!b || !c) return coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "a press did not run (second %d, third %d)", b ? 1 : 0, c ? 1 : 0);
    Mark("pressed_bc");
    ClientEnds();
}

}  // namespace

DWORD WINAPI ModuleBuyThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (!KnownArm(Arm())) {
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "VOTVCOOP_MODULE_BUY_ARM '%s' is not nobit, reach or valid", Arm().c_str());
        return 0;
    }
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest

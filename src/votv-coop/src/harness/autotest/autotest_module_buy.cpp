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
// what the first three charged.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/interactables/device_occupancy.h"
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
constexpr int kTrianglePrice = 400;
constexpr int kStorelessPrice = 30;
constexpr int kBalance = 5000;
constexpr int kStepSec = 60;         // the bound on one step of the other peer's
constexpr int kEffectSec = 30;       // the bound on the host's balance moving after a press
const wchar_t* const kBuy = L"BndEvt__button_upgDownloadSpd_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature";

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
        if (void* row = LiveModuleRow(index)) pressed = ue_wrap::component_calls::CallParamlessNamed(row, kBuy);
        else UE_LOGW("module-buy: no live module row %d", index);
    });
    return pressed;
}

// The host's word, in the shape every drill ends on: said once, on the host.
void Fail(const char* what, const std::string& state) {
    UE_LOGW("module-buy: FAIL %hs -- host %hs", what, state.c_str());
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Fail, "%s", what);
}

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    // The fixture: the row the module writes starts empty, as on a fresh save, and the group can pay.
    OnGameThread([] { UP::WriteLevel(kTriangleIndex, 0); ue_wrap::economy::WritePoints(kBalance); });
    if (!PS::WaitFor("the fixture written", 15, [] { return Points() == kBalance && Level(kTriangleIndex) == 0; })) return;
    if (!Await("client_ready", 480)) return;
    Mark("host_ready");
    if (!Await("client_in", kStepSec)) return;

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
    Mark("host_state", hostState);
    std::string client;
    if (!AwaitText("client_seen", client, kStepSec)) return;
    if (client.empty() || client[0] != '1') return Fail("the client's copy does not read the host's levels and balance", client);
    UE_LOGI("module-buy: DONE on host");
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Pass, "400 charged once, the full row free, 30 charged by a storeless row; %s",
                           hostState.c_str());
}

void RunClient() {
    if (!disc_drill::HostPuppetUp()) return;
    if (!meet::WalkToHost(kTag)) return;
    Mark("client_ready");
    if (!Await("host_ready", kStepSec)) return;
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

    if (!Press(kTriangleIndex)) return coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "the first press did not run");
    Mark("pressed_a");
    if (!Await("ack_a", kStepSec)) return;
    const bool b = Press(kTriangleIndex);
    const bool c = Press(kStorelessIndex);
    if (!b || !c) return coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "a press did not run (second %d, third %d)", b ? 1 : 0, c ? 1 : 0);
    Mark("pressed_bc");

    std::string host;
    if (!AwaitText("host_state", host, kStepSec)) return;
    const bool same = PS::WaitFor("this client reading the host's levels and balance", kEffectSec, [&] { return State() == host; }, false);
    UE_LOGI("module-buy: client %hs, host %hs", State().c_str(), host.c_str());
    Mark("client_seen", same ? std::string("1") : "0 " + State());
    UE_LOGI("module-buy: DONE on client");
}

}  // namespace

DWORD WINAPI ModuleBuyThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest

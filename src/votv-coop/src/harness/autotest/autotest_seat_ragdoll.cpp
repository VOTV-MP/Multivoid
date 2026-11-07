// harness/autotest/autotest_seat_ragdoll.cpp -- TEST-ONLY: a device's claim when its user ragdolls.
//
// One peer (the client, or the host with VOTVCOOP_SEAT_RAGDOLL_WHO=host) enters a device the way the game's enter ends
// (setActiveInterface): the laptop, or with VOTVCOOP_SEAT_RAGDOLL_DEVICE=desk or sat the desk's coordinates screen or
// the SAT console. Once the other peer reads its claim, it ragdolls as a faint does (ragdollMode(true, passOut, false)).
// Both peers log who holds the device every second for 8 s, then the other peer enters it. Arms:
// VOTVCOOP_SEAT_RAGDOLL_PING=1 (the host, at the desk) starts a ping before the faint, and VOTVCOOP_SEAT_RAGDOLL_EXIT=1
// leaves by the game's own exit instead of a ragdoll. The host says `rig: VERDICT seat-ragdoll PASS|FAIL|INVALID`: PASS
// when the claim is free within 5 s and the other peer's enter stands, or, with both arms, when the pinger keeps the
// desk and the enter is refused. Gated by VOTVCOOP_RUN_SEAT_RAGDOLL; the peers' folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"
#include "disc_drills.h"   // co-located private header (src tree, not include/)
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/interactables/desk_input_sync.h"
#include "coop/interactables/device_occupancy.h"
#include "coop/player/players_registry.h"
#include "coop/session/rig_verdict.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/console_desk.h"
#include "ue_wrap/desk/device_screen.h"
#include "ue_wrap/devices/laptop_widget.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace harness::autotest {
namespace {

namespace PS = harness::autotest::peer_signal;
namespace R = ue_wrap::reflection;
using disc_drill::OnGameThread;

constexpr const char* kTag = "seat-ragdoll";
constexpr int kStepSec = 120;

std::string Device() {
    const std::string d = coop::config::ReadEnv("VOTVCOOP_SEAT_RAGDOLL_DEVICE");
    return d.empty() ? "laptop" : d;
}
const wchar_t* Key() { return Device() == "desk" ? L"desk" : Device() == "sat" ? L"sat" : L"laptop"; }

// The device's widget: the laptop's, or the first live instance of the desk's or the SAT console's shared widget.
void* DeviceWidget() {
    if (Device() == "laptop") return ue_wrap::laptop_widget::Widget();
    void* const cls = R::FindClass(Device() == "desk" ? L"ui_consolesAtlas_C" : L"ui_console_C");
    if (!cls) return nullptr;
    void* found = nullptr;
    ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        void*& out = *static_cast<void**>(c);
        if (!out && obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) out = obj;
    }, &found);
    return found;
}

bool EnterDevice() {
    void* player = coop::players::Registry::Get().Local();
    void* widget = DeviceWidget();
    void* const fn = player ? R::FindDispatchFunctionCached(R::ClassOf(player), L"setActiveInterface") : nullptr;
    if (!fn || !widget) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<void*>(L"activeInterface", widget);
    f.Set<bool>(L"isActiveINterface3D", true);
    return ue_wrap::Call(player, f);
}

unsigned Holder() {
    unsigned h = 0xFF;
    OnGameThread([&] { h = coop::device_occupancy::HolderOf(Key()); });
    return h;
}

bool InInterface() {
    bool in = false;
    OnGameThread([&] {
        void* p = coop::players::Registry::Get().Local();
        in = p && ue_wrap::device_screen::ReadActiveInterface(p) != nullptr;
    });
    return in;
}

void LogState(const char* who, int t) {
    uint8_t ping = 0xFF;
    OnGameThread([&] { ping = coop::desk_input_sync::PingActiveSlot(); });
    UE_LOGI("seat-ragdoll: %hs t=%d holder=%u inInterface=%d pinger=%u", who, t, Holder(), InInterface() ? 1 : 0, ping);
}

// Which peer enters and ragdolls (VOTVCOOP_SEAT_RAGDOLL_WHO=host or client, the client by default); the other watches.
// VOTVCOOP_SEAT_RAGDOLL_PING=1: the actor (the host) starts a ping on the desk before it ragdolls, as a player who faints
// mid-ping would; its machine keeps running after the faint.
bool Ping() { return coop::config::ReadEnv("VOTVCOOP_SEAT_RAGDOLL_PING") == "1"; }

// VOTVCOOP_SEAT_RAGDOLL_EXIT=1: the actor leaves the device by the game's own exit instead of a ragdoll. With the ping
// arm the desk stays the pinger's while its machine runs, so the other peer's enter must be refused.
bool ExitOnly() { return coop::config::ReadEnv("VOTVCOOP_SEAT_RAGDOLL_EXIT") == "1"; }

bool HostActs() { return coop::config::ReadEnv("VOTVCOOP_SEAT_RAGDOLL_WHO") == "host"; }

void Act(const char* me) {
    bool entered = false;
    OnGameThread([&] { entered = EnterDevice(); });
    UE_LOGI("seat-ragdoll: %hs entered the %hs (call=%d)", me, Device().c_str(), entered ? 1 : 0);
    PS::WaitFor("this peer's claim", 10, [] { return InInterface() && Holder() != 0xFF; }, false);
    LogState(me, 0);
    if (Ping()) {
        bool wrote = false;
        OnGameThread([&] {
            ue_wrap::console_desk::Scalars sc;
            if (ue_wrap::console_desk::EnsureResolved() && ue_wrap::console_desk::ReadScalars(sc)) {
                sc.coordIsPing = true;
                wrote = ue_wrap::console_desk::WriteScalars(sc, ue_wrap::console_desk::kPaintNone);
            }
        });
        const bool running = PS::WaitFor("the desk's ping machine attributed", 10, [] {
            uint8_t slot = 0xFF;
            OnGameThread([&] { slot = coop::desk_input_sync::PingActiveSlot(); });
            return slot != 0xFF;
        }, false);
        UE_LOGI("seat-ragdoll: %hs started a ping (write=%d attributed=%d)", me, wrote ? 1 : 0, running ? 1 : 0);
    }
    PS::Mark("actor_in");
    if (!PS::Await("observer_saw_claim", kStepSec)) return;
    bool rag = false;
    OnGameThread([&] {
        void* p = coop::players::Registry::Get().Local();
        rag = p && (ExitOnly() ? ue_wrap::device_screen::ForceExitInterface(p)
                               : ue_wrap::engine::SetMainPlayerRagdollMode(p, true, true, false));
    });
    UE_LOGI("seat-ragdoll: %hs %hs (call=%d)", me, ExitOnly() ? "left by the game's exit" : "ragdolled as a faint", rag ? 1 : 0);
    PS::Mark("actor_ragdolled");
    for (int t = 1; t <= 8; ++t) { ::Sleep(1000); LogState(me, t); }
}

// The watcher: the actor's claim here, freed after the ragdoll, and this peer's own enter. "claimed freedAt stays".
std::string Watch(const char* me, unsigned actorSlot) {
    if (!PS::Await("actor_in", kStepSec)) return "";
    PS::WaitFor("the actor's claim here", 10, [actorSlot] { return Holder() == actorSlot; }, false);
    LogState(me, 0);
    const bool claimed = Holder() == actorSlot;
    PS::Mark("observer_saw_claim");
    if (!PS::Await("actor_ragdolled", kStepSec)) return "";
    int freedAt = -1;
    for (int t = 1; t <= 8; ++t) {
        ::Sleep(1000);
        LogState(me, t);
        if (freedAt < 0 && Holder() == 0xFF) freedAt = t;
    }
    bool entered = false;
    OnGameThread([&] { entered = EnterDevice(); });
    ::Sleep(2000);
    const bool stays = InInterface();
    LogState(me, 10);
    UE_LOGI("seat-ragdoll: %hs claimed=%d freedAt=%d enter=%d stays=%d", me, claimed ? 1 : 0, freedAt, entered ? 1 : 0, stays ? 1 : 0);
    return std::to_string(claimed ? 1 : 0) + " " + std::to_string(freedAt) + " " + std::to_string(stays ? 1 : 0);
}

void Judge(const std::string& r) {
    int claimed = 0, freedAt = -1, stays = 0;
    if (r.empty() || std::sscanf(r.c_str(), "%d %d %d", &claimed, &freedAt, &stays) != 3)
        return coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "the watcher's word never came");
    // The exit arm mid-ping: the pinger's machine keeps the desk, so the other peer is refused and nothing frees it.
    const bool pass = ExitOnly() && Ping() ? claimed && freedAt < 0 && !stays : claimed && freedAt > 0 && freedAt <= 5 && stays;
    coop::rig_verdict::Say(kTag, pass ? coop::rig_verdict::Outcome::Pass : coop::rig_verdict::Outcome::Fail,
                           "%s %s at the %s: claimed=%d, freed after %d s, the other peer's enter %s",
                           HostActs() ? "the host" : "the client", ExitOnly() ? "left" : "ragdolled", Device().c_str(),
                           claimed, freedAt,
                           stays ? "stands" : "was refused");
}

void RunClient() {
    if (!disc_drill::HostPuppetUp()) return;
    PS::Mark("client_ready");
    if (!PS::Await("host_ready", kStepSec)) return;
    if (HostActs()) PS::Mark("watch_result", Watch("CLIENT", 0));
    else Act("CLIENT");
    PS::Await("host_done", kStepSec);
    UE_LOGI("seat-ragdoll: DONE on client");
}

void RunHost() {
    if (!disc_drill::HostWorldUp()) return;
    if (!PS::Await("client_ready", 480)) return;
    PS::Mark("host_ready");
    std::string r;
    if (HostActs()) {
        Act("HOST");
        PS::AwaitText("watch_result", r, kStepSec);
    } else {
        r = Watch("HOST", 1);
    }
    PS::Mark("host_done");
    Judge(r);
    UE_LOGI("seat-ragdoll: DONE on host");
}

}  // namespace

DWORD WINAPI SeatRagdollThread(LPVOID /*arg*/) {
    if (!PS::Begin(kTag)) return 0;
    if (!IsClientRole()) RunHost();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest

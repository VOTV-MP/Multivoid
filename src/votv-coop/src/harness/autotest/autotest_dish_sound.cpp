// harness/autotest/autotest_dish_sound.cpp -- TEST-ONLY: a dish slew's sounds on the host and on a client's parked copy.
//
// The host runs one dish's own startMovingTo, and each peer logs every edge of that dish's hum and motor with its time. The
// host then judges the two edge sequences itself: equal is PASS, else FAIL with both. The clunks and the last one-shot are
// read from the lane's own lines, since a one-shot's state is too brief to sample. Gated by VOTVCOOP_RUN_DISH_SOUND; the
// folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"
#include "harness/autotest/peer_signal.h"

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/desk/dish.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <atomic>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace D = ue_wrap::dish;
namespace PS = harness::autotest::peer_signal;
using PS::Await;
using PS::Mark;
using PS::WaitFor;

constexpr int32_t kDish = 0;
constexpr int     kWatchSeconds = 120;
constexpr float   kAimOff = 9000.f;
constexpr DWORD   kEndGapMs = 1000;   // the lane waits this after the fade before its last sound (dish_sync.cpp)

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

// Every edge of the dish's hum and motor, until the slew ends: the hum has been on, then both are off for the end gap.
// `edges` receives the state after each change ("H1M1 H1M0 H0M0"). False, with an ABORT line, when the slew does not end
// within the bound.
bool Watch(const char* who, std::string& edges) {
    bool hum = false, motor = false, sawHum = false;
    DWORD offSince = 0;
    const DWORD start = ::GetTickCount();
    while (::GetTickCount() - start < kWatchSeconds * 1000u) {
        bool nowHum = false, nowMotor = false;
        OnGameThread([&] { D::ReadCues(kDish, nowHum, nowMotor); });
        if (nowHum != hum || nowMotor != motor) {
            UE_LOGI("dish-sound: %hs t=%.1f hum=%d motor=%d", who, (::GetTickCount() - start) / 1000.f, nowHum ? 1 : 0, nowMotor ? 1 : 0);
            hum = nowHum;
            motor = nowMotor;
            if (!edges.empty()) edges += ' ';
            edges += hum ? "H1" : "H0";
            edges += motor ? "M1" : "M0";
            sawHum = sawHum || hum;
            offSince = (sawHum && !hum && !motor) ? ::GetTickCount() : 0;
        }
        if (offSince && ::GetTickCount() - offSince >= kEndGapMs) return true;
        ::Sleep(50);
    }
    UE_LOGW("dish-sound: VERDICT ABORT -- the slew did not end within %ds", kWatchSeconds);
    return false;
}

void RunHost() {
    if (!Await("client_ready")) return;
    if (!WaitFor("the dish resolvable", 30, [&] {
            void* dish = nullptr;
            OnGameThread([&] { dish = D::DishByIndex(kDish); });
            return dish != nullptr;
        }))
        return;
    OnGameThread([&] {
        void* dish = D::DishByIndex(kDish);
        void* fn = dish ? R::FindDispatchFunctionCached(R::ClassOf(dish), L"startMovingTo") : nullptr;
        ue_wrap::FVector at{};
        if (!fn || !E::TryGetActorLocation(dish, at)) {
            UE_LOGW("dish-sound: VERDICT ABORT -- no dish to move");
            return;
        }
        ue_wrap::ParamFrame f(fn);
        if (f.valid() && f.Set<ue_wrap::FVector>(L"lookAt", {at.X + kAimOff, at.Y - kAimOff, at.Z + kAimOff})) ue_wrap::Call(dish, f);
        UE_LOGI("dish-sound: the host's dish was told to move");
    });
    Mark("go");
    std::string hostEdges, clientEdges;
    if (!Watch("host", hostEdges)) return;
    if (!PS::AwaitText("client_edges", clientEdges)) return;
    if (hostEdges == clientEdges) UE_LOGI("dish-sound: VERDICT PASS");
    else UE_LOGW("dish-sound: VERDICT FAIL host=%hs client=%hs", hostEdges.c_str(), clientEdges.c_str());
    Mark("host_done");
    UE_LOGI("dish-sound: DONE on host");
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    if (!WaitFor("the host's puppet", 60, [&] {
            bool have = false;
            OnGameThread([&] {
                coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
                have = p && p->valid();
            });
            return have;
        }))
        return;
    Mark("client_ready");
    if (!Await("go")) return;
    std::string edges;
    Watch("client", edges);
    // A slew that never ended still reports what it heard, so the host judges it and does not wait out a clock.
    PS::Mark("client_edges", edges.empty() ? "none" : edges);
    UE_LOGI("dish-sound: DONE on client");
}

}  // namespace

DWORD WINAPI DishSoundThread(LPVOID /*arg*/) {
    if (!PS::Begin("dish-sound")) return 0;
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest

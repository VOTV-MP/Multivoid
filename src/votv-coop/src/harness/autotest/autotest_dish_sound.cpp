// harness/autotest/autotest_dish_sound.cpp -- TEST-ONLY: a dish slew's sounds on the host and on a client's parked copy.
//
// The host runs one dish's own startMovingTo, and each peer logs every edge of that dish's hum and motor with its time, for
// the driver to compare. The clunks and the last one-shot are read from the lane's own lines, since a one-shot's state is too
// brief to sample. Gated by VOTVCOOP_RUN_DISH_SOUND; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
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
#include <fstream>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace D = ue_wrap::dish;

constexpr int32_t kDish = 0;
constexpr int     kWatchSeconds = 120;
constexpr float   kAimOff = 9000.f;

std::string g_dir;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Mark(const char* name) { std::ofstream(g_dir + "\\" + name, std::ios::binary | std::ios::trunc) << "x"; }

bool Await(const char* name, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) return true;
        ::Sleep(100);
    }
    UE_LOGW("dish-sound: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

// Every edge of the dish's hum and motor, until the watch ends.
void Watch(const char* who) {
    bool hum = false, motor = false;
    const DWORD start = ::GetTickCount();
    while (::GetTickCount() - start < kWatchSeconds * 1000u) {
        bool nowHum = false, nowMotor = false;
        OnGameThread([&] { D::ReadCues(kDish, nowHum, nowMotor); });
        if (nowHum != hum || nowMotor != motor) {
            UE_LOGI("dish-sound: %hs t=%.1f hum=%d motor=%d", who, (::GetTickCount() - start) / 1000.f, nowHum ? 1 : 0, nowMotor ? 1 : 0);
            hum = nowHum;
            motor = nowMotor;
        }
        ::Sleep(50);
    }
}

void RunHost() {
    if (!Await("client_ready")) return;
    ::Sleep(3000);
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
    Watch("host");
    Mark("host_done");
    UE_LOGI("dish-sound: DONE on host");
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(8000);
    Mark("client_ready");
    if (!Await("go")) return;
    Watch("client");
    UE_LOGI("dish-sound: DONE on client");
}

}  // namespace

DWORD WINAPI DishSoundThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("dish-sound: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest

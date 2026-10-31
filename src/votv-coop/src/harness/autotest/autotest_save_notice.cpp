// harness/autotest/autotest_save_notice.cpp -- TEST-ONLY: what every player hears of the host's saves.
//
// The host runs the game's own mainGamemode.save three ways with a client joined: with no event, with an event
// counted by the game's own lib_C.setEvent, and with no event again. It also runs save with bypassEvent during the
// event, which is the path that writes without gathering. Each step logs `save-drill: STEP <name> event=<0|1>`;
// the driver reads each peer's save_notice lines after it. Gated by VOTVCOOP_RUN_SAVE_NOTICE; the peers' shared
// folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/save_capture.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <atomic>
#include <fstream>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;

std::string g_dir;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Mark(const char* name) {
    std::ofstream(g_dir + "\\" + name, std::ios::binary | std::ios::trunc) << "x";
}

bool Await(const char* name, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) return true;
        ::Sleep(100);
    }
    UE_LOGW("save-drill: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void SetEvent(bool on) {
    OnGameThread([&] {
        void* const gm = ue_wrap::world_singleton::Gamemode();
        void* const lib = R::FindClassDefaultObject(L"lib_C");
        void* const fn = lib ? R::FindFunction(R::ClassOf(lib), L"setEvent") : nullptr;
        if (!gm || !fn) { UE_LOGW("save-drill: setEvent did not resolve"); return; }
        ue_wrap::ParamFrame f(fn);
        f.Set<bool>(L"isEventActive", on);
        f.Set<bool>(L"deactivateAmbientTrack", false);
        f.Set<void*>(L"__WorldContext", gm);
        const bool ok = f.valid() && ue_wrap::Call(lib, f);
        UE_LOGI("save-drill: setEvent(%d) ran (ok=%d)", on ? 1 : 0, ok ? 1 : 0);
    });
}

void Save(const char* step, bool bypassEvent) {
    OnGameThread([&] {
        void* const gm = ue_wrap::world_singleton::Gamemode();
        void* const fn = gm ? R::FindFunction(R::ClassOf(gm), L"save") : nullptr;
        const auto ev = ue_wrap::save_capture::GameEventState();
        UE_LOGI("save-drill: STEP %hs event=%d bypass=%d", step,
                ev == ue_wrap::save_capture::EventState::On ? 1 : 0, bypassEvent ? 1 : 0);
        if (!fn) { UE_LOGW("save-drill: save did not resolve"); return; }
        ue_wrap::ParamFrame f(fn);
        f.Set<bool>(L"quicksave", false);
        f.Set<bool>(L"bypassEvent", bypassEvent);
        f.Set<bool>(L"overwriteSubsave", false);
        const bool ok = f.valid() && ue_wrap::Call(gm, f);
        UE_LOGI("save-drill: save ran (ok=%d)", ok ? 1 : 0);
    });
    ::Sleep(4000);   // the game's save runs across latent delays, and the line crosses to the client
}

void RunHost() {
    if (!Await("client_ready")) return;
    Save("1-no-event", false);
    SetEvent(true);
    ::Sleep(500);
    Save("2-event", false);
    Save("3-event-bypass", true);
    SetEvent(false);
    ::Sleep(500);
    Save("4-no-event-again", false);
    Mark("host_done");
    UE_LOGI("save-drill: DONE");
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(8000);
    Mark("client_ready");
    if (!Await("host_done")) return;
    ::Sleep(1000);
    UE_LOGI("save-drill: DONE");
}

}  // namespace

DWORD WINAPI SaveNoticeThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("save-drill: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else                RunHost();
    return 0;
}

}  // namespace harness::autotest

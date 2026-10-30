// harness/autotest/autotest_desk_view.cpp -- TEST-ONLY: the desk's signal view settings across two peers.
//
// The bounds (remapValue, moved by the desk's own scrollRemap verbs) and the mode (visualizerMode, graph or spectrogram).
// The host changes both before the client joins, so the join carries them; then the host moves a bound, and the client
// moves a bound and flips the mode. Each peer logs `desk-view: STATE <step> min= max= mode=` after each step for the
// driver to compare; a file per step orders the two peers. Gated by VOTVCOOP_RUN_DESK_VIEW; the folder is
// VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/console_desk.h"

#include <windows.h>

#include <atomic>
#include <fstream>
#include <string>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace CD = ue_wrap::console_desk;

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
    UE_LOGW("desk-view: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void LogState(const char* step) {
    OnGameThread([&] {
        CD::Scalars sc;
        if (!CD::EnsureResolved() || !CD::ReadScalars(sc)) {
            UE_LOGW("desk-view: STATE %hs -- the desk did not resolve", step);
            return;
        }
        UE_LOGI("desk-view: STATE %hs min=%.2f max=%.2f mode=%d", step, sc.remapMin, sc.remapMax,
                sc.visualizerMode ? 1 : 0);
    });
}

// One of the desk's own no-argument verbs, as a scroll press runs it.
void Verb(const wchar_t* name) {
    OnGameThread([&] {
        void* d = CD::EnsureResolved() ? CD::Instance() : nullptr;
        void* fn = d ? R::FindDispatchFunctionCached(R::ClassOf(d), name) : nullptr;
        const bool ok = fn && ue_wrap::component_calls::CallParamless(d, fn);
        UE_LOGI("desk-view: %ls ran (ok=%d)", name, ok ? 1 : 0);
    });
}

// The red button's body: the flag flipped, setCanvas and endCanvas.
void FlipMode() {
    OnGameThread([&] {
        CD::Scalars sc;
        if (!CD::EnsureResolved() || !CD::ReadScalars(sc)) return;
        const bool ok = CD::ApplyVisualizerMode(!sc.visualizerMode);
        UE_LOGI("desk-view: mode flipped to %d (ok=%d)", sc.visualizerMode ? 0 : 1, ok ? 1 : 0);
    });
}

void Settle() { ::Sleep(1500); }

void RunHost() {
    ::Sleep(20000);   // the world up, before any client: these changes reach it only through the join
    Verb(L"scrollRemapMaxUp");
    Verb(L"scrollRemapMaxUp");
    FlipMode();
    Settle();
    LogState("0-before-join");
    Mark("host_prepared");
    if (!Await("client_ready")) return;
    Verb(L"scrollRemapMinUp");
    Settle();
    LogState("2-host-moved-min");
    Mark("host_moved");
    if (!Await("client_changed")) return;
    Settle();
    LogState("3-client-changed");
    Mark("host_done");
    UE_LOGI("desk-view: DONE");
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(8000);
    LogState("1-after-join");
    Mark("client_ready");
    if (!Await("host_moved")) return;
    Settle();
    LogState("2-host-moved-min");
    Verb(L"scrollRemapMaxDown");
    FlipMode();
    Settle();
    LogState("3-client-changed");
    Mark("client_changed");
    if (!Await("host_done")) return;
    UE_LOGI("desk-view: DONE");
}

}  // namespace

DWORD WINAPI DeskViewThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("desk-view: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else                RunHost();
    return 0;
}

}  // namespace harness::autotest

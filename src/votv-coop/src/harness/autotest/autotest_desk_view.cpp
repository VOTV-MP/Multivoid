// harness/autotest/autotest_desk_view.cpp -- TEST-ONLY: the desk's signal view settings across two peers.
//
// The bounds (remapValue, moved by the desk's own scrollRemap verbs) and the mode (visualizerMode, graph or spectrogram).
// The host changes both before the client joins, so the join carries them; then the host moves a bound, and the client
// moves a bound and flips the mode. Each peer logs `desk-view: STATE <step> min= max= mode=` after each step for the
// driver to compare; a file per step orders the two peers (host_prepared carries the host's state as text, and the
// joiner reads its own only once it equals that). Gated by VOTVCOOP_RUN_DESK_VIEW; the folder is
// VOTVCOOP_CAN_DIR. Signals count only when written after this drill began, and every wait is on a game state with
// a bound, never on a clock.

#include "harness/autotest.h"
#include "harness/autotest/peer_signal.h"

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/desk/console_desk.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace harness::autotest {
namespace {

namespace R  = ue_wrap::reflection;
namespace CD = ue_wrap::console_desk;
namespace PS = harness::autotest::peer_signal;
using PS::Await;
using PS::AwaitText;
using PS::Mark;
using PS::WaitFor;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

// The three view settings as this peer reads them now; ok is false while the desk does not resolve.
struct View {
    bool ok = false;
    float mn = 0, mx = 0;
    bool mode = false;
};

View ReadView() {
    View v;
    OnGameThread([&] {
        CD::Scalars sc;
        if (!CD::EnsureResolved() || !CD::ReadScalars(sc)) return;
        v.ok = true;
        v.mn = sc.remapMin;
        v.mx = sc.remapMax;
        v.mode = sc.visualizerMode;
    });
    return v;
}

// `min max mode`, formatted as the STATE line formats them: the host sends this text and the joiner compares its own.
std::string FormatView(const View& v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f %.2f %d", v.mn, v.mx, v.mode ? 1 : 0);
    return buf;
}

bool Differs(float a, float b) { return std::fabs(a - b) > 1e-4f; }

// Waits for the view to show the expected change from `was`: the named bound moved, the mode flipped. A timeout logs
// nothing here: the step's STATE line right after carries the mismatch to the driver, which is the FAIL.
void WaitViewChanged(const View& was, bool minMoved, bool maxMoved, bool modeFlipped) {
    WaitFor("the desk view changed", 30, [&] {
        const View v = ReadView();
        return v.ok && (!minMoved || Differs(v.mn, was.mn)) && (!maxMoved || Differs(v.mx, was.mx)) &&
               (!modeFlipped || v.mode != was.mode);
    }, false);
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

void RunHost() {
    // The world up, before any client: these changes reach it only through the join. The client's first signal is
    // awaited after, so no client is in before the desk is readable.
    if (!WaitFor("the desk readable", 120, [&] { return ReadView().ok; })) return;
    const View base = ReadView();
    Verb(L"scrollRemapMaxUp");
    Verb(L"scrollRemapMaxUp");
    FlipMode();
    WaitViewChanged(base, false, true, true);
    LogState("0-before-join");
    Mark("host_prepared", FormatView(ReadView()));
    if (!Await("client_ready")) return;
    const View beforeMin = ReadView();
    Verb(L"scrollRemapMinUp");
    WaitViewChanged(beforeMin, true, false, false);
    LogState("2-host-moved-min");
    Mark("host_moved");
    const View moved = ReadView();
    if (!Await("client_changed")) return;
    WaitViewChanged(moved, false, true, true);
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
    if (!WaitFor("the host's puppet", 60, [&] {
            bool have = false;
            OnGameThread([&] {
                coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
                have = p && p->valid();
            });
            return have;
        }))
        return;
    // The join snapshot applies after the puppet shows: read the state only once it equals what the host prepared.
    // host_prepared is marked before the host awaits client_ready, so this wait cannot deadlock.
    std::string hostState;
    if (!AwaitText("host_prepared", hostState)) return;
    WaitFor("the host's desk state arrived", 30, [&] {
        const View v = ReadView();
        return v.ok && FormatView(v) == hostState;
    }, false);
    LogState("1-after-join");
    const View joined = ReadView();
    Mark("client_ready");
    if (!Await("host_moved")) return;
    WaitViewChanged(joined, true, false, false);
    LogState("2-host-moved-min");
    const View seen = ReadView();
    Verb(L"scrollRemapMaxDown");
    FlipMode();
    WaitViewChanged(seen, false, true, true);
    LogState("3-client-changed");
    Mark("client_changed");
    if (!Await("host_done")) return;
    UE_LOGI("desk-view: DONE");
}

}  // namespace

DWORD WINAPI DeskViewThread(LPVOID /*arg*/) {
    if (!PS::Begin("desk-view")) return 0;
    if (IsClientRole()) RunClient();
    else                RunHost();
    return 0;
}

}  // namespace harness::autotest

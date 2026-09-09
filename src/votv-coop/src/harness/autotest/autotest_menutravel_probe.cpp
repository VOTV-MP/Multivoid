// harness/autotest/autotest_menutravel_probe.cpp -- SP-solo probe for VOTV's menu-travel verb.
//
// A dead player has to be able to leave the gameplay world for the main menu, and neither
// engine verb does it: `disconnect` is a no-op (VOTV is single-player, no netdriver) and a
// bare `open menu` leaves the live UWorld on `untitled`. VOTV travels through its own
// AmainGamemode_C::transition(FName LevelName), which needs no pause and therefore works
// with the player ragdolled.
//
// The probe settles in gameplay, optionally dwells, then dispatches transition("/Game/menu")
// in one game-thread task. A held transparent bypass goes first by default, because our own
// detour hangs the untitled_1 teardown; flat RSS across the run is the pass condition.
//
// Gated by env VOTVCOOP_RUN_MENUTRAVEL_PROBE=1. Throwaway diagnostic, not a shipping path.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "harness/session_runtime.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;
namespace GT = ue_wrap::game_thread;
namespace E = ue_wrap::engine;

// Bounded spin-wait on a game-thread task's completion flag (mirrors the sibling
// probes). false => the posted task faulted (SEH firewall ate the AV, flag never
// set) so the caller bails instead of hanging.
bool WaitDone(const std::shared_ptr<std::atomic<int>>& d, int timeoutMs) {
    for (int i = 0; i < timeoutMs / 5 && d->load() == 0; ++i) ::Sleep(5);
    return d->load() != 0;
}

// Read the live UWorld leaf name on the game thread. "<null>" if no world (a world
// pointer can be momentarily null mid-travel).
std::wstring WorldNameGT() {
    auto done = std::make_shared<std::atomic<int>>(0);
    auto out  = std::make_shared<std::wstring>(L"<null>");
    GT::Post([done, out] {
        if (void* w = R::FindObjectByClass(P::name::WorldClass)) {
            *out = R::ToString(R::NameOf(w));
        }
        done->store(1);
    });
    WaitDone(done, 8000);
    return *out;
}

// The gameplay world's leaf name contains "ntitled" (untitled_1.Untitled_1); the
// menu / preLoad / loading worlds do not. So "left gameplay" == name lacks it.
bool InGameplay(const std::wstring& worldName) {
    return worldName.find(L"ntitled") != std::wstring::npos;
}

// Call AmainGamemode_C::transition(FName LevelName) INLINE (caller on the game
// thread). VOTV's own level-travel verb; "/Game/menu" (full path) travels to the menu.
// Works regardless of player state (no pause needed) -> dead-player-safe.
bool CallTransitionInline(const std::wstring& levelName) {
    void* gm = R::FindObjectByClass(P::name::GamemodeClass);
    if (!gm || !R::IsLive(gm)) { UE_LOGW("menutravel: no live mainGamemode_C"); return false; }
    void* fn = R::FindFunction(R::ClassOf(gm), L"transition");
    if (!fn) { UE_LOGW("menutravel: mainGamemode_C::transition not resolved"); return false; }
    R::FName ln = ue_wrap::fname_utils::StringToFName(levelName);
    ue_wrap::ParamFrame f(fn);
    if (!f.valid() || !f.SetRaw(L"LevelName", &ln, sizeof(ln))) {
        UE_LOGW("menutravel: transition SetRaw failed for '%ls'", levelName.c_str());
        return false;
    }
    return ue_wrap::Call(gm, f);
}

void RunProbe() {
    UE_LOGI("menutravel: === menu-travel BYPASS probe START ===");

    // Settle: wait until we're in gameplay (`untitled`), up to ~40 s (covers VOTV's
    // boot-time `open untitled_1` level travel). Uses the pump -- bypass not armed yet.
    // WAIT_SESSION mode gets 240 s: a save-transfer JOIN reaches gameplay only after
    // boot + menu + auto-connect + transfer + world load, well past the solo cap.
    const int settleCap =
        (coop::config::ReadEnv("VOTVCOOP_MENUTRAVEL_WAIT_SESSION") == "1") ? 240 : 40;
    std::wstring w;
    for (int i = 0; i < settleCap; ++i) {
        w = WorldNameGT();
        if (InGameplay(w)) break;
        ::Sleep(1000);
    }
    if (!InGameplay(w)) {
        UE_LOGW("menutravel: never reached gameplay (world='%ls') -- abort", w.c_str());
        UE_LOGI("menutravel: DONE");
        return;
    }
    UE_LOGI("menutravel: in gameplay (world='%ls') -- arming held bypass + transition", w.c_str());

    // VOTVCOOP_MENUTRAVEL_DWELL_S: reaching gameplay is not the same as HAVING PLAYED. Without
    // a dwell this probe fires `transition` within ~1 s of the world coming up -- before the
    // overlay's first present, so before `input_owner` has ever ticked and before anything has
    // warmed `players::Registry::Local()`. The stale-cross-world-pawn window that follows a
    // travel is GC-purge-timing dependent, i.e. a function of how much the session generated,
    // so dwelling here makes the run the same experiment as a played save.
    {
        const std::string dwell = coop::config::ReadEnv("VOTVCOOP_MENUTRAVEL_DWELL_S");
        const int dwellS = dwell.empty() ? 0 : atoi(dwell.c_str());
        if (dwellS > 0) {
            UE_LOGI("menutravel: DWELL %d s in gameplay before travel (warming the caches "
                    "a real play session would have warmed)", dwellS);
            ::Sleep(static_cast<DWORD>(dwellS) * 1000);
            UE_LOGI("menutravel: dwell complete -- travelling now");
        }
    }

    // VOTVCOOP_MENUTRAVEL_WAIT_SESSION=1: this peer is a CLIENT in a two-peer run. Wait until
    // the coop session is live (gameplay is only reachable through the save-transfer world, so
    // running()==true here means joined), then dwell so the join tail -- seeds, snapshot,
    // replays -- settles and does not confound what follows. The transition then exits to menu
    // with the layer LIVE, which opens the <=4 s purge-blind window the host-side wire census
    // (VOTVCOOP_WIRE_CENSUS=1) measures.
    if (coop::config::ReadEnv("VOTVCOOP_MENUTRAVEL_WAIT_SESSION") == "1") {
        bool sessionUp = false;
        for (int i = 0; i < 180; ++i) {
            if (harness::session_runtime::Session().running()) { sessionUp = true; break; }
            ::Sleep(1000);
        }
        if (!sessionUp) {
            UE_LOGW("menutravel: WAIT_SESSION set but no running session after 180 s -- abort");
            UE_LOGI("menutravel: DONE");
            return;
        }
        UE_LOGI("menutravel: session LIVE -- dwelling 25 s to settle the join tail");
        ::Sleep(25000);
        UE_LOGI("menutravel: WIRE-WINDOW transition NOW tick=%llu",
                static_cast<unsigned long long>(GetTickCount64()));
    }

    // The travel itself. Arm a held bypass and dispatch transition in ONE task: our detour hangs
    // the untitled_1 teardown, and the post-travel RSS climb is our own layer resuming at the
    // menu when the bypass expires, so a 300 s hold keeps RSS flat for the whole probe.
    // transition needs no pause, which is what makes it usable from a real death.
    //
    // VOTVCOOP_MENUTRAVEL_NO_BYPASS=1 travels with the layer LIVE instead -- the shape of a
    // player's own in-game exit to menu, and the only way to reach the exit-path IsLive fault,
    // which the default bypass hides by keeping the layer dormant through teardown.
    const bool noBypass = coop::config::ReadEnv("VOTVCOOP_MENUTRAVEL_NO_BYPASS") == "1";
    auto done = std::make_shared<std::atomic<int>>(0);
    auto ok = std::make_shared<int>(0);
    GT::Post([done, ok, noBypass] {
        if (!noBypass)
            ue_wrap::game_thread::SetTransparentBypass(300000);  // hold dormant (never expires in-probe)
        if (CallTransitionInline(L"/Game/menu")) *ok = 1;
        UE_LOGI("menutravel: %s + transition(/Game/menu) dispatched=%d",
                noBypass ? "LAYER LIVE (no bypass)" : "armed bypass(300s)", *ok);
        done->store(1);
    });
    WaitDone(done, 8000);

    // Pump is off now (bypass armed). Wait out VOTV's fade, teardown and menu load, then signal
    // the screenshot; worker-thread logging is GT-independent. VOTVCOOP_MENUTRAVEL_MENU_S widens
    // the window past the default 32 s, which is too short to tell "the pawn never went stale"
    // from "it was still stale when we stopped looking".
    {
        const std::string ms = coop::config::ReadEnv("VOTVCOOP_MENUTRAVEL_MENU_S");
        const int menuS = ms.empty() ? 32 : atoi(ms.c_str());
        ::Sleep(static_cast<DWORD>(menuS > 0 ? menuS : 32) * 1000);
    }
    UE_LOGI("menutravel: MENU-SHOT READY");  // mp.py captures the window here
    ::Sleep(5000);
    UE_LOGI("menutravel: DONE");
    // The menu is log-quiet, so everything since the last WARN sits in the CRT INFO buffer and a
    // kill would discard it. Flush so an external runner can read the full tail from disk.
    ue_wrap::log::Flush();
}

}  // namespace

void RunMenuTravelProbe() { RunProbe(); }
DWORD WINAPI MenuTravelProbeThread(LPVOID) { RunMenuTravelProbe(); return 0; }

}  // namespace harness::autotest

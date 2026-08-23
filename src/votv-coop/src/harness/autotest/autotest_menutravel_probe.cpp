// harness/autotest_menutravel_probe.cpp -- SP-solo "which command travels to the
// MAIN MENU?" probe.
//
// The client-death OOM fix must flee the leaking gameplay world to the menu (the
// balloon is VOTV's own possessed-player ragdoll leaking in-world ~165 MB/s -- a
// VOTV-native leak we cannot fix at source per Principle 1; the only mod-side cure
// is to leave the world fast). Three hands-on death tests failed because we never
// had a WORKING travel command: `disconnect` is a no-op (VOTV is single-player, no
// UE netdriver), and raw `open menu` does NOT travel -- the live death log showed
// the world stayed `untitled` through four re-issues, then froze. VOTV travels via
// its OWN verb: AmainGamemode_C::transition(FName LevelName) (mainGamemode.hpp:512),
// NOT a bare engine `open`.
//
// This probe settles in normal gameplay (NO death needed -- the travel command is
// independent of death) and tries the candidate travel commands SERIALLY: a command
// that fails leaves us in `untitled`, so the next can be tried, and the FIRST one
// that changes the live UWorld away from `untitled` is logged as the WINNER. It
// breaks the guess-and-ship-to-user cycle: we learn the correct menu-travel
// primitive autonomously before wiring it into the death path.
//
// Gated by env VOTVCOOP_RUN_MENUTRAVEL_PROBE=1; launch `mp.py menutravel` (solo).
// Throwaway diagnostic -- not a shipping path.

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

    // VOTVCOOP_MENUTRAVEL_DWELL_S (2026-08-23, the stale-cross-world-pawn measurement):
    // reaching gameplay is not the same as HAVING PLAYED. This probe historically fired
    // `transition` within ~1 s of the world coming up -- before the overlay's first
    // present, so before `input_owner` had ever ticked and before anything had warmed
    // `players::Registry::Local()`. The field flow it is standing in for (Linux triage
    // 2026-08-23) played a solo save for ~75 s first, and the 44-second stale window
    // that followed is GC-purge-timing dependent, i.e. a function of how much the
    // session actually generated. Dwell here so the run is the same experiment.
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

    // VOTVCOOP_MENUTRAVEL_WAIT_SESSION=1 (2026-08-22, the D2 wire-window probe):
    // this peer is a CLIENT in a two-peer run -- wait until the coop session is
    // live (join complete; gameplay is only reachable through the save-transfer
    // world, so running()==true here means joined), then DWELL so the join-tail
    // traffic (seeds, snapshot, replays) settles and the census window is not
    // confounded by it. The transition then exits to menu with the layer LIVE,
    // which opens the <=4 s purge-blind window the host-side wire census
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

    // The decisive experiment. Established: (1) our detour HANGS the untitled_1 teardown
    // -> a transparent bypass is required; (2) the menu IS reachable; (3) the post-flee
    // balloon is OUR layer resuming at the menu at bypass-EXPIRY -- HOLDING the bypass
    // (300 s) kept RSS dead-flat for 171 s (validated). Now test the DEAD-PLAYER-SAFE
    // reach: AmainGamemode_C::transition("/Game/menu") needs NO pause (a real death
    // ragdolls the player; the pause may be unavailable). Arm a held bypass FIRST, then
    // transition, in ONE task. Flat RSS for the whole probe == transition+held works for
    // a dead player -> the robust death-menu-return.
    // VOTVCOOP_MENUTRAVEL_NO_BYPASS=1 (2026-08-22, the IsLive/VEH repro): transition
    // with the layer LIVE -- the shape of a player's own in-game exit-to-menu, which
    // is where the exit-path IsLive fault fires (the bypass keeps the layer dormant
    // through teardown, so the default probe can never reproduce it).
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

    // Pump is off now (bypass armed). Just wait for VOTV's fade + teardown + menu load,
    // then signal the screenshot. Worker-thread logging is GT-independent.
    // VOTVCOOP_MENUTRAVEL_MENU_S widens the menu window past the default 32 s: the
    // stale-pawn window this probe now also measures ran 44 s in the field, and a
    // 32 s observation cannot distinguish "it never went stale" from "it was still
    // stale when we stopped looking".
    {
        const std::string ms = coop::config::ReadEnv("VOTVCOOP_MENUTRAVEL_MENU_S");
        const int menuS = ms.empty() ? 32 : atoi(ms.c_str());
        ::Sleep(static_cast<DWORD>(menuS > 0 ? menuS : 32) * 1000);
    }
    UE_LOGI("menutravel: MENU-SHOT READY");  // mp.py captures the window here
    ::Sleep(5000);
    UE_LOGI("menutravel: DONE");
    // The menu is log-quiet, so everything since the last WARN (incl. the re-injection
    // lines the D1 differential asserts on) sits in the CRT INFO buffer -- a kill would
    // discard it. Flush so an external runner can read the full tail from disk.
    ue_wrap::log::Flush();
}

}  // namespace

void RunMenuTravelProbe() { RunProbe(); }
DWORD WINAPI MenuTravelProbeThread(LPVOID) { RunMenuTravelProbe(); return 0; }

}  // namespace harness::autotest

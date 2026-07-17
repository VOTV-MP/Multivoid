// harness/autotest_pauseguard.cpp -- coop pause_guard e2e (2026-07-04).
//
// USER REPORT: a client pressing ESC pauses its world -- the engine stops ticking, its
// pose stream freezes on every other screen. The fix (coop/session/pause_guard) enforces
// the no-pause invariant while connected. This test drives the exact STATE the native
// ESC path engages: mainGamemode pauses via GameplayStatics::SetGamePaused (EX_CallMath,
// PE-invisible -- so we reproduce the state through our own engine wrapper of the SAME
// verb rather than replaying the input event; save_button_disable's autotest already
// established a reflective InpActEvt_Escape does not produce a real pause).
//
// CLIENT-ONLY flow: settle 75 s (connect + world + possession) -> baseline IsGamePaused
// (expect 0) -> SetGamePaused(true) -> sample IsGamePaused every 100 ms for 3 s. The
// guard runs in the TickGameplay chain, which keeps pumping while paused (the pause
// menu widget's own Tick dispatches ProcessEvent every Slate frame), so the pause must
// read FALSE within ~1 s. VERDICT PASS/FAIL logged; cross-check this client log for
// "pause_guard: world pause detected". Gated by env VOTVCOOP_RUN_PAUSE_TEST=1.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

#include <atomic>
#include <memory>

namespace harness::autotest {
namespace {

namespace E  = ue_wrap::engine;
namespace GT = ue_wrap::game_thread;

// Post `body` to the game thread and wait (the autotest_chippile pattern).
template <typename Fn>
int RunGT(Fn&& body) {
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([done, body]() mutable { body(*done); });
    while (done->load() == 0) ::Sleep(5);
    return done->load();
}

}  // namespace

void RunAutonomousPauseGuardTest() {
    if (coop::config::ReadEnv("VOTVCOOP_NET_ROLE") != "client") {
        UE_LOGI("pause_test: not client -- this routine is client-only (the reported freeze "
                "is the CLIENT pausing). Returning.");
        return;
    }
    UE_LOGI("pause_test: starting on client (waiting 75 s: connect + world + possession)");
    ::Sleep(75000);

    // Baseline + engage the pause through the game's own verb (one GT hop).
    const int engaged = RunGT([](std::atomic<int>& d) {
        const bool before = E::IsGamePaused();
        const bool ok = E::SetGamePaused(true);
        const bool after = E::IsGamePaused();
        UE_LOGI("pause_test: baseline paused=%d -> SetGamePaused(true) ok=%d -> paused=%d "
                "(the guard's next gameplay tick must clear it)",
                before ? 1 : 0, ok ? 1 : 0, after ? 1 : 0);
        d.store(after ? 1 : (ok ? 2 : -1));
    });
    if (engaged == -1) {
        UE_LOGW("pause_test: VERDICT ABORT -- SetGamePaused unresolved (no world/CDO?)");
        return;
    }
    if (engaged == 2) {
        // SetGamePaused succeeded but the world already read unpaused in the SAME task --
        // the guard cannot have run between the two calls (same GT slice), so the engine
        // itself refused the pause (no PlayerController?). Inconclusive rather than PASS.
        UE_LOGW("pause_test: VERDICT INCONCLUSIVE -- pause did not engage at all (engine "
                "refused, not the guard clearing it)");
        return;
    }

    // Sample: the guard must clear the pause within ~a tick; give it 3 s of 100 ms polls.
    int clearedAtMs = -1;
    for (int i = 1; i <= 30; ++i) {
        ::Sleep(100);
        const int paused = RunGT([](std::atomic<int>& d) {
            d.store(E::IsGamePaused() ? 1 : 2);
        });
        if (paused == 2) { clearedAtMs = i * 100; break; }
    }
    if (clearedAtMs > 0 && clearedAtMs <= 1000) {
        UE_LOGI("pause_test: VERDICT PASS -- world un-paused %d ms after the pause engaged "
                "(pause_guard cleared it; grep this log for 'pause_guard: world pause detected')",
                clearedAtMs);
    } else if (clearedAtMs > 0) {
        UE_LOGW("pause_test: VERDICT SLOW -- un-paused only after %d ms (guard alive but "
                "late; check pump liveness while paused)", clearedAtMs);
    } else {
        UE_LOGW("pause_test: VERDICT FAIL -- still paused 3 s after engaging (the guard did "
                "not clear it; the reported client freeze would reproduce)");
    }
}

DWORD WINAPI PauseGuardTestThread(LPVOID /*arg*/) {
    RunAutonomousPauseGuardTest();
    return 0;
}

}  // namespace harness::autotest

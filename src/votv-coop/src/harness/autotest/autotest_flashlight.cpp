// harness/autotest_flashlight.cpp -- the flashlight-toggle end-to-end test
// (VOTVCOOP_RUN_FLASHLIGHT_TEST): both peers toggle, and the ItemActivate wire path drives
// the other peer's puppet. Interface and doc in harness/autotest.h.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/dev/flashlight_setup.h"
#include "coop/player/item_activate.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
#include <memory>
#include <string>

namespace harness::autotest {
namespace {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace GT = ue_wrap::game_thread;
namespace cfg = coop::config;

}  // namespace

// --- The autonomous flashlight test ------------------------------------------
//
// Both peers run this routine: each toggles its own flashlight and the other
// peer's puppet should follow over the wire. Verification is by log diff.
//
// Expected on the SENDING peer:
//   flashlight_test: iteration N -- DebugForceToggle (local visual + wire)
//   flashlight: DebugForceToggle sent state=1/0 ...
// and on the RECEIVING peer:
//   flashlight: applied to puppet=... state=1/0
//
// Pre-requisites: mainPlayer_C exists (the autotest pose teleport ran), a
// flashlight is equipped, and the session is Connected -- the env gate sits
// after the same Start() call.
void RunAutonomousFlashlightTest() {
    const bool isHost = !IsClientRole();
    const char* roleStr = isHost ? "host" : "client";
    UE_LOGI("flashlight_test: starting autonomous routine on %s (waiting 15 s for stabilization)", roleStr);
    // 15 s is the same settle window grab_test uses; lets both peers
    // teleport to their autotest poses and the session reach Connected.
    ::Sleep(15000);

    // ---- Resolve mainPlayer.
    struct Resolved {
        void* player = nullptr;
        bool ok = false;
    };
    auto rsv = std::make_shared<Resolved>();
    auto done = std::make_shared<std::atomic<int>>(0);
    GT::Post([rsv, done] {
        rsv->player = R::FindObjectByClass(P::name::MainPlayerClass);
        if (!rsv->player) { UE_LOGW("flashlight_test: mainPlayer not found"); done->store(2); return; }
        rsv->ok = true;
        done->store(1);
    });
    while (done->load() == 0) ::Sleep(5);
    if (!rsv->ok) {
        UE_LOGW("flashlight_test: resolve failed -- aborting");
        return;
    }
    UE_LOGI("flashlight_test: resolved (mainPlayer=%p)", rsv->player);

    // ---- Setup. The local flashlight must be equipped and charged before any
    // toggle path can flip the light. The save should carry one, and
    // coop::dev::flashlight_setup::EnsureFlashlightReady() makes sure of it: it
    // reads hasFlashlight and calls addPropToPlayer when false, writes
    // saveSlot.battery = 100 (the scale is 0-100, not 0-1) and
    // saveSlot.flashlightBattery = prop_batts_C, and logs the state before and
    // after.
    {
        auto ensureDone = std::make_shared<std::atomic<int>>(0);
        GT::Post([rsv, ensureDone] {
            if (rsv->player) coop::dev::flashlight_setup::EnsureFlashlightReady(rsv->player);
            ensureDone->store(1);
        });
        while (ensureDone->load() == 0) ::Sleep(5);
        ::Sleep(500);  // let the BP equip path settle if addPropToPlayer ran
    }

    // ---- Toggle loop, 2 s apart. The Blueprint graph cannot be driven from here:
    // every reflected path -- updateFlashlight, 'Flashlight Update', the InpActEvt
    // entries -- either dispatches and no-ops or wants input state we cannot
    // synthesise, the graph being gated on the engine input system actually firing
    // an InputAction event. So the test bypasses it.
    // coop::item_activate::DebugForceToggle flips the player's flashlight bool and
    // drives the local light_R Intensity through SetIntensity -- what the Blueprint
    // would have done -- then builds and sends the ItemActivate packet itself,
    // rather than through the POST observer that path never reaches. The sender's
    // own light therefore toggles, a packet flies on every iteration since each is
    // a genuine state change, and the receiver applies the same intensity to the
    // puppet. The iteration count is ODD so both peers end ON, which is what an
    // end-of-run screenshot should show.
    const int kIterations = 5;
    for (int i = 0; i < kIterations; ++i) {
        UE_LOGI("flashlight_test: iteration %d -- DebugForceToggle (local visual + wire)", i);
        coop::item_activate::DebugForceToggle(rsv->player);
        ::Sleep(2000);
    }
    UE_LOGI("flashlight_test: DONE -- %d iterations on %s (DebugForceToggle path; "
            "final state should be ON for visual screenshot)", kIterations, roleStr);
}

DWORD WINAPI FlashlightTestThread(LPVOID /*arg*/) {
    RunAutonomousFlashlightTest();
    return 0;
}

}  // namespace harness::autotest

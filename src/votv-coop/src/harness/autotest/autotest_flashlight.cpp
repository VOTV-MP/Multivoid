// harness/autotest_flashlight.cpp -- the Phase 5F flashlight-toggle e2e test
// (VOTVCOOP_RUN_FLASHLIGHT_TEST): both peers toggle; the ItemActivate wire
// path drives the other peer's puppet. Extracted verbatim from
// harness/autotest.cpp (2026-07-19 dissolve); interface + doc in harness/autotest.h.

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

// --- Phase 5F flashlight autonomous test --------------------------------------
//
// Drives flashlight toggles by calling AmainPlayer_C::`Flashlight Update`
// directly via reflection (CallFunction -> ProcessEvent). That UFunction is
// part of our 4-observer install set; calling it via reflection trips the
// POST observer (the way F-press doesn't, because the input-event BP is
// inlined). Each call toggles `mp.flashlight` AND fires the POST observer,
// which sends the ItemActivate wire packet to the peer.
//
// Both peers run this routine; each toggles its own local flashlight + the
// OTHER peer's puppet should reflect it via the wire. Verification is by
// log diff (the LAN harness parses both logs).
//
// Expected log lines on the SENDING peer:
//   flashlight: 4 POST observer(s) installed (...)
//   flashlight_test: about to call 'Flashlight Update' (iteration N)
//   flashlight[POST Flashlight_Update] self=... flashlight=1/0 ...
//   flashlight: sent state=1/0 (peer=0 or 1)
//
// Expected log lines on the RECEIVING peer:
//   event_feed: <something about ItemActivate received>  (drain path)
//   flashlight: applied to puppet=... state=1/0
//
// Pre-reqs:
//   - mainPlayer_C exists (we're in gameplay; the autotest pose teleport ran)
//   - flashlight equipped (s_may2026 save has one; both peers load the same
//     save so both start with hasFlashlight=true)
//   - session connected (the harness flips state to Connected before this
//     test fires; the env gate is also after the same Start() call)
void RunAutonomousFlashlightTest() {
    const std::string roleEnv = cfg::ReadEnv("VOTVCOOP_NET_ROLE");
    const bool isHost = (roleEnv != "client");
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

    // ---- Toggle loop. 4 iterations with 2 s spacing.
    //
    // Prior approach (call 'Flashlight Update' via reflection) failed:
    // the BP graph runs but the gating BP-side state (timer / press
    // detector) is never satisfied from a reflection-only call, so the
    // flashlight bool stays at 0 -- dedup then blocks every send past
    // the first. Diagnosis: LAN run 2026-05-26.
    //
    // New approach: bypass the BP graph entirely. coop::item_activate::
    // DebugForceToggle directly flips mp.flashlight @0x0838 + invokes
    // our POST observer with the new state. Wire packet flies on every
    // iteration (each is a genuine state change). The local light_R
    // visual does NOT update because the BP did not fire -- but that's
    // OK for the autonomous wire test (we verify the puppet's light
    // toggles on the OTHER peer via log diff).
    // 2026-05-26: per inventory/equip/battery RE, the local player's
    // flashlight needs to be properly set up before BP toggle paths will
    // actually flip the light. The s_may2026 save SHOULD have one
    // equipped, but we top off the battery + verify the gate state just
    // in case. coop::dev::flashlight_setup::EnsureFlashlightReady():
    //   - reads hasFlashlight; if false, calls addPropToPlayer to give
    //     the player a flashlight (the cheat-menu-equivalent path)
    //   - writes saveSlot.battery = 1.0 (full)
    //   - writes saveSlot.flashlightBattery = prop_batts_C UClass*
    //   - logs the verified pre/post state
    {
        auto ensureDone = std::make_shared<std::atomic<int>>(0);
        GT::Post([rsv, ensureDone] {
            if (rsv->player) coop::dev::flashlight_setup::EnsureFlashlightReady(rsv->player);
            ensureDone->store(1);
        });
        while (ensureDone->load() == 0) ::Sleep(5);
        ::Sleep(500);  // let the BP equip path settle if addPropToPlayer ran
    }

    // 2026-05-26 #3: every BP path tried via reflection (updateFlashlight,
    // Flashlight Update, InpActEvt_13/14) either dispatched-but-no-op'd
    // or required input state we can't synthesise. The BP graph is
    // genuinely gated on the engine input system actually firing an
    // InputAction event -- reflection can't fake that.
    //
    // Pivot: DebugForceToggle now also drives the LOCAL light_R Intensity
    // via SetIntensity reflection, replicating exactly what the BP would
    // have done (flip bool + set Intensity). Visual toggles on the sender,
    // wire packet flies to the peer, receiver applies same intensity to
    // puppet -- end-to-end visual + wire from one entry point.
    // 5 iterations so final state is ON (each iter toggles, so odd
    // count starting from OFF ends ON). This lets the end screenshot
    // capture BOTH peers with their flashlights ON visually (user
    // feedback 2026-05-26: 4-iter pattern left both OFF + the staggered
    // peer-startup made the mid screenshot catch asymmetric states).
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

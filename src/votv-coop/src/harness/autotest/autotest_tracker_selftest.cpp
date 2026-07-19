// harness/autotest_tracker_selftest.cpp -- prop_element_tracker self-tests:
// the dead-Prop-Element reaper check (VOTVCOOP_RUN_PROPREAP_TEST) + the
// re-seed snapshot-completeness probe (VOTVCOOP_RUN_RESEED_TEST). Extracted
// verbatim from harness/autotest.cpp (2026-07-19 dissolve); interfaces + docs
// in harness/autotest.h.

#include "harness/autotest.h"

#include "coop/props/prop_element_tracker.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <memory>

namespace harness::autotest {
namespace {

namespace GT = ue_wrap::game_thread;

}  // namespace

// ---- dead-Prop-Element reaper self-test -----------------------------------
//
// Runs on both peers. After stabilization, calls
// prop_element_tracker::DebugCheckPropElementReap on the game thread: it
// installs a synthetic DEAD local Prop Element (sentinel actor, internalIdx -1)
// and verifies ReapDeadLocalPropElements evicts it + clears the actor-keyed maps
// + frees the eid -- the fix for the cave/level-transition mass-purge leak
// (~2000 props flagged PendingKill without firing K2_DestroyActor, dead shadows
// leaking until the 16384 caps exhaust). Confirms the reap mechanism at runtime
// without the hard-to-reproduce natural mass-purge. Gated by env
// VOTVCOOP_RUN_PROPREAP_TEST="1".
void RunAutonomousPropReapTest() {
    UE_LOGI("propreap_test: starting (waiting 15 s for world + prop seed up)");
    ::Sleep(15000);
    auto done = std::make_shared<std::atomic<int>>(0);  // 0=pending 1=pass -1=fail
    GT::Post([done] {
        const bool ok = coop::prop_element_tracker::DebugCheckPropElementReap();
        done->store(ok ? 1 : -1, std::memory_order_release);
    });
    for (int i = 0; i < 1000 && done->load(std::memory_order_acquire) == 0; ++i) ::Sleep(5);
    UE_LOGI("propreap_test: DONE (result=%d) -- grep 'propreap_test: forced-dead reap check' for PASS/FAIL",
            done->load(std::memory_order_acquire));
}

DWORD WINAPI PropReapTestThread(LPVOID /*arg*/) {
    RunAutonomousPropReapTest();
    return 0;
}

// ---- re-seed snapshot-completeness probe -----------------------------------
//
// Verify step (see autotest.h). After a 25 s settle -- past VOTV's boot-time
// `open untitled_1` level travel -- runs ReSeedKnownKeyedProps on the GT and
// logs how many NEW live keyed props it adds. A large number proves the boot
// seed missed the story map's placed props (incomplete late-joiner snapshot).
// Gated by env VOTVCOOP_RUN_RESEED_TEST="1".
void RunAutonomousReSeedTest() {
    UE_LOGI("reseed_test: starting (waiting 25 s for world settle past boot level-travel)");
    ::Sleep(25000);
    auto added = std::make_shared<std::atomic<long>>(-1);
    GT::Post([added] {
        const size_t n = coop::prop_element_tracker::ReSeedKnownKeyedProps();
        added->store(static_cast<long>(n), std::memory_order_release);
    });
    for (int i = 0; i < 2000 && added->load(std::memory_order_acquire) < 0; ++i) ::Sleep(5);
    UE_LOGI("reseed_test: DONE -- re-seed added %ld NEW props (grep 're-seed found' for the full line; >0 confirms the incomplete-snapshot bug)",
            added->load(std::memory_order_acquire));
}

DWORD WINAPI ReSeedTestThread(LPVOID /*arg*/) {
    RunAutonomousReSeedTest();
    return 0;
}

}  // namespace harness::autotest

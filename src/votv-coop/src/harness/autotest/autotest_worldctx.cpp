// harness/autotest_worldctx.cpp -- the bug2 world-context staleness guard
// self-test (VOTVCOOP_RUN_WORLDCTX_TEST): forces the cached world context
// stale and verifies engine::EnsureWorldContext recovers. Extracted verbatim
// from harness/autotest.cpp (2026-07-19 dissolve); interface + doc in
// harness/autotest.h.

#include "harness/autotest.h"

#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <memory>

namespace harness::autotest {
namespace {

namespace GT = ue_wrap::game_thread;

}  // namespace

// ---- bug2 world-context staleness guard self-test -------------------------
//
// Runs on both peers. After stabilization, calls engine::DebugCheckWorldContextRecovery on
// the game thread: it forces the cached world context to look stale (corrupts the cached
// GUObjectArray index) and verifies EnsureWorldContext DROPS + re-resolves it to a live
// context -- the bug2 fix (host failing to spawn the client puppet, BeginDeferredActor-
// SpawnFromClass null, 128 consecutive). Confirms the recovery mechanism at runtime without
// the intermittent natural repro. Gated by env VOTVCOOP_RUN_WORLDCTX_TEST="1".
void RunAutonomousWorldCtxTest() {
    UE_LOGI("worldctx_test: starting (waiting 15 s for world + GameInstance up)");
    ::Sleep(15000);
    auto done = std::make_shared<std::atomic<int>>(0);  // 0=pending 1=pass -1=fail
    GT::Post([done] {
        const bool ok = ue_wrap::engine::DebugCheckWorldContextRecovery();
        done->store(ok ? 1 : -1, std::memory_order_release);
    });
    for (int i = 0; i < 1000 && done->load(std::memory_order_acquire) == 0; ++i) ::Sleep(5);
    UE_LOGI("worldctx_test: DONE (result=%d) -- grep 'worldctx_test: forced-stale guard check' for PASS/FAIL",
            done->load(std::memory_order_acquire));
}

DWORD WINAPI WorldCtxTestThread(LPVOID /*arg*/) {
    RunAutonomousWorldCtxTest();
    return 0;
}

}  // namespace harness::autotest

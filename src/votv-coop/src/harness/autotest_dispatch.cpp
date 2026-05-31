// harness/autotest_dispatch.cpp -- see harness/autotest_dispatch.h.

#include "harness/autotest_dispatch.h"

#include "harness/autotest.h"
#include "harness/config.h"
#include "ue_wrap/log.h"

#include <windows.h>

namespace harness::autotest {
namespace {

namespace cfg = harness::config;

const char* RoleStr(coop::net::Role role) {
    return role == coop::net::Role::Host ? "host" : "client";
}

// Spawn `thread` (detached) iff env `envKey`=="1". Mirrors the old inline
// blocks one-for-one (same log wording), minus the copy-paste.
void SpawnIf(const char* envKey, const char* label,
             LPTHREAD_START_ROUTINE thread, coop::net::Role role) {
    if (cfg::ReadEnv(envKey) != "1") return;
    UE_LOGI("harness: %s=1 (%s) -- spawning %s thread", envKey, RoleStr(role), label);
    if (HANDLE h = ::CreateThread(nullptr, 0, thread, nullptr, 0, nullptr)) {
        ::CloseHandle(h);
    }
}

}  // namespace

void SpawnEnvGatedTests(coop::net::Role role) {
    // Autonomous grab test: both peers (host drives grab/move/release via native
    // PhysicsHandle UFunctions; client scan-only for cross-peer FName stability).
    SpawnIf("VOTVCOOP_RUN_GRAB_TEST", "grab test", &GrabTestThread, role);
    // Phase 5F flashlight: both peers toggle their own flashlight; the OTHER
    // peer's puppet should reflect it via the ItemActivate wire path.
    SpawnIf("VOTVCOOP_RUN_FLASHLIGHT_TEST", "flashlight test", &FlashlightTestThread, role);
    // Phase 5W weather: host-only; forces rain ON/OFF cycles, client applies via wire.
    SpawnIf("VOTVCOOP_RUN_WEATHER_TEST", "weather test", &WeatherTestThread, role);
    // Phase 5W Inc-fix-2 red sky: host-only; visually unambiguous variant.
    SpawnIf("VOTVCOOP_RUN_REDSKY_TEST", "red sky test", &RedSkyTestThread, role);
    // PR-FOUNDATION-2 (B) save-block: client-only; drives saveSlot.saveToSlot so
    // the SaveGameToSlot hook's BLOCK is observable in a short smoke.
    SpawnIf("VOTVCOOP_RUN_SAVEBLOCK_TEST", "save-block test", &SaveBlockTestThread, role);
    // PR-FOUNDATION-2 (B part 2) save-button grey-out: client-only; drives
    // InpActEvt_Escape so the pause-menu Save button disable is observable.
    SpawnIf("VOTVCOOP_RUN_SAVEBTN_TEST", "save-button test", &SaveBtnDisableTestThread, role);
    // bug2 world-context staleness guard self-test: both peers; forces a stale
    // world context and verifies EnsureWorldContext recovers.
    SpawnIf("VOTVCOOP_RUN_WORLDCTX_TEST", "world-context test", &WorldCtxTestThread, role);
    // Dead-Prop-Element reaper self-test: both peers; forces a synthetic dead
    // local Prop Element and verifies ReapDeadLocalPropElements evicts it.
    SpawnIf("VOTVCOOP_RUN_PROPREAP_TEST", "prop-reap test", &PropReapTestThread, role);
    // Re-seed snapshot-completeness probe: both peers; after settle, re-seeds and
    // logs how many NEW live keyed props the boot seed missed (verify step).
    SpawnIf("VOTVCOOP_RUN_RESEED_TEST", "re-seed probe", &ReSeedTestThread, role);
    // Vitals Inc2b ragdoll e2e wire test: BOTH peers. Client DRIVES its local
    // ragdollMode/forceGetUp; host OBSERVES its slot-1 puppet flip isRagdoll
    // 0->1->0 purely via the pose stream's kStateBitRagdoll + receiver reconcile.
    // (Supersedes the Inc2a #8 standalone probe -- the e2e path covers it.)
    SpawnIf("VOTVCOOP_RUN_RAGDOLL_TEST", "ragdoll e2e test", &RagdollTestThread, role);
    // Vitals Inc3 damage hurt-flash e2e: BOTH peers. Client lowers its own health;
    // host confirms its slot-1 puppet's nameplate flashes red via the streamed
    // health drop (no new wire).
    SpawnIf("VOTVCOOP_RUN_DAMAGE_TEST", "damage flash e2e test", &DamageTestThread, role);
}

}  // namespace harness::autotest

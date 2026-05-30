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
}

}  // namespace harness::autotest

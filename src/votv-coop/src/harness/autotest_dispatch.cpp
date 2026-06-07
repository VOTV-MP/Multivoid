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
    // Held-clump e2e (v26 mannequin model): HOST spawns + "grabs" a prop_garbageClump_C;
    // the held-edge broadcasts a PropSpawn + streams PropPose keyed by our EID; the
    // CLIENT spawns the visible clump mirror + drives it kinematically by eid, physics
    // on release. Validates the non-keyable clump on the prop pipeline + no crash.
    SpawnIf("VOTVCOOP_RUN_CLUMP_TEST", "held-clump mannequin e2e test", &ClumpTestThread, role);
    // Clump VISIBILITY probe: solo. Spawns a bare prop_garbageClump_C + logs its
    // StaticMesh asset (null vs named) -- gates the mannequin-model clump rework.
    SpawnIf("VOTVCOOP_RUN_CLUMPVIS_PROBE", "clump visibility probe", &ClumpVisProbeThread, role);
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
    // Vitals #6 puppet-damage HAZARD probe (gates Inc3-WIRE): host invokes Add Player
    // Damage on the slot-1 puppet + diffs its OWN saveSlot.health to confirm (at
    // runtime) that player health is the shared per-machine saveSlot. Client just
    // connects so the puppet exists.
    SpawnIf("VOTVCOOP_RUN_DMGHAZARD_TEST", "damage-hazard #6 probe", &DmgHazardTestThread, role);
    // Vitals Inc3-WIRE relay e2e: host sends synthetic PlayerDamage to slot 1; client
    // applies it to its own player; the streamed health drop flashes the host's slot-1
    // puppet -- proving the full reliable host->owner damage relay (no real enemy).
    SpawnIf("VOTVCOOP_RUN_PLAYERDMG_TEST", "PlayerDamage relay e2e", &PlayerDamageTestThread, role);
    // Xray-ragdoll feasibility probe: SINGLE instance (plain SP, role-agnostic).
    // Spawns playerRagdoll_C MANUALLY (no ragdollMode) + dumps it vs a real
    // ragdollMode body -- decides whether the manual spawn is visible/simulating
    // AND death-free (the whole xray-ragdoll direction hinges on it).
    SpawnIf("VOTVCOOP_RUN_RAGDOLL_SPAWN_PROBE", "xray-ragdoll spawn probe", &RagdollSpawnProbeThread, role);

    // Menu-travel command probe: solo, finds which command travels gameplay->menu
    // (for the client-death flee-to-menu fix). No connection needed.
    SpawnIf("VOTVCOOP_RUN_MENUTRAVEL_PROBE", "menu-travel probe", &MenuTravelProbeThread, role);

    // Fog ON/OFF model + clear-path probe: solo. Forces fog on, samples
    // finalFogDensity/thickFog/actors, then runs the RE'd clear sequence -- gates
    // the host-authoritative weather fix (client mist while host clear). No connection.
    SpawnIf("VOTVCOOP_RUN_FOG_PROBE", "fog probe", &FogProbeThread, role);

    // TEST-ONLY local-player movement oscillator: circles the local player so the OTHER
    // peer's interp has a MOVING source. Verification rig for the interp-starvation fix
    // (static-source smokes show trail~=0 and hide the bug). Enable on ONE peer; read the
    // other peer's `pose-diag[slot N] ... trail=`. Role-agnostic. Never ships.
    SpawnIf("VOTVCOOP_RUN_MOVE_OSC", "move oscillator (interp verify)", &MoveOscThread, role);
}

}  // namespace harness::autotest

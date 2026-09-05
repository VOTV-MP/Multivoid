// harness/autotest_dispatch.cpp -- see harness/autotest_dispatch.h. Each routine is described
// at its declaration in harness/autotest.h.

#include "harness/autotest_dispatch.h"

#include "harness/autotest.h"
#include "coop/session/join_seed.h"  // the inline seed selftest
#include "coop/config/config.h"
#include "coop/dev/director/director.h"
#include "ue_wrap/core/log.h"

#include <windows.h>

namespace harness::autotest {
namespace {

namespace cfg = coop::config;

const char* RoleStr(coop::net::Role role) {
    return role == coop::net::Role::Host ? "host" : "client";
}

// Spawn `thread` detached when env `envKey` is "1".
void SpawnIf(const char* envKey, const char* label,
             LPTHREAD_START_ROUTINE thread, coop::net::Role role) {
    if (cfg::ReadEnv(envKey) != "1") return;
    UE_LOGI("harness: %s=1 (%s) -- spawning %s thread", envKey, RoleStr(role), label);
    if (HANDLE h = ::CreateThread(nullptr, 0, thread, nullptr, 0, nullptr)) {
        ::CloseHandle(h);
    }
}

}  // namespace

bool IsClientRole() {
    // Latched, one resolve per process: the routines call this from their worker threads after
    // boot, and the role cannot change within a launch.
    static const bool isClient =
        cfg::ResolveEnum(coop::config_registry::rows::net_role) == "client";
    return isClient;
}

void SpawnEnvGatedTests(coop::net::Role role) {
    // The grab test: the host drives grab, move and release through the PhysicsHandle UFunctions;
    // the client only scans.
    SpawnIf("VOTVCOOP_RUN_GRAB_TEST", "grab test", &GrabTestThread, role);
    // The held-clump test: the host spawns and holds a trash clump; the client mirrors it by eid,
    // kinematic while held and physical on release.
    SpawnIf("VOTVCOOP_RUN_CLUMP_TEST", "held-clump mannequin e2e test", &ClumpTestThread, role);
    // The clump visibility probe, solo: spawns a bare clump and logs its StaticMesh asset.
    SpawnIf("VOTVCOOP_RUN_CLUMPVIS_PROBE", "clump visibility probe", &ClumpVisProbeThread, role);
    // The world-rules probe, both peers: runs the rules panel's read path and logs every rule.
    SpawnIf("VOTVCOOP_RUN_WORLDRULES_PROBE", "world-rules probe", &WorldRulesProbeThread, role);
    // The chipPile grab test: the host fires a real use press at a tracked pile and measures
    // whether the morphed clump lands in its hand and the client's mirror converts.
    SpawnIf("VOTVCOOP_RUN_CHIPPILE_TEST", "chipPile grab test", &ChipPileTestThread, role);
    // The puppet-grab probe: the host runs playerGrabbed on the slot-1 puppet and measures whether
    // the puppet holds the clump and its tick keeps it at the hand.
    SpawnIf("VOTVCOOP_RUN_PUPPET_GRAB_PROBE", "puppet-grab probe", &PuppetGrabProbeThread, role);
    // The synthetic GrabIntent test: the client sends a GrabIntent for a mirrored pile; the host
    // runs the grab on the puppet, broadcasts the convert and drives the held clump.
    SpawnIf("VOTVCOOP_RUN_GRAB_INTENT_TEST", "synthetic GrabIntent test", &GrabIntentTestThread, role);
    // The host-drift scenario: the host destroys and moves some of its own piles before connect,
    // so the client's join sweep sees real orphans.
    SpawnIf("VOTVCOOP_RUN_PILE_DRIFT", "host-drift pile scenario", &PileDriftScenarioThread, role);
    // The flashlight test, both peers: each toggles its own flashlight, and the other's puppet
    // must reflect it through the item-activate path.
    SpawnIf("VOTVCOOP_RUN_FLASHLIGHT_TEST", "flashlight test", &FlashlightTestThread, role);
    // The config-corpus selftest, solo: the real ini lexer over a corpus directory plus the
    // fault-injection controls.
    SpawnIf("VOTVCOOP_RUN_CONFIG_SELFTEST", "config-corpus selftest", &ConfigSelftestThread, role);
    // The join-seed delta-math selftest is pure and engine-free, so it runs inline; the smoke
    // driver greps its PASS/FAIL lines.
    if (cfg::ReadEnv("VOTVCOOP_RUN_SEED_SELFTEST") == "1") coop::join_seed::RunSelfTest();
    // The join-window email drill: the host authors at the solo and in-window instants; pair with
    // VOTVCOOP_SEED_DISABLE=1 for the red run.
    SpawnIf("VOTVCOOP_RUN_SEED_DRILL", "seed drill", &SeedDrillThread, role);
    SpawnIf("VOTVCOOP_RUN_SCANPARITY", "scan-hub parity drill", &ScanParityThread, role);
    // The weather test, host only: forces rain on and off; the client applies it over the wire.
    SpawnIf("VOTVCOOP_RUN_WEATHER_TEST", "weather test", &WeatherTestThread, role);
    // The red-sky test, host only: the visually unambiguous variant.
    SpawnIf("VOTVCOOP_RUN_REDSKY_TEST", "red sky test", &RedSkyTestThread, role);
    // The save-block test, client only: drives saveToSlot so the save hook's block is observable.
    SpawnIf("VOTVCOOP_RUN_SAVEBLOCK_TEST", "save-block test", &SaveBlockTestThread, role);
    // The save-button test, client only: drives the escape press so the pause menu's disabled
    // Save button is observable.
    SpawnIf("VOTVCOOP_RUN_SAVEBTN_TEST", "save-button test", &SaveBtnDisableTestThread, role);
    // The world-context test, both peers: forces a stale world context and verifies the recovery.
    SpawnIf("VOTVCOOP_RUN_WORLDCTX_TEST", "world-context test", &WorldCtxTestThread, role);
    // The prop-reap test, both peers: forces a synthetic dead local prop Element and verifies the
    // reaper evicts it.
    SpawnIf("VOTVCOOP_RUN_PROPREAP_TEST", "prop-reap test", &PropReapTestThread, role);
    // The re-seed probe, both peers: after settle, re-seeds and logs how many live keyed props the
    // boot seed missed.
    SpawnIf("VOTVCOOP_RUN_RESEED_TEST", "re-seed probe", &ReSeedTestThread, role);
    // The ragdoll test, both peers: the client drives its own ragdollMode; the host watches its
    // slot-1 puppet flip through the pose stream's ragdoll bit.
    SpawnIf("VOTVCOOP_RUN_RAGDOLL_TEST", "ragdoll e2e test", &RagdollTestThread, role);
    // The puppet-frame shot: the host frames the standing slot-1 puppet and holds it for the
    // nameplate screenshot.
    SpawnIf("VOTVCOOP_RUN_PUPPET_FRAME", "puppet-frame nameplate shot", &PuppetFrameThread, role);
    // The damage-flash test, both peers: the client lowers its own health, and the host's slot-1
    // nameplate must flash red from the streamed health.
    SpawnIf("VOTVCOOP_RUN_DAMAGE_TEST", "damage flash e2e test", &DamageTestThread, role);
    // The puppet-damage hazard probe: the host applies Add Player Damage to the slot-1 puppet and
    // diffs its own health, confirming player health is the shared per-machine save slot.
    SpawnIf("VOTVCOOP_RUN_DMGHAZARD_TEST", "damage-hazard #6 probe", &DmgHazardTestThread, role);
    // The PlayerDamage relay test: the host sends a synthetic PlayerDamage to slot 1, the client
    // applies it, and the streamed health drop flashes the host's puppet.
    SpawnIf("VOTVCOOP_RUN_PLAYERDMG_TEST", "PlayerDamage relay e2e", &PlayerDamageTestThread, role);
    // The native death chain, solo and sessionless: a lethal Add Player Damage, the whole native
    // death, its timeline and the dead-window memory differential.
    SpawnIf("VOTVCOOP_RUN_DEATH_TEST", "native death chain", &DeathTestThread, role);
    // The ragdoll spawn probe, single instance: spawns playerRagdoll_C by hand and dumps it against
    // a real ragdollMode body.
    SpawnIf("VOTVCOOP_RUN_RAGDOLL_SPAWN_PROBE", "xray-ragdoll spawn probe", &RagdollSpawnProbeThread, role);

    // The menu-travel probe, solo: finds which command travels gameplay to menu.
    SpawnIf("VOTVCOOP_RUN_MENUTRAVEL_PROBE", "menu-travel probe", &MenuTravelProbeThread, role);

    // The re-load churn probe, solo: gameplay to menu to re-load N times, censusing every live
    // world's PersistentLevel and WorldSettings chain; the negative control for a rejoin crash
    // with no coop layer present.
    SpawnIf("VOTVCOOP_RUN_RELOAD_CHURN", "re-load churn probe", &ReloadChurnProbeThread, role);

    // The zero-AV drill, solo: a fake decommitted-page object; the bare liveness check faults
    // exactly once and CachedObjRef::Alive answers with no AV.
    SpawnIf("VOTVCOOP_RUN_ISLIVE_DRILL", "islive zero-AV drill", &IsLiveDrillThread, role);

    // The fog probe, solo: forces fog on, samples the density and actors, then runs the clear
    // sequence.
    SpawnIf("VOTVCOOP_RUN_FOG_PROBE", "fog probe", &FogProbeThread, role);

    // The HUD red-tint discriminator, solo: collapses the damage indicator's arms one at a time,
    // with a screenshot marker per arm.
    SpawnIf("VOTVCOOP_RUN_HUD_TINT_PROBE", "HUD red-tint discriminator", &HudTintProbeThread, role);

    // The EventFire replay smoke: the host fires three events through HostFire, and the client log
    // proves the replay policy.
    SpawnIf("VOTVCOOP_RUN_EVENTFIRE_TEST", "EventFire replay smoke", &EventFireTestThread, role);

    // The event force-now smoke: the host resolves the obelisk box badge, forces it, and asserts
    // shots go from 1 to 0 through the native overlap dispatch.
    SpawnIf("VOTVCOOP_RUN_EVENTFORCE_TEST", "event force-NOW smoke", &EventForceTestThread, role);

    // The starRain cue driver: the host fires it before any client, so the orchestration can join
    // a client mid-shower and prove exactly one replay.
    SpawnIf("VOTVCOOP_RUN_CUEFORCE_TEST", "starRain cue-force driver", &CueForceTestThread, role);

    // The base radar alarm test: the host forces the trigger on, then off; the poll must broadcast
    // both edges and the client must log its replay applies.
    SpawnIf("VOTVCOOP_RUN_ALARMFORCE_TEST", "alarm lane e2e driver", &AlarmForceTestThread, role);

    // The piramid mirror-lane test: the host forces the piramid event, asserts the lane arms, then
    // baits a real gather by re-pinning the wisps around the pyramid; the verdict is the gather
    // relay firing.
    SpawnIf("VOTVCOOP_RUN_PIRAMIDFORCE_TEST", "piramid mirror-lane e2e", &PiramidForceTestThread, role);

    // The wisp mirror-lane test: the host forces the swarm, then midday, so the self-despawns
    // invisible to ProcessEvent exercise the dead-retire broadcast.
    SpawnIf("VOTVCOOP_RUN_WISPLANE_TEST", "wisp mirror-lane e2e", &WispLaneTestThread, role);

    // The killerwisp acquisition probe: read-only FSM sampling with the host teleported away, to
    // localise where the chain breaks.
    SpawnIf("VOTVCOOP_RUN_KWISP_PROBE", "killerwisp acquisition probe", &KwispProbeThread, role);

    // The pause-guard test: the client pauses its world through the game's own verb, and the
    // no-pause invariant must clear it within about a tick.
    SpawnIf("VOTVCOOP_RUN_PAUSE_TEST", "pause-guard e2e", &PauseGuardTestThread, role);

    // The movement oscillator: circles the local player so the other peer's interpolation has a
    // moving source; enable on one peer and read the other's pose diagnostic.
    SpawnIf("VOTVCOOP_RUN_MOVE_OSC", "move oscillator (interp verify)", &MoveOscThread, role);

    // The bot-director halt probe, solo: measures the path query over the baked NavMesh and a
    // reflected AddMovementInput moving the possessed body.
    SpawnIf("VOTVCOOP_RUN_NAV_PROBE", "nav HALT probe (director Phase-0)", &NavHaltProbeThread, role);

    // The director's walked-grab scenario, solo-runnable: the brain walks the possessed player to
    // an open chipPile over the NavMesh and grabs it, clearing a full hand first.
    SpawnIf("VOTVCOOP_RUN_DIRECTOR_WALKGRAB", "bot-director walked grab", &coop::director::WalkGrabDirectorThread, role);

    // The container-take probe, solo: walks to a placed non-empty container, drives the human take
    // chain, and measures whether the take executed.
    SpawnIf("VOTVCOOP_RUN_CTAKE_PROBE", "container-take input probe (director Phase-2)", &coop::director::ContainerTakeProbeThread, role);

    // The container race: two peers walk to the same container and take the same item at a
    // barrier; each counts locally afterwards, and the driver sums across peers.
    SpawnIf("VOTVCOOP_RUN_CTAKE_RACE", "container concurrent-take race (director Phase-2)", &coop::director::ContainerRaceThread, role);
}

}  // namespace harness::autotest

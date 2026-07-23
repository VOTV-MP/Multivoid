// harness/autotest_dispatch.cpp -- see harness/autotest_dispatch.h.

#include "harness/autotest_dispatch.h"

#include "harness/autotest.h"
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
    // World-rules probe: BOTH peers. Runs the F1>World>Rules read path + logs every
    // rule -- exercises the UI-only panel code AND measures G1 (diff host vs client).
    SpawnIf("VOTVCOOP_RUN_WORLDRULES_PROBE", "world-rules probe", &WorldRulesProbeThread, role);
    // chipPile GRAB test (v81 morph verify): HOST teleports to a tracked chipPile, aims +
    // confirms lookAtActor==pile via the game's own trace, fires InpActEvt_use (the real
    // E-press edge), then measures whether the morphed clump lands in holding_actor and the
    // peer's mirror converts. CLIENT scan-only. Closes the one link an audit/smoke can't.
    SpawnIf("VOTVCOOP_RUN_CHIPPILE_TEST", "chipPile grab test", &ChipPileTestThread, role);
    // Puppet-grab probe (docs/piles/08 Increment-2 gate): HOST executes playerGrabbed on the slot-1
    // PUPPET + measures whether the puppet HOLDS the clump and the per-tick PHC maintenance tracks it
    // to the puppet's hand (tick alive) vs floats at the spawn spot. Settles the one [?] the bytecode
    // could not -- whether an unpossessed puppet's ReceiveTick dispatches. Client just stands.
    SpawnIf("VOTVCOOP_RUN_PUPPET_GRAB_PROBE", "puppet-grab probe", &PuppetGrabProbeThread, role);
    // Synthetic GrabIntent test (docs/piles/08 Increment-2 host-side): the CLIENT sends a GrabIntent for a
    // mirrored pile eid; the HOST executes playerGrabbed on the puppet + broadcasts the convert + drives the
    // held clump. Exercises the full client->host wire/router/handler/hand-drive (no client suppress/collision).
    SpawnIf("VOTVCOOP_RUN_GRAB_INTENT_TEST", "synthetic GrabIntent test", &GrabIntentTestThread, role);
    // HOST-DRIFT scenario (L1 orphan census driver, docs/piles/08): the HOST destroys + moves some of its
    // own native chipPiles in the pre-connect window so its join snapshot diverges from the save the client
    // loaded -> the client's join-sweep [PILE-CENSUS] populates with real orphans. Read-only census this build.
    SpawnIf("VOTVCOOP_RUN_PILE_DRIFT", "host-drift pile scenario", &PileDriftScenarioThread, role);
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
    // Puppet-frame nameplate shot (PROPER, NO ragdoll): host frames the STANDING slot-1
    // puppet (positions back + aims at its head) + holds it for mp.py puppetshot to grab
    // the ImGui "Client" nameplate over it. Client just stands.
    SpawnIf("VOTVCOOP_RUN_PUPPET_FRAME", "puppet-frame nameplate shot", &PuppetFrameThread, role);
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

    // v95 EventFire replay-channel smoke: host fires solar/arirGraff_0/enasus through
    // event_fire_sync::HostFire; the client log proves the replay policy + suppression.
    SpawnIf("VOTVCOOP_RUN_EVENTFIRE_TEST", "EventFire replay smoke", &EventFireTestThread, role);

    // Event force-NOW smoke: host resolves the obelisk box badge, ForceNow()s it, and
    // asserts shots 1 -> 0 through the native overlap dispatch (coop/dev/event_force).
    SpawnIf("VOTVCOOP_RUN_EVENTFORCE_TEST", "event force-NOW smoke", &EventForceTestThread, role);

    // starRain cue-force driver: host fires 'starRain' PRE-client so the orchestration can
    // join a client mid-shower and prove the event_cue join re-send (exactly one replay).
    SpawnIf("VOTVCOOP_RUN_CUEFORCE_TEST", "starRain cue-force driver", &CueForceTestThread, role);

    // Base radar alarm lane e2e (v101): host DevForces runTrigger(1) -> wait -> (0); the
    // alarm_sync poll must broadcast both edges and the client must log its own native
    // replay applies (docs/events/alarm.md section 5).
    SpawnIf("VOTVCOOP_RUN_ALARMFORCE_TEST", "alarm lane e2e driver", &AlarmForceTestThread, role);

    // Piramid mirror-lane e2e: host ForceNow()s the piramid event (native spawner chain -> WA
    // mirror + npc wisps), asserts the v97 lane arms, then baits a REAL gather by re-pinning
    // the wisps around the walking pyramid; VERDICT = the PyramidGather relay firing.
    SpawnIf("VOTVCOOP_RUN_PIRAMIDFORCE_TEST", "piramid mirror-lane e2e", &PiramidForceTestThread, role);

    // Wisp mirror-lane e2e: host ForceNow()s the wisps swarm (EX_CallMath spawns -> Func-thunk
    // enroll -> client mirrors), then forces midday so the PE-invisible self-despawns exercise
    // the pose-walk dead-retire broadcast. Assert via log diff.
    SpawnIf("VOTVCOOP_RUN_WISPLANE_TEST", "wisp mirror-lane e2e", &WispLaneTestThread, role);

    // Killerwisp-vs-peers probe: read-only FSM sampling around SpawnKillerWispOnClient with the
    // host teleported away -- localizes where the "ignores peers" chain breaks.
    SpawnIf("VOTVCOOP_RUN_KWISP_PROBE", "killerwisp acquisition probe", &KwispProbeThread, role);

    // Pause-guard e2e: the CLIENT pauses its world via the game's own SetGamePaused verb (the
    // ESC state); the coop no-pause invariant must clear it within ~a tick. VERDICT in the log.
    SpawnIf("VOTVCOOP_RUN_PAUSE_TEST", "pause-guard e2e", &PauseGuardTestThread, role);

    // TEST-ONLY local-player movement oscillator: circles the local player so the OTHER
    // peer's interp has a MOVING source. Verification rig for the interp-starvation fix
    // (static-source smokes show trail~=0 and hide the bug). Enable on ONE peer; read the
    // other peer's `pose-diag[slot N] ... trail=`. Role-agnostic. Never ships.
    SpawnIf("VOTVCOOP_RUN_MOVE_OSC", "move oscillator (interp verify)", &MoveOscThread, role);

    // Phase-0 HALT probe for the autonomous bot-director (2026-07-23): SOLO, role-agnostic. Measures
    // Gate A (FindPathToLocationSynchronously returns a traversable path over the baked NavMesh) + Gate B
    // (reflected AddMovementInput -- resolved on the Pawn declaring class -- moves the possessed body).
    // The verdict picks the director's fallback rung; nothing of the director is built until this runs.
    SpawnIf("VOTVCOOP_RUN_NAV_PROBE", "nav HALT probe (director Phase-0)", &NavHaltProbeThread, role);

    // The autonomous bot-director's walked-grab scenario (2026-07-23): the BRAIN (PlayerContext read-model
    // + priority-arbitrated processes ClearHand>Goto>Grab + tick loop) drives the possessed player to pick
    // an open chipPile over the NavMesh and grab it -- state-aware (clears a full hand first). SOLO-runnable.
    SpawnIf("VOTVCOOP_RUN_DIRECTOR_WALKGRAB", "bot-director walked grab", &coop::director::WalkGrabDirectorThread, role);

    // Director Phase-2 HALT gate (2026-07-23): the CONTAINER-TAKE input probe. SOLO. Walks to a placed
    // non-empty world container, drives the faithful human take chain (openContainer -> pressButton ->
    // em_take), and MEASURES whether the take executed (GObjStack item-count decrement). Its verdict
    // decides whether the container concurrent-take race is buildable on the reflected-verb model.
    SpawnIf("VOTVCOOP_RUN_CTAKE_PROBE", "container-take input probe (director Phase-2)", &coop::director::ContainerTakeProbeThread, role);
}

}  // namespace harness::autotest

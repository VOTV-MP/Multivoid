// harness/autotest.h -- Autonomous grab test (no user E-press required).
//
// Forced-grab routine: find the nearest Aprop_C derivative, teleport it
// to the player's hand, then drive grabHandle.GrabComponentAtLocation /
// SetTargetLocation / ReleaseComponent via reflection (these UFunctions
// are ProcessEvent-dispatched and observable -- so this routine exercises
// the FULL Stage-1 observer pipeline end-to-end without a real keypress).
//
// Gated by env VOTVCOOP_RUN_GRAB_TEST="1" + role=Host. Spawned from the
// netEnabled play loop on a dedicated thread; all engine work goes through
// GT::Post so we never touch UObject state off the game thread.

#pragma once

#include <windows.h>

namespace harness::autotest {

// Run the full autonomous grab test routine. Blocks the calling thread
// for ~20 seconds (waits + screenshot captures + multiple drive ticks).
// Designed to be called from a worker thread (see GrabTestThread).
void RunAutonomousGrabTest();

// Worker-thread wrapper: just calls RunAutonomousGrabTest then returns 0.
// Pass to ::CreateThread as the start routine.
DWORD WINAPI GrabTestThread(LPVOID arg);

// Autonomous clump-mirror e2e test (env VOTVCOOP_RUN_CLUMP_TEST=1). HOST spawns
// a prop_garbageClump_C, writes it to grabbing_actor so net_pump's held-prop
// send broadcasts it (trash_collect_sync v3), then sweeps it; CLIENT mirrors it
// kinematically via the wire. Verifies the non-Aprop_C kinematic path + no crash.
void RunAutonomousClumpTest();
DWORD WINAPI ClumpTestThread(LPVOID arg);

// Clump VISIBILITY probe (env VOTVCOOP_RUN_CLUMPVIS_PROBE=1). Solo. Spawns a bare
// prop_garbageClump_C in front of the player + logs whether its StaticMesh asset is
// null (empty) or named (visible) -- gates the mannequin-model rework. Launch via mp.py clumpvis.
void RunClumpVisProbe();
DWORD WINAPI ClumpVisProbeThread(LPVOID arg);

// Phase 5F: autonomous flashlight-toggle test. Calls
// AmainPlayer_C::`Flashlight Update` via reflection 4 times with 2 s
// spacing. The POST observer detour catches each call + sends the
// ItemActivate wire packet. Both peers run this; the OTHER peer's
// puppet should reflect the toggles via the receiver path
// (item_activate::ApplyToPuppet).
// Blocks the calling thread for ~25 seconds (15 s settle + 4 * 2 s + tail).
// Gated by env VOTVCOOP_RUN_FLASHLIGHT_TEST="1".
void RunAutonomousFlashlightTest();
DWORD WINAPI FlashlightTestThread(LPVOID arg);

// Phase 5W: autonomous weather sync test. Host-only. After stabilization,
// calls weather_sync::DebugForceRain to force rain on / off / on / off
// at 5-second intervals. The POST observer on setRainProperties catches
// each call and broadcasts a WeatherState packet. Verification:
//   - Host log: at least 2 `weather: host broadcast` lines.
//   - Client log: at least 2 `weather: applied flags 0x...` lines.
//   - Cross-peer state read (via a diagnostic log line every cycle on
//     BOTH peers): host `isRaining` and client `isRaining` should match
//     after each forced toggle.
// Blocks the calling thread for ~30 seconds.
// Gated by env VOTVCOOP_RUN_WEATHER_TEST="1" + role=Host.
void RunAutonomousWeatherTest();
DWORD WINAPI WeatherTestThread(LPVOID arg);

// Phase 5W Inc-fix-2 (2026-05-27): autonomous RED SKY sync test. Host-
// only sender. After stabilization, host calls
// weather_sync::DebugForceRedSky(true) to flip the entire scene's sky
// + ambient color curves to the red set. Host's POST observer on
// spawnRedSky catches + broadcasts; client invokes the same on its
// local gamemode. Verification: both peers' screenshots show red sky.
// Chosen over rain (user feedback 2026-05-27): rain particle rendering
// turned out to be mostly atmospheric mist, ambiguous in screenshots;
// red sky is unmistakable.
// Gated by env VOTVCOOP_RUN_REDSKY_TEST="1" + role=Host.
void RunAutonomousRedSkyTest();
DWORD WINAPI RedSkyTestThread(LPVOID arg);

// PR-FOUNDATION-2 (B): autonomous CLIENT save-block test. Client-only. After
// stabilization, resolves the live saveSlot_C world-save object and calls its
// saveToSlot UFunction via reflection -- driving UGameplayStatics::SaveGameToSlot
// (our client hook target) without needing an autosave/menu trigger. Proves the
// hook CANCELS the write (not just that it installed): the client log shows
// `saveblock_test: invoking ...` then `save_block: BLOCKED ...`. The host runs
// nothing and installs no hook (its save path is untouched).
// Gated by env VOTVCOOP_RUN_SAVEBLOCK_TEST="1".
void RunAutonomousSaveBlockTest();
DWORD WINAPI SaveBlockTestThread(LPVOID arg);

// PR-FOUNDATION-2 (B part 2): autonomous CLIENT Save-button grey-out test. Client-only.
// Drives mainPlayer_C::InpActEvt_Escape via reflection (a real ESC press is impossible
// autonomously) so coop/save_button_disable's POST observer fires and disables the live
// pause-menu button_Save; the module logs the GetIsEnabled read-back. Proves the disable
// took; the visual grey needs a human glance. Gated by env VOTVCOOP_RUN_SAVEBTN_TEST="1".
void RunAutonomousSaveBtnDisableTest();
DWORD WINAPI SaveBtnDisableTestThread(LPVOID arg);

// bug2 world-context staleness guard self-test (2026-05-30). Both peers. Forces the cached
// world context stale and verifies engine::EnsureWorldContext recovers (the fix for the host
// failing to spawn the client puppet). Gated by env VOTVCOOP_RUN_WORLDCTX_TEST="1".
void RunAutonomousWorldCtxTest();
DWORD WINAPI WorldCtxTestThread(LPVOID arg);

// Dead-Prop-Element reaper self-test (2026-05-30). Both peers. Constructs a
// synthetic DEAD local Prop Element and verifies prop_element_tracker::
// ReapDeadLocalPropElements evicts it (the fix for the mass-purge leak: a
// cave/level transition flags ~2000 props PendingKill without firing
// K2_DestroyActor, so dead Prop Element shadows leak until the 16384 caps
// exhaust). Gated by env VOTVCOOP_RUN_PROPREAP_TEST="1".
void RunAutonomousPropReapTest();
DWORD WINAPI PropReapTestThread(LPVOID arg);

// Re-seed snapshot-completeness PROBE (2026-05-30). Both peers. After a long
// settle (25 s -- well past VOTV's boot-time `open untitled_1` level travel),
// calls prop_element_tracker::ReSeedKnownKeyedProps on the game thread and logs
// how many NEW live keyed props it adds. A large add proves the bug (the
// one-shot boot seed ran on the pre-travel world, so the story map's placed
// props were never tracked -> incomplete late-joiner snapshot). Gated by env
// VOTVCOOP_RUN_RESEED_TEST="1". This is the verify step before wiring an
// automatic world-change re-seed trigger.
void RunAutonomousReSeedTest();
DWORD WINAPI ReSeedTestThread(LPVOID arg);

// Vitals Inc2b ragdoll/faint e2e WIRE test (2026-05-31, harness/autotest_vitals.cpp).
// BOTH peers, role-branched: the CLIENT drives ragdollMode(1,1,0)/forceGetUp() on
// its LOCAL possessed player; the HOST observes its slot-1 puppet flip isRagdoll
// 0->1->0 purely via the pose stream's kStateBitRagdoll + RemotePlayer's reconcile.
// Proves the full sender-bit + receiver-reconcile path end to end. Supersedes the
// Inc2a standalone #8 probe (host-drives-own-puppet -- PASSED, 4d52d40; the e2e
// path covers it, so it was retired per RULE 2). Gated by env
// VOTVCOOP_RUN_RAGDOLL_TEST="1" (host = observer, client = driver).
void RunAutonomousRagdollTest();
DWORD WINAPI RagdollTestThread(LPVOID arg);

// Vitals Inc3 damage hurt-flash e2e WIRE test (2026-05-31, harness/autotest_vitals.cpp).
// BOTH peers: the CLIENT lowers its own saveSlot.health in steps; vitals Inc1 streams
// the lower fraction; the HOST confirms its slot-1 puppet's nameplate flashes red
// (RemotePlayer::IsHurtFlashing) -- proving a peer's damage shows on its puppet with
// NO new wire (the flash rides the existing health stream). Gated by env
// VOTVCOOP_RUN_DAMAGE_TEST="1" (host = observer, client = driver).
void RunAutonomousDamageTest();
DWORD WINAPI DamageTestThread(LPVOID arg);

// Vitals #6 puppet-damage HAZARD probe (2026-05-31, harness/autotest_vitals.cpp).
// Gates Inc3-WIRE. HOST-only verdict: invokes AmainPlayer_C::"Add Player Damage" on
// the slot-1 UNPOSSESSED puppet and diffs the host's OWN saveSlot.health. A drop ==
// health is the shared per-machine saveSlot (static RE said so; this locks it at
// runtime) -> Inc3-WIRE must intercept native damage on puppets, not just relay it.
// A local-player control disambiguates a BP early-out from a non-landing call; host
// health is restored after. The client just connects so the puppet exists. Gated by
// env VOTVCOOP_RUN_DMGHAZARD_TEST="1".
void RunAutonomousDmgHazardTest();
DWORD WINAPI DmgHazardTestThread(LPVOID arg);

// Vitals Inc3-WIRE relay e2e (2026-05-31, harness/autotest_vitals.cpp). BOTH peers.
// The HOST sends synthetic PlayerDamage to slot 1 (player_damage::DebugForceHitPuppet);
// the CLIENT receives + applies Add Player Damage to its OWN player; the client's
// streamed health drop flashes the host's slot-1 puppet (reusing the Inc3 hurt-flash).
// Proves the FULL reliable host->owner damage relay with no real enemy. Gated by env
// VOTVCOOP_RUN_PLAYERDMG_TEST="1" (host = driver+observer, client = passive receiver).
void RunAutonomousPlayerDamageTest();
DWORD WINAPI PlayerDamageTestThread(LPVOID arg);

// Xray-ragdoll feasibility PROBE (2026-06-01, harness/autotest_ragdoll_spawn_probe.cpp).
// SINGLE instance, role-agnostic (plain single-player; NO connection). Answers, by
// observing live objects, whether VOTV's playerRagdoll_C can be spawned MANUALLY
// (deferred spawn + set Player @0x248, NO ragdollMode) into a VISIBLE, PHYSICALLY
// SIMULATING body WITHOUT triggering the player death/faint that ragdollMode's
// global path causes -- then dumps a REAL ragdollMode-spawned body as the target
// config to match. Decides the xray-ragdoll-actor implementation. Gated by env
// VOTVCOOP_RUN_RAGDOLL_SPAWN_PROBE="1"; launch via `mp.py ragdollspawn` (solo).
void RunRagdollSpawnProbe();
DWORD WINAPI RagdollSpawnProbeThread(LPVOID arg);

// Menu-travel command PROBE (2026-06-01, harness/autotest_menutravel_probe.cpp).
// SINGLE instance, role-agnostic (plain single-player; NO connection). The client-
// death OOM fix must flee the leaking gameplay world to the MENU, but three hands-on
// death tests failed for lack of a working travel command (`disconnect` is a no-op
// without a netdriver; raw `open menu` never travels). This probe settles in normal
// gameplay (no death needed) and tries the candidate travel commands SERIALLY --
// AmainGamemode_C::transition(FName) with "menu" / "/Game/menu", then `open
// /Game/menu`, then `open menu` -- logging the FIRST that moves the live UWorld off
// `untitled` as the WINNER. Verifies the menu-travel primitive autonomously before
// it is wired into the death path. Gated by env VOTVCOOP_RUN_MENUTRAVEL_PROBE=1;
// launch via `mp.py menutravel` (solo).
void RunMenuTravelProbe();
DWORD WINAPI MenuTravelProbeThread(LPVOID arg);

// Fog ON/OFF model + clear-path PROBE (2026-06-01, harness/autotest_fog_probe.cpp).
// SINGLE instance, role-agnostic (plain single-player; NO connection). Gates the
// host-authoritative weather fix (hands-on bug: client STRONG MIST while host
// CLEAR). Forces fog ON via the cycle's own spawnFog()/superFogEvent() verbs,
// samples finalFogDensity/thickFog/fogEventObject/superFog_C-count for ~12 s (=
// the density-vs-target model that decides the wire design), then runs the RE'd
// CLEAR sequence (destroy the rolling-fog + super-fog actors + zero density +
// SetFogDensity()) and confirms the density stays ~0 with no fog actors. Proves
// the clear-path mechanics + answers the thickFog-target question autonomously
// before any protocol change. Gated by env VOTVCOOP_RUN_FOG_PROBE=1; launch via
// `mp.py fogprobe` (solo). Emits FOG-FORCED READY / FOG-CLEARED READY markers.
void RunFogProbe();
DWORD WINAPI FogProbeThread(LPVOID arg);

// TEST-ONLY local-player movement oscillator (2026-06-06, harness/autotest_move_osc.cpp).
// Role-agnostic: circles the LOCAL player around a small horizontal circle so the OTHER
// peer's RemotePlayer interpolation has a MOVING source to track. The verification rig for
// the interp-starvation fix -- with a static source the receiver's pose-diag `trail=` is
// trivially ~0 cm (why the bug hid in every prior smoke). Enable on ONE peer, read the
// OTHER peer's `pose-diag[slot N] ... trail=`: bounded ~ speed*window (~18 cm) == FIXED,
// hundreds of cm == still starved. Gated by env VOTVCOOP_RUN_MOVE_OSC="1". Never ships.
void RunAutonomousMoveOsc();
DWORD WINAPI MoveOscThread(LPVOID arg);

}  // namespace harness::autotest

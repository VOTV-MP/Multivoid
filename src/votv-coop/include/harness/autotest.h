// harness/autotest.h -- the autotest island's interface: one Run* and *Thread pair per autonomous
// routine, each in its own harness/autotest_*.cpp (dispatch table: autotest_dispatch.cpp). A
// routine is gated by its VOTVCOOP_RUN_* env var, spawned from the play loop on its own thread,
// and does all engine work through GT::Post, never touching UObject state off the game thread.

#pragma once

#include <windows.h>

namespace harness::autotest {

// The one role predicate for every routine's host/client branch: the registry's resolve of
// net.role (env twin, ini, unset) through coop::config::ResolveEnum, the product's own resolver.
// "client" is the client; anything else is the host side. Latched: one resolve per process,
// since routines poll from worker threads.
bool IsClientRole();

// The forced-grab routine (harness/autotest_grab.cpp): the nearest Aprop_C derivative is
// teleported to the player's hand and driven through grabHandle's GrabComponentAtLocation,
// SetTargetLocation and ReleaseComponent by reflection, the same ProcessEvent-dispatched
// UFunctions a real keypress reaches, so the whole observer pipeline runs end to end. Blocks
// about 20 s. Env VOTVCOOP_RUN_GRAB_TEST=1, host.
void RunAutonomousGrabTest();

// The thread start routine for ::CreateThread.
DWORD WINAPI GrabTestThread(LPVOID arg);

// The clump-mirror test (harness/autotest_clump.cpp): the host spawns a prop_garbageClump_C,
// writes it to grabbing_actor so the held-prop send broadcasts it, then sweeps it; the client
// mirrors it kinematically. The non-Aprop_C kinematic path, and no crash. Env
// VOTVCOOP_RUN_CLUMP_TEST=1.
void RunAutonomousClumpTest();
DWORD WINAPI ClumpTestThread(LPVOID arg);

// The chipPile grab test (harness/autotest_chippile.cpp), host-driven: does a real E-press grab
// of a tracked chipPile put the morphed clump into mainPlayer.holding_actor? The host player is
// teleported to a pile, the camera aimed and the game's own interaction trace polled until
// lookAtActor is the pile, then InpActEvt_use is fired through the same ProcessEvent edge a real
// press hits; holding_actor is measured and a throw tests the re-pile. The client only observes
// OnConvert. It produces the log to read, never a pass by itself. Env
// VOTVCOOP_RUN_CHIPPILE_TEST=1.
void RunAutonomousChipPileTest();
DWORD WINAPI ChipPileTestThread(LPVOID arg);

// The puppet-grab probe, host only: when the host runs actorChipPile_C::playerGrabbed with a
// puppet (an unpossessed mainPlayer_C) as the player, does the puppet hold the spawned clump,
// and does the per-tick hand maintenance run on it (the clump tracks the hand) or not (it floats
// at the spawn spot and the drive must target it). Read-only beyond the one pile the grab
// consumes. Env VOTVCOOP_RUN_PUPPET_GRAB_PROBE=1.
void RunPuppetGrabProbe();
DWORD WINAPI PuppetGrabProbeThread(LPVOID arg);

// The synthetic GrabIntent test: the client picks a mirrored pile proxy, resolves its eid and
// sends a GrabIntent; the host validates it, runs playerGrabbed on the puppet, broadcasts the
// convert and drives the puppet-held clump. The verdict is the host's [GRAB-INTENT] and
// [PUPPET-DRIVE] log lines. Env VOTVCOOP_RUN_GRAB_INTENT_TEST=1.
void RunGrabIntentTest();
DWORD WINAPI GrabIntentTestThread(LPVOID arg);

// The host-drift scenario, host only: in the pre-connect solo window (mp.py smoke --host-settle)
// the host destroys N native chipPiles and moves M, so its join snapshot diverges from the save
// both peers loaded; the client spawns proxies at the drifted poses and its unmatched natives
// survive as orphans, which the join sweep's [PILE-CENSUS] reports. The only autonomous way to
// populate the orphan histogram (a clean same-save join has none). Env VOTVCOOP_RUN_PILE_DRIFT=1.
void RunPileDriftScenario();
DWORD WINAPI PileDriftScenarioThread(LPVOID arg);

// The clump visibility probe (harness/autotest_clump.cpp), solo: spawns a bare
// prop_garbageClump_C in front of the player and logs whether its StaticMesh asset is null or
// named. Env VOTVCOOP_RUN_CLUMPVIS_PROBE=1; mp.py clumpvis.
void RunClumpVisProbe();
DWORD WINAPI ClumpVisProbeThread(LPVOID arg);

// The world-rules probe (harness/autotest_worldrules.cpp), both peers: runs the F1 > World >
// Rules read path (ue_wrap::game_rules::ReadLocal) and logs every rule, so a diff of the two
// peers' `worldrules:` lines shows whether the host's rules reached the client's game instance.
// Env VOTVCOOP_RUN_WORLDRULES_PROBE=1.
void RunWorldRulesProbe();
DWORD WINAPI WorldRulesProbeThread(LPVOID arg);

// The flashlight-toggle test (harness/autotest_flashlight.cpp): the local flashlight is
// toggled five times, 2 s apart, through item_activate::DebugForceToggle -- the Blueprint
// path is gated on real engine input, which reflection cannot synthesise -- and each
// toggle sends ItemActivate, so the other peer's puppet reflects it
// (item_activate::ApplyToPuppet). Both peers; blocks about 25 s. Env
// VOTVCOOP_RUN_FLASHLIGHT_TEST=1.
void RunAutonomousFlashlightTest();
DWORD WINAPI FlashlightTestThread(LPVOID arg);

// The config-corpus selftest (harness/autotest_config.cpp), solo and role-agnostic, no session:
// the real ini lexer over the VOTVCOOP_CONFIG_CORPUS_DIR corpus (both value layers, one verdict
// line per file and key) plus the tri-state controls (a missing file is Absent, an injected
// mid-stream failure Unreadable). Env VOTVCOOP_RUN_CONFIG_SELFTEST=1.
void RunConfigSelftest();
DWORD WINAPI ConfigSelftestThread(LPVOID arg);

// The weather sync test (harness/autotest_weather.cpp), host: after the settle, DebugForceRain
// forces rain on, off, on, off at 5 s intervals; the observer on setRainProperties broadcasts
// each. Pass: two or more host broadcast lines, two or more client applied lines, and both
// peers' isRaining diagnostic matching after each toggle. Blocks about 30 s. Env
// VOTVCOOP_RUN_WEATHER_TEST=1.
void RunAutonomousWeatherTest();
DWORD WINAPI WeatherTestThread(LPVOID arg);
DWORD WINAPI SeedDrillThread(LPVOID arg);  // the seeds lane's red/green drill (host only)
DWORD WINAPI ScanParityThread(LPVOID arg);  // the scan hub's N-match parity drill (either role)

// The red-sky sync test (harness/autotest_weather.cpp), host sender: weather_redsky::DebugForce
// flips the scene's sky and ambient curves to the red set; the client applies the same on its
// gamemode, and both peers' screenshots show a red sky (rain reads as ambiguous mist in a
// screenshot; red sky is unmistakable). Env VOTVCOOP_RUN_REDSKY_TEST=1.
void RunAutonomousRedSkyTest();
DWORD WINAPI RedSkyTestThread(LPVOID arg);

// The client save-block test, client only: after the settle the live saveSlot_C is resolved and
// its saveToSlot called by reflection, driving UGameplayStatics::SaveGameToSlot (the hook
// target) without an autosave; the client log shows the invoke line and then the block line, so
// the hook cancels the write rather than merely installing. The host runs nothing. Env
// VOTVCOOP_RUN_SAVEBLOCK_TEST=1.
void RunAutonomousSaveBlockTest();
DWORD WINAPI SaveBlockTestThread(LPVOID arg);

// The client Save-button grey-out test, client only: mainPlayer_C::InpActEvt_Escape is driven
// by reflection (a real ESC is impossible autonomously) so save_button_disable's observer
// disables the live pause-menu button_Save and logs the GetIsEnabled read-back; the visual grey
// needs a human glance. Env VOTVCOOP_RUN_SAVEBTN_TEST=1.
void RunAutonomousSaveBtnDisableTest();
DWORD WINAPI SaveBtnDisableTestThread(LPVOID arg);

// The world-context staleness self-test (harness/autotest_worldctx.cpp), both peers: forces the
// cached world context stale and checks that engine::EnsureWorldContext recovers (the host once
// failed to spawn the client puppet on a stale context). Env VOTVCOOP_RUN_WORLDCTX_TEST=1.
void RunAutonomousWorldCtxTest();
DWORD WINAPI WorldCtxTestThread(LPVOID arg);

// The dead-element reaper self-test (harness/autotest_tracker_selftest.cpp), both peers: a
// synthetic dead local Prop element must be evicted by ReapDeadLocalPropElements (a level
// transition flags ~2,000 props PendingKill without K2_DestroyActor, and their shadows leaked
// until the caps ran out). Env VOTVCOOP_RUN_PROPREAP_TEST=1.
void RunAutonomousPropReapTest();
DWORD WINAPI PropReapTestThread(LPVOID arg);

// The re-seed completeness probe (harness/autotest_tracker_selftest.cpp), both peers: after a
// 25 s settle (past the boot-time level travel) ReSeedKnownKeyedProps runs on the game thread
// and logs how many new live keyed props it adds; a large count means the boot seed ran on the
// pre-travel world and the story map's placed props were untracked. Env
// VOTVCOOP_RUN_RESEED_TEST=1.
void RunAutonomousReSeedTest();
DWORD WINAPI ReSeedTestThread(LPVOID arg);

// The ragdoll wire test (harness/autotest_ragdoll.cpp), both peers: the client drives
// ragdollMode(1,1,0) and forceGetUp on its own player; the host watches its slot-1 puppet's
// isRagdoll go 0, 1, 0 through the pose stream's ragdoll bit and RemotePlayer's reconcile. Env
// VOTVCOOP_RUN_RAGDOLL_TEST=1 (host observes, client drives).
void RunAutonomousRagdollTest();
DWORD WINAPI RagdollTestThread(LPVOID arg);

// The puppet-frame shot (harness/autotest_puppetframe.cpp): the host stands about 420 units back
// from the standing slot-1 puppet, aims at its head and holds the frame so mp.py puppetshot can
// capture the nameplate; the client stands. Env VOTVCOOP_RUN_PUPPET_FRAME=1.
void RunAutonomousPuppetFrame();
DWORD WINAPI PuppetFrameThread(LPVOID arg);

// The hurt-flash wire test (harness/autotest_damage.cpp), both peers: the client lowers its own
// saveSlot.health in steps, the vitals stream carries the fraction, and the host confirms its
// slot-1 puppet's nameplate flashes red (RemotePlayer::IsHurtFlashing), with no new wire. Env
// VOTVCOOP_RUN_DAMAGE_TEST=1 (host observes, client drives).
void RunAutonomousDamageTest();
DWORD WINAPI DamageTestThread(LPVOID arg);

// The puppet-damage hazard probe (harness/autotest_dmghazard.cpp), host verdict: Add Player
// Damage is invoked on the unpossessed slot-1 puppet and the host's own saveSlot.health diffed;
// a drop means the saveSlot is shared per machine, so native damage on a puppet must be
// intercepted, not merely relayed. A local-player control separates a BP early-out from a
// non-landing call; health is restored after. The client only connects so the puppet exists.
// Env VOTVCOOP_RUN_DMGHAZARD_TEST=1.
void RunAutonomousDmgHazardTest();
DWORD WINAPI DmgHazardTestThread(LPVOID arg);

// The damage relay test (harness/autotest_playerdmg.cpp), both peers: the host sends a synthetic
// PlayerDamage to slot 1 (player_damage::DebugForceHitPuppet), the client applies Add Player
// Damage to its own player, and the client's streamed health drop flashes the host's slot-1
// puppet. The full host-to-owner relay with no enemy. Env VOTVCOOP_RUN_PLAYERDMG_TEST=1 (host
// drives and observes).
void RunAutonomousPlayerDamageTest();
DWORD WINAPI PlayerDamageTestThread(LPVOID arg);

// The native death-chain instrument (harness/autotest_death.cpp), solo and sessionless (no net
// role, so the local-death handling never fires and VOTV's own death plays out): a lethal Add
// Player Damage, then an observation (the measured dead, ragdoll, blackScreen and travel timeline
// against the bytecode prediction, and the dead window's RSS slope against an equally long alive
// control) and an acceptance verdict against the death contract (the travel cancelled, the player
// revived). Env VOTVCOOP_RUN_DEATH_TEST=1; ends with a "death_test: DONE" line.
DWORD WINAPI DeathTestThread(LPVOID arg);

// The ragdoll spawn probe (harness/autotest_ragdoll_spawn_probe.cpp), single instance, no
// connection: can playerRagdoll_C be spawned by hand (a deferred spawn with Player set, no
// ragdollMode) into a visible, simulating body without the death or faint that ragdollMode's
// global path causes; then a real ragdollMode-spawned body is dumped as the configuration to
// match. Env VOTVCOOP_RUN_RAGDOLL_SPAWN_PROBE=1; mp.py ragdollspawn.
void RunRagdollSpawnProbe();
DWORD WINAPI RagdollSpawnProbeThread(LPVOID arg);

// The menu-travel probe (harness/autotest_menutravel_probe.cpp), single instance, no connection:
// settles in gameplay and tries the candidate travel commands in series
// (AmainGamemode_C::transition with "menu" and "/Game/menu", then `open /Game/menu`, then `open
// menu`), logging the first that moves the live UWorld off untitled. `disconnect` is a no-op
// without a netdriver and a raw `open menu` never travels. Env VOTVCOOP_RUN_MENUTRAVEL_PROBE=1;
// mp.py menutravel.
void RunMenuTravelProbe();
DWORD WINAPI MenuTravelProbeThread(LPVOID arg);

// The re-load churn probe (harness/autotest_reloadchurn.cpp), a negative control: solo and
// sessionless, it drives gameplay to menu to re-load N times and censuses every live UWorld's
// PersistentLevel, WorldSettings and DefaultGameMode chain at each step. The decisive frame is
// at the menu: a resident Untitled_1 world with a null WorldSettings is what
// UGameInstance::CreateGameModeForURL dereferences on the next load, so the failing link gets
// named rather than inferred from a fault address. Env VOTVCOOP_RUN_RELOAD_CHURN=1; mp.py
// reloadchurn.
void RunReloadChurnProbe();
DWORD WINAPI ReloadChurnProbeThread(LPVOID arg);

// The zero-AV drill for the CachedObjRef discipline (harness/autotest_islive_drill.cpp): a fake
// object on a decommitted page; a bare IsLive must fault exactly once (absorbed by its SEH) and
// CachedObjRef::Alive must answer false with no AV. A scoped diagnostic VEH covers the drill
// body only. Env VOTVCOOP_RUN_ISLIVE_DRILL=1; the dev smoke lane only.
DWORD WINAPI IsLiveDrillThread(LPVOID arg);

// The fog probe (harness/autotest_fog_probe.cpp), single instance, no connection: forces fog on
// through the cycle's own spawnFog and superFogEvent, samples finalFogDensity, thickFog,
// fogEventObject and the superFog_C count for about 12 s (the density-versus-target model), then
// runs the clear sequence (destroy the rolling-fog and super-fog actors, zero the density,
// SetFogDensity) and confirms the density stays near 0 with no fog actors. Emits FOG-FORCED
// READY and FOG-CLEARED READY markers. Env VOTVCOOP_RUN_FOG_PROBE=1; mp.py fogprobe.
void RunFogProbe();
DWORD WINAPI FogProbeThread(LPVOID arg);

// The HUD red-tint discriminator (harness/autotest_hud_tint.cpp), solo: ui_damageIndicator_C
// (a child of ui_UI_C, invisible to a viewport-widget census) holds two candidates for a red
// wash, dmg_tunnel, which has no Visibility property and draws mat_tunnel full-screen every
// frame, and dmg_full, the death branch's one-way latch; a cooked material's transparency at
// alpha 0 cannot be read statically. Four arms (baseline, the whole indicator collapsed,
// dmg_tunnel only, dmg_full only), one screenshot marker each, everything restored on exit; if
// the red survives arm B the indicator is excluded. Env VOTVCOOP_RUN_HUD_TINT_PROBE=1; mp.py
// hudtint.
DWORD WINAPI HudTintProbeThread(LPVOID arg);

// The movement oscillator (harness/autotest_move_osc.cpp), role-agnostic: circles the local
// player so the other peer's RemotePlayer interpolation has a moving source (with a static
// source the pose diagnostic's trail is trivially ~0 cm). Enable on one peer and read the
// other's `pose-diag ... trail=`: bounded near speed times window (~18 cm) is right, hundreds
// of cm is starved. Env VOTVCOOP_RUN_MOVE_OSC=1. Never ships.
void RunAutonomousMoveOsc();
DWORD WINAPI MoveOscThread(LPVOID arg);

// The EventFire replay smoke (harness/autotest_eventfire.cpp), host: after the join settles,
// solar (RunEvent, replay-allowlisted), arirGraff_0 (SpecialEvent, replay-allowlisted) and enasus
// (RunEvent, prop lane, no replay) fire through the same event_fire_sync::HostFire seam the F1
// menu uses; the client log proves two replays and one not replayed. Env
// VOTVCOOP_RUN_EVENTFIRE_TEST=1 (a LAN pair).
void RunAutonomousEventFireTest();
DWORD WINAPI EventFireTestThread(LPVOID arg);

// The event force-now smoke (harness/autotest_eventforce.cpp), host: the volume-gate feature
// (coop/dev/event_force) on obelisk, the badge snapshot resolving (armed 0, shots 1), ForceNow
// arming through HostFire and driving the box's own BeginOverlap with the local pawn, and the
// POST snapshot reading shots 0. Greppable "eventforce_test: VERDICT". Env
// VOTVCOOP_RUN_EVENTFORCE_TEST=1 (a LAN pair; the client observes the arm replay).
void RunAutonomousEventForceTest();
DWORD WINAPI EventForceTestThread(LPVOID arg);

// The starRain cue driver (harness/autotest_cueforce.cpp), host: runEvent('starRain') through
// HostFire as soon as the eventer resolves, before any client, so the orchestration can launch a
// client mid-shower and prove the event_cue join re-send delivers exactly one replay on the
// joiner. Env VOTVCOOP_RUN_CUEFORCE_TEST=1.
void RunAutonomousCueForceTest();
DWORD WINAPI CueForceTestThread(LPVOID arg);

// The base radar alarm driver (harness/autotest_alarmforce.cpp), host: after a 55 s client settle,
// DevForce runTrigger(1) then (0) on the native trigger; the alarm_sync poll must detect both
// edges and broadcast, and the client's own "applied active=N" lines are the assert. Env
// VOTVCOOP_RUN_ALARMFORCE_TEST=1.
void RunAutonomousAlarmForceTest();
DWORD WINAPI AlarmForceTestThread(LPVOID arg);

// The wisp mirror-lane smoke (harness/autotest_wisplane.cpp), host: ForceNow the wisps event
// (the swarm spawns ~32 wisp_C through EX_CallMath, which the Func-thunk catch must enrol and
// mirror), then midday sun so the landed wisps self-destroy invisibly to ProcessEvent and the
// pose-walk dead-retire must broadcast every EntityDestroy. The assert is the log diff. Env
// VOTVCOOP_RUN_WISPLANE_TEST=1.
void RunAutonomousWispLaneTest();
DWORD WINAPI WispLaneTestThread(LPVOID arg);

// The killerwisp acquisition probe (harness/autotest_kwisp_probe.cpp), host, read-only:
// SpawnKillerWispOnClient with the host teleported outside the 5,000-unit acquire radius, then
// FSM samples every 2 s for 90 s (target classification, harmless, tryGrab, grab, distances, in
// range) to localise where the chain breaks. Env VOTVCOOP_RUN_KWISP_PROBE=1.
void RunAutonomousKwispProbe();
DWORD WINAPI KwispProbeThread(LPVOID arg);

// The pause-guard test (harness/autotest_pauseguard.cpp), client: after the settle the world is
// paused through the game's own SetGamePaused (the state the ESC menu engages) and IsGamePaused
// sampled; pass if the pause reads false within 1 s and the log shows the guard's un-pause line.
// Env VOTVCOOP_RUN_PAUSE_TEST=1.
void RunAutonomousPauseGuardTest();
DWORD WINAPI PauseGuardTestThread(LPVOID arg);

// The piramid mirror-lane smoke (harness/autotest_piramidforce.cpp), host: ForceNow("piramid")
// runs the real native chain (the trigger overlap, the spawner's runTrigger, four killerwisp_C
// and a piramid2_C at scale 2); the lane must arm (DebugHooksArmed), and the wisps are re-pinned
// onto a 150 m ring around the pyramid every 5 s until its own brain acquires, arrives and
// gathers, DebugHostRelayCount >= 1 being the pass. The client side is a log diff. Env
// VOTVCOOP_RUN_PIRAMIDFORCE_TEST=1.
void RunAutonomousPiramidForceTest();
DWORD WINAPI PiramidForceTestThread(LPVOID arg);

// The bot-director halt probe (harness/autotest_navprobe.cpp), solo: two gates measured before
// any director is built. Gate A: UNavigationSystemV1::FindPathToLocationSynchronously returns a
// traversable path over VOTV's baked NavMesh to a reachable endpoint (call failed, no route, or
// traversable). Gate B: a reflected APawn::AddMovementInput (resolved on Pawn, not the leaf
// class) moves the possessed body, swept over four directions, horizontal displacement only,
// start location restored. The verdict picks the director's fallback rung. Greppable
// "nav_probe: VERDICT". Env VOTVCOOP_RUN_NAV_PROBE=1.
void RunNavHaltProbe();
DWORD WINAPI NavHaltProbeThread(LPVOID arg);

}  // namespace harness::autotest

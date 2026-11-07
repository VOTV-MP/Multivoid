// harness/autotest/drill_threads.h -- the autonomous routines' entry points: one Run* and *Thread
// pair per routine, each with one line naming its env var, its role and its .cpp, where the
// routine's doc lives. The dispatch table is autotest_dispatch.cpp.

#pragma once

#include <windows.h>

namespace harness::autotest {

// VOTVCOOP_RUN_GRAB_TEST, host -- harness/autotest/autotest_grab.cpp
void RunAutonomousGrabTest();
DWORD WINAPI GrabTestThread(LPVOID arg);

// VOTVCOOP_RUN_CLUMP_TEST, host drives -- harness/autotest/autotest_clump.cpp
void RunAutonomousClumpTest();
DWORD WINAPI ClumpTestThread(LPVOID arg);

// VOTVCOOP_RUN_PUPPET_GRAB_PROBE, host -- harness/autotest/autotest_chippile.cpp
void RunPuppetGrabProbe();
DWORD WINAPI PuppetGrabProbeThread(LPVOID arg);

// VOTVCOOP_RUN_TRASH_PARK, both peers -- harness/autotest/autotest_trashpark.cpp
void RunTrashParkProbe();
DWORD WINAPI TrashParkProbeThread(LPVOID arg);

// VOTVCOOP_RUN_PILE_DRIFT, host -- harness/autotest/autotest_chippile.cpp
void RunPileDriftScenario();
DWORD WINAPI PileDriftScenarioThread(LPVOID arg);

// VOTVCOOP_RUN_CLUMPVIS_PROBE, solo -- harness/autotest/autotest_clump.cpp
void RunClumpVisProbe();
DWORD WINAPI ClumpVisProbeThread(LPVOID arg);

// VOTVCOOP_RUN_WORLDRULES_PROBE, both peers -- harness/autotest/autotest_worldrules.cpp
void RunWorldRulesProbe();
DWORD WINAPI WorldRulesProbeThread(LPVOID arg);

// VOTVCOOP_RUN_FLASHLIGHT_TEST, both peers -- harness/autotest/autotest_flashlight.cpp
void RunAutonomousFlashlightTest();
DWORD WINAPI FlashlightTestThread(LPVOID arg);

// VOTVCOOP_RUN_CONFIG_SELFTEST, solo -- harness/autotest/autotest_config.cpp
void RunConfigSelftest();
DWORD WINAPI ConfigSelftestThread(LPVOID arg);

// VOTVCOOP_RUN_WEATHER_TEST, host -- harness/autotest/autotest_weather.cpp
void RunAutonomousWeatherTest();
DWORD WINAPI WeatherTestThread(LPVOID arg);

// VOTVCOOP_RUN_SEED_DRILL, host -- harness/autotest/autotest_seeddrill.cpp
DWORD WINAPI SeedDrillThread(LPVOID arg);

// VOTVCOOP_RUN_SCANPARITY, either role -- harness/autotest/autotest_scanparity.cpp
DWORD WINAPI ScanParityThread(LPVOID arg);

// VOTVCOOP_RUN_SAVEBLOCK_TEST, client -- harness/autotest/autotest_saveui.cpp
void RunAutonomousSaveBlockTest();
DWORD WINAPI SaveBlockTestThread(LPVOID arg);

// VOTVCOOP_RUN_SAVEBTN_TEST, client -- harness/autotest/autotest_saveui.cpp
void RunAutonomousSaveBtnDisableTest();
DWORD WINAPI SaveBtnDisableTestThread(LPVOID arg);

// VOTVCOOP_RUN_PROPREAP_TEST, both peers -- harness/autotest/autotest_tracker_selftest.cpp
void RunAutonomousPropReapTest();
DWORD WINAPI PropReapTestThread(LPVOID arg);

// VOTVCOOP_RUN_RESEED_TEST, both peers -- harness/autotest/autotest_tracker_selftest.cpp
void RunAutonomousReSeedTest();
DWORD WINAPI ReSeedTestThread(LPVOID arg);

// VOTVCOOP_RUN_RAGDOLL_TEST, both peers -- harness/autotest/autotest_ragdoll.cpp
void RunAutonomousRagdollTest();
DWORD WINAPI RagdollTestThread(LPVOID arg);

// VOTVCOOP_RUN_PUPPET_FRAME, host frames -- harness/autotest/autotest_puppetframe.cpp
void RunAutonomousPuppetFrame();
DWORD WINAPI PuppetFrameThread(LPVOID arg);

// VOTVCOOP_RUN_DAMAGE_TEST, both peers -- harness/autotest/autotest_damage.cpp
void RunAutonomousDamageTest();
DWORD WINAPI DamageTestThread(LPVOID arg);

// VOTVCOOP_RUN_DMGHAZARD_TEST, host verdict -- harness/autotest/autotest_dmghazard.cpp
void RunAutonomousDmgHazardTest();
DWORD WINAPI DmgHazardTestThread(LPVOID arg);

// VOTVCOOP_RUN_PLAYERDMG_TEST, both peers -- harness/autotest/autotest_playerdmg.cpp
void RunAutonomousPlayerDamageTest();
DWORD WINAPI PlayerDamageTestThread(LPVOID arg);

// VOTVCOOP_RUN_DEATH_TEST, solo -- harness/autotest/autotest_death.cpp
DWORD WINAPI DeathTestThread(LPVOID arg);

// VOTVCOOP_RUN_RAGDOLL_SPAWN_PROBE, solo -- harness/autotest/autotest_ragdoll_spawn_probe.cpp
void RunRagdollSpawnProbe();
DWORD WINAPI RagdollSpawnProbeThread(LPVOID arg);

// VOTVCOOP_RUN_MENUTRAVEL_PROBE, solo -- harness/autotest/autotest_menutravel_probe.cpp
void RunMenuTravelProbe();
DWORD WINAPI MenuTravelProbeThread(LPVOID arg);

// VOTVCOOP_RUN_RELOAD_CHURN, solo -- harness/autotest/autotest_reloadchurn.cpp
void RunReloadChurnProbe();
DWORD WINAPI ReloadChurnProbeThread(LPVOID arg);

// VOTVCOOP_RUN_ISLIVE_DRILL, solo -- harness/autotest/autotest_islive_drill.cpp
DWORD WINAPI IsLiveDrillThread(LPVOID arg);

// VOTVCOOP_RUN_FOG_PROBE, solo -- harness/autotest/autotest_fog_probe.cpp
void RunFogProbe();
DWORD WINAPI FogProbeThread(LPVOID arg);

// VOTVCOOP_RUN_HUD_TINT_PROBE, solo -- harness/autotest/autotest_hud_tint.cpp
DWORD WINAPI HudTintProbeThread(LPVOID arg);

// VOTVCOOP_RUN_MOVE_OSC, either role -- harness/autotest/autotest_move_osc.cpp
void RunAutonomousMoveOsc();
DWORD WINAPI MoveOscThread(LPVOID arg);

// VOTVCOOP_RUN_BEAM_LOOK, both peers -- harness/autotest/autotest_beam_look.cpp
DWORD WINAPI BeamLookThread(LPVOID arg);

// VOTVCOOP_RUN_FLASH_BATTERY, both peers -- harness/autotest/autotest_flash_battery.cpp
DWORD WINAPI FlashBatteryThread(LPVOID arg);

// VOTVCOOP_RUN_DESK_VIEW, both peers -- harness/autotest/autotest_desk_view.cpp
DWORD WINAPI DeskViewThread(LPVOID arg);

// VOTVCOOP_RUN_DISH_SOUND, both peers -- harness/autotest/autotest_dish_sound.cpp
DWORD WINAPI DishSoundThread(LPVOID arg);

// VOTVCOOP_RUN_SAVE_NOTICE, both peers -- harness/autotest/autotest_save_notice.cpp
DWORD WINAPI SaveNoticeThread(LPVOID arg);

// VOTVCOOP_RUN_VENT_CRAWLER, both peers -- harness/autotest/autotest_vent_crawler.cpp
DWORD WINAPI VentCrawlerThread(LPVOID arg);

// VOTVCOOP_RUN_CAN_STATE, both peers (shared folder VOTVCOOP_CAN_DIR) -- harness/autotest/autotest_can_state.cpp
DWORD WINAPI CanStateThread(LPVOID arg);

// VOTVCOOP_RUN_FOOD_STATE, both peers (shared folder VOTVCOOP_CAN_DIR) -- harness/autotest/autotest_food_state.cpp
DWORD WINAPI FoodStateThread(LPVOID arg);

// VOTVCOOP_RUN_EVENTFORCE_TEST, host -- harness/autotest/autotest_eventforce.cpp
void RunAutonomousEventForceTest();
DWORD WINAPI EventForceTestThread(LPVOID arg);

// VOTVCOOP_RUN_ALARMFORCE_TEST, host -- harness/autotest/autotest_alarmforce.cpp
void RunAutonomousAlarmForceTest();
DWORD WINAPI AlarmForceTestThread(LPVOID arg);

// VOTVCOOP_RUN_WISPLANE_TEST, host -- harness/autotest/autotest_wisplane.cpp
void RunAutonomousWispLaneTest();
DWORD WINAPI WispLaneTestThread(LPVOID arg);

// VOTVCOOP_RUN_KWISP_PROBE, host -- harness/autotest/autotest_kwisp_probe.cpp
void RunAutonomousKwispProbe();
DWORD WINAPI KwispProbeThread(LPVOID arg);

// VOTVCOOP_RUN_PAUSE_TEST, client -- harness/autotest/autotest_pauseguard.cpp
void RunAutonomousPauseGuardTest();
DWORD WINAPI PauseGuardTestThread(LPVOID arg);

// VOTVCOOP_RUN_PIRAMIDFORCE_TEST, host -- harness/autotest/autotest_piramidforce.cpp
void RunAutonomousPiramidForceTest();
DWORD WINAPI PiramidForceTestThread(LPVOID arg);

// VOTVCOOP_RUN_NAV_PROBE, solo -- harness/autotest/autotest_navprobe.cpp
void RunNavHaltProbe();
DWORD WINAPI NavHaltProbeThread(LPVOID arg);

// VOTVCOOP_RUN_KICK_PROBE, host -- harness/autotest/autotest_kickprobe.cpp
void RunKickProbe();
DWORD WINAPI KickProbeThread(LPVOID arg);

// VOTVCOOP_RUN_FLOOD_PROBE, client -- harness/autotest/autotest_floodprobe.cpp
void RunFloodProbe();
DWORD WINAPI FloodProbeThread(LPVOID arg);

// script_gate_drill=1, solo or host -- harness/autotest/autotest_scriptgate.cpp
void RunScriptGateDrill();
DWORD WINAPI ScriptGateDrillThread(LPVOID arg);

// VOTVCOOP_RUN_MENUSPAWN_DRILL, both peers -- harness/autotest/autotest_menuspawn.cpp
void RunMenuSpawnDrill();
DWORD WINAPI MenuSpawnDrillThread(LPVOID arg);

// VOTVCOOP_RUN_RUNEND_DRILL, solo host -- harness/autotest/autotest_runend.cpp
void RunRunEndDrill();
DWORD WINAPI RunEndDrillThread(LPVOID arg);

// VOTVCOOP_RUN_SPILL, both peers -- harness/autotest/autotest_spill.cpp
DWORD WINAPI SpillThread(LPVOID arg);

// VOTVCOOP_RUN_AMBIENT_CREATURE -- the ambient creatures: one set of deer and walking trees, the host's
DWORD WINAPI AmbientCreatureThread(LPVOID arg);

// VOTVCOOP_RUN_AMBIENT_FIELD -- one set of hexahive holes, sus holes and beehive branches
DWORD WINAPI AmbientFieldThread(LPVOID arg);

// VOTVCOOP_RUN_SUS_HOLE -- a sus hole a client rolls, made by the host
DWORD WINAPI SusHoleThread(LPVOID arg);
// VOTVCOOP_RUN_PLANT_STATE, both peers -- harness/autotest/autotest_plant_state.cpp
DWORD WINAPI PlantStateThread(LPVOID arg);

// woodchipper_drill, both peers -- harness/autotest/autotest_woodchipper.cpp
DWORD WINAPI WoodchipperThread(LPVOID arg);
// VOTVCOOP_RUN_SV_REQUEST, both peers -- harness/autotest/autotest_sv_request.cpp
DWORD WINAPI SvRequestThread(LPVOID arg);

// VOTVCOOP_RUN_MODULE_BUY, both peers -- harness/autotest/autotest_module_buy.cpp
DWORD WINAPI ModuleBuyThread(LPVOID arg);

// VOTVCOOP_RUN_PROP_PROPS, both peers -- harness/autotest/autotest_prop_props.cpp
DWORD WINAPI PropPropsThread(LPVOID arg);

// VOTVCOOP_RUN_DRONE_HOVER, both peers -- harness/autotest/autotest_drone_hover.cpp
DWORD WINAPI DroneHoverThread(LPVOID arg);
}  // namespace harness::autotest

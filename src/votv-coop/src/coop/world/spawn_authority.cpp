// coop/world/spawn_authority.cpp -- see the header. The rows rest on these bytecode facts: the mushroom master arms one
// looping spawn timer at begin-play, and its spawn mints spawner children with a lifespan, so refused children are reaped
// by the engine independently of the refused event; the mushroom spawner's own looping timer materialises the food cap and
// self-destroys, and with spawn refused the lifespan reaps it; the yellow-wisp ticker spawns at a navmesh random-walk
// point and its product is host-mirrored, so the client must not run its own spawner; the sky-wisp ticker spawns at
// absolute map coordinates, so the host rolls and clients mirror through the source-gated catch and the variant
// allowlist; the roach master's summon and its three looping timer entries fire independently of its tick. The tick rows:
// the insomniac and fossilhound tickers roll inside their tick with no delay chains or reap duties, so refusing the tick
// stops the roll and the product; the roach master's tick drives roach movement, the food-eat mutation and crush traces,
// which would diverge on a client, so it and its summoner are refused while the roach sync drives the client population.
// The jellyfish path's spawn makes seven fish inside its graph, mirrored through the source-gated catch, so a client's own
// (its 18:00 roll's) is refused. The yellow-wisp ticker's killer wisp, the fossilhound ticker's hound and the fire tank
// the gray tank's box unfolds into are mirrored through the same catch (that box's own unfolding is refused on a client).

#include "coop/world/spawn_authority.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>

namespace coop::spawn_authority {
namespace {

namespace SG = ue_wrap::script_gate;
namespace R  = ue_wrap::reflection;

std::atomic<coop::net::Session*> g_session{nullptr};

// The facts rows: the body a client must not run, named by its class and its function. The function
// is spelled as the live header dump spells it; names compare without case.
UE_FACT_FN(kRowSpawnMushroomMaster, "spawn_authority", L"mushroomMaster_C", L"Spawn", Declared);
UE_FACT_FN(kRowSpawnMushroomSpawner, "spawn_authority", L"mushroomSpawner_C", L"Spawn", Declared);
UE_FACT_FN(kRowReceiveTickTickerYellowWispSpawner, "spawn_authority", L"ticker_yellowWispSpawner_C",
           L"ReceiveTick", Declared);
UE_FACT_FN(kRowReceiveTickTickerWispSpawner, "spawn_authority", L"ticker_wispSpawner_C", L"ReceiveTick",
           Declared);
UE_FACT_FN(kRowReceiveTickTickerEgSpawner, "spawn_authority", L"ticker_egSpawner_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowSpawnJellyfishPath, "spawn_authority", L"jellyfishPath_C", L"spawn", Declared);
UE_FACT_FN(kRowSummonRoachCockroachMaster, "spawn_authority", L"cockroachMaster_C", L"summonRoach", Declared);
UE_FACT_FN(kRowAddRoachTimerCockroachMaster, "spawn_authority", L"cockroachMaster_C", L"addRoachTimer",
           Declared);
UE_FACT_FN(kRowSpawnNestTimerCockroachMaster, "spawn_authority", L"cockroachMaster_C", L"spawnNestTimer",
           Declared);
UE_FACT_FN(kRowCustomEventCockroachMaster, "spawn_authority", L"cockroachMaster_C", L"CustomEvent", Declared);
UE_FACT_FN(kRowReceiveTickTickerInsomniacSpawner, "spawn_authority", L"ticker_insomniacSpawner_C",
           L"ReceiveTick", Declared);
UE_FACT_FN(kRowReceiveTickTickerFossilhoundSpawner, "spawn_authority", L"ticker_fossilhoundSpawner_C",
           L"ReceiveTick", Declared);
UE_FACT_FN(kRowReceiveTickCockroachMaster, "spawn_authority", L"cockroachMaster_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowReceiveTickTickerRoachSummoner, "spawn_authority", L"ticker_roachSummoner_C", L"ReceiveTick",
           Declared);
UE_FACT_FN(kRowReceiveBeginPlayVentCrawler, "spawn_authority", L"ventCrawler_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowBoxBeginOverlapVentCrawler, "spawn_authority", L"ventCrawler_C",
           L"BndEvt__ventCrawler_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowReceiveBeginPlayGrayTest, "spawn_authority", L"grayTest_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowSphereBeginOverlapGrayTest, "spawn_authority", L"grayTest_C",
           L"BndEvt__grayTest_Sphere_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowDeacCamsGrayTest, "spawn_authority", L"grayTest_C", L"deacCams", Declared);
UE_FACT_FN(kRowDisableCamsGrayTest, "spawn_authority", L"grayTest_C", L"disableCams", Declared);
UE_FACT_FN(kRowReceiveBeginPlayEg, "spawn_authority", L"eg_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowReceiveTickEg, "spawn_authority", L"eg_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowOnLandedEg, "spawn_authority", L"eg_C", L"OnLanded", Declared);
UE_FACT_FN(kRowRetrieveEg, "spawn_authority", L"eg_C", L"retrieve", Declared);
UE_FACT_FN(kRowReceiveBeginPlayTentacleBall, "spawn_authority", L"tentacleBall_C", L"ReceiveBeginPlay",
           Declared);
UE_FACT_FN(kRowReceiveTickTentacleBall, "spawn_authority", L"tentacleBall_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowMoveTentacleBall, "spawn_authority", L"tentacleBall_C", L"move", Declared);
UE_FACT_FN(kRowSphereHitTentacleBall, "spawn_authority", L"tentacleBall_C",
           L"BndEvt__tentacleBall_Sphere_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowBeginGrayEventController, "spawn_authority", L"grayEventController_C", L"begin", Declared);
UE_FACT_FN(kRowTurnedonGrayEventController, "spawn_authority", L"grayEventController_C", L"turnedon", Declared);
UE_FACT_FN(kRowSpawnGrayEventController, "spawn_authority", L"grayEventController_C", L"spawn", Declared);
UE_FACT_FN(kRowTriggerboxOverlapGrayEventController, "spawn_authority", L"grayEventController_C",
           L"BndEvt__grayEventController_triggerbox_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowRunTriggerTentacleBallsFollower, "spawn_authority", L"tentacleBallsFollower_C", L"runTrigger",
           Declared);
UE_FACT_FN(kRowReceiveTickTentacleBallsFollower, "spawn_authority", L"tentacleBallsFollower_C", L"ReceiveTick",
           Declared);
UE_FACT_FN(kRowReceiveBeginPlaySuperEgger, "spawn_authority", L"superEgger_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowSpwnSuperEgger, "spawn_authority", L"superEgger_C", L"spwn", Declared);
UE_FACT_FN(kRowCustomEvent5Gamemode, "spawn_authority", L"mainGamemode_C", L"CustomEvent_5", Declared);
UE_FACT_FN(kRowReceiveTickTickerDeerSpawner, "spawn_authority", L"ticker_deerSpawner_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowReceiveTickTickerTreeSpawner, "spawn_authority", L"ticker_treeSpawner_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowReceiveTickTickerHexahiveSpawner, "spawn_authority", L"ticker_hexahiveSpawner_C", L"ReceiveTick",
           Declared);
UE_FACT_FN(kRowSpawnTickerBeehiveSpawner, "spawn_authority", L"ticker_beehiveSpawner_C", L"spawn", Declared);
UE_FACT_FN(kRowReceiveTickTickerMannequinSpawner, "spawn_authority", L"ticker_mannequinSpawner_C", L"ReceiveTick",
           Declared);
UE_FACT_FN(kRowSpawnBushTickerBushSpawning, "spawn_authority", L"ticker_bushSpawning_C", L"spawnBush", Declared);
UE_FACT_FN(kRowSphereHitMushroomSpore, "spawn_authority", L"mushroomSpore_C",
           L"BndEvt__mushroomSpore_Sphere_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowTrySpawnHerbOutsideChurch, "spawn_authority", L"outsideChurch_C", L"trySpawnHerb", Declared);
UE_FACT_FN(kRowReceiveBeginPlayUfoDropper, "spawn_authority", L"ufoDropper_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowReceiveBeginPlayUfoDropperTank, "spawn_authority", L"ufoDropper_tank_C", L"ReceiveBeginPlay",
           Declared);
UE_FACT_FN(kRowReceiveBeginPlayUfoDropperPig, "spawn_authority", L"ufoDropper_pig_C", L"ReceiveBeginPlay",
           Declared);
UE_FACT_FN(kRowDropTimelineUfoDropper, "spawn_authority", L"ufoDropper_C", L"Timeline_0__Drop__EventFunc",
           Declared);
UE_FACT_FN(kRowReceiveBeginPlaySoltomiaCleaning, "spawn_authority", L"soltomiaCleaning_C", L"ReceiveBeginPlay",
           Declared);
UE_FACT_FN(kRowBoxBeginOverlapSoltomiaCleaning, "spawn_authority", L"soltomiaCleaning_C",
           L"BndEvt__soltomiaCleaning_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowReceiveDestroyedSoltomiaCleaning, "spawn_authority", L"soltomiaCleaning_C", L"ReceiveDestroyed",
           Declared);
UE_FACT_FN(kRowReceiveTickGrayBoarSpawner, "spawn_authority", L"grayBoarSpawner_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowBoxBeginOverlapHoleController, "spawn_authority", L"event_bottomHoleController_C",
           L"BndEvt__event_bottomHoleController_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowSetEventLib, "spawn_authority", L"lib_C", L"setEvent", Declared);
UE_FACT_FN(kRowSpawnPartsFiretankDeploy, "spawn_authority", L"firetankDeploy_C", L"spawnParts", Declared);
UE_FACT_FN(kRowCheckSurroundingsTreehouse, "spawn_authority", L"treehouse_C", L"checkSurroundings", Declared);
UE_FACT_FN(kRowTrySpawnPigShooterTreehouse, "spawn_authority", L"treehouse_C", L"trySpawnPigShooter", Declared);
UE_FACT_FN(kRowTrySpawnTankShooterTreehouse, "spawn_authority", L"treehouse_C", L"trySpawnTankShooter", Declared);
UE_FACT_FN(kRowReceiveTickEventArirStealsGun, "spawn_authority", L"event_arirStealsGun_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowGraphCaveSpawner, "spawn_authority", L"antibreatherSPawner_C", L"ExecuteUbergraph_antibreatherSPawner",
           Declared);
UE_FACT_FN(kRowTickerLockerHeadGamemode, "spawn_authority", L"mainGamemode_C", L"ticker_lockerHead", Declared);
UE_FACT_FN(kRowTickerRadioTowerPoofGamemode, "spawn_authority", L"mainGamemode_C", L"ticker_radioTowerPoof", Declared);
UE_FACT_FN(kRowTickerLakeMonsterGamemode, "spawn_authority", L"mainGamemode_C", L"ticker_lakeMonsert", Declared);
UE_FACT_FN(kRowUfoBallGamemode, "spawn_authority", L"mainGamemode_C", L"ufo_ball", Declared);
UE_FACT_FN(kRowUfoBooGamemode, "spawn_authority", L"mainGamemode_C", L"ufo_boo", Declared);
UE_FACT_FN(kRowUfoJoelGamemode, "spawn_authority", L"mainGamemode_C", L"ufo_joel", Declared);
UE_FACT_FN(kRowUfoPillGamemode, "spawn_authority", L"mainGamemode_C", L"ufo_pill", Declared);
UE_FACT_FN(kRowUfoMidasGamemode, "spawn_authority", L"mainGamemode_C", L"ufo_midas", Declared);
UE_FACT_FN(kRowTickerBodyGamemode, "spawn_authority", L"mainGamemode_C", L"tickerBody", Declared);
UE_FACT_FN(kRowTickerFunguyGamemode, "spawn_authority", L"mainGamemode_C", L"tickerFunguy", Declared);
UE_FACT_FN(kRowSphereBeginOverlapTheBody, "spawn_authority", L"theBody_C",
           L"BndEvt__theBody_Sphere_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowRedPhoneTimerAnalogScreen, "spawn_authority", L"analogDScreenTest_C", L"redPhoneEventTimer", Declared);
UE_FACT_FN(kRowFinishedArirShipAppear, "spawn_authority", L"arirShipAppear_C", L"a__FinishedFunc", Declared);
UE_FACT_FN(kRowSpawnTrifoAnalogScreen, "spawn_authority", L"analogDScreenTest_C", L"spawn_trifo", Declared);
UE_FACT_FN(kRowSolarPowerControl, "spawn_authority", L"powercontrol_C", L"solar", Declared);
UE_FACT_FN(kRowFuckEriePlush, "spawn_authority", L"prop_eriePlush_C", L"fuck", Declared);
UE_FACT_FN(kRowAngerEriePlush, "spawn_authority", L"prop_eriePlush_C", L"anger", Declared);
UE_FACT_FN(kRowWriteEriePlush, "spawn_authority", L"prop_eriePlush_C", L"write", Declared);
UE_FACT_FN(kRowLoneTimerEriePlush, "spawn_authority", L"prop_eriePlush_C", L"lonetimer", Declared);
UE_FACT_FN(kRowEatCheeseTimerEriePlush, "spawn_authority", L"prop_eriePlush_C", L"eatcheesetimer", Declared);
UE_FACT_FN(kRowFireDamageEriePlush, "spawn_authority", L"prop_eriePlush_C", L"firedamage", Declared);
UE_FACT_FN(kRowMicrowaveEriePlush, "spawn_authority", L"prop_eriePlush_C", L"microwave", Declared);
UE_FACT_FN(kRowExplodedEriePlush, "spawn_authority", L"prop_eriePlush_C", L"exploded", Declared);
UE_FACT_FN(kRowReceiveBeginPlayErieHold, "spawn_authority", L"prop_erieHold_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowReceiveTickErieDrag, "spawn_authority", L"erieDrag_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowClothesFinishedErieChop, "spawn_authority", L"erieChop_C", L"anim_clothes__FinishedFunc", Declared);
UE_FACT_FN(kRowReceiveTickChurch, "spawn_authority", L"church_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowTimerTeleBooth, "spawn_authority", L"teleBooth_C", L"timer", Declared);
UE_FACT_FN(kRowReceiveTickSirenPost, "spawn_authority", L"sirenPost_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowReceiveBeginPlayHallwayDoor, "spawn_authority", L"hallwayDoor_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowReceiveTickHexPillar, "spawn_authority", L"hexpillar_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowReceiveTickEasterZone, "spawn_authority", L"easterZone_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowSpawnEggEasterEggSpawn, "spawn_authority", L"easterEggSpawn_C", L"spawnEgg", Declared);
UE_FACT_FN(kRowThrowItemsOminousBunny, "spawn_authority", L"ominousBunny_C", L"throwItems", Declared);
UE_FACT_FN(kRowReceiveDestroyedOminousBunny, "spawn_authority", L"ominousBunny_C", L"ReceiveDestroyed", Declared);
UE_FACT_FN(kRowTpTheDoor, "spawn_authority", L"TheDoor_C", L"tp", Declared);
UE_FACT_FN(kRowOpenTheDoor, "spawn_authority", L"TheDoor_C", L"open", Declared);
UE_FACT_FN(kRowCloseTheDoor, "spawn_authority", L"TheDoor_C", L"close", Declared);
UE_FACT_FN(kRowMoveRorGraf, "spawn_authority", L"rorGraf_C", L"move", Declared);
UE_FACT_FN(kRowReceiveTickLog, "spawn_authority", L"log_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowCheckLog, "spawn_authority", L"log_C", L"check", Declared);
UE_FACT_FN(kRowGraphPropDuc, "spawn_authority", L"prop_duc_C", L"ExecuteUbergraph_prop_duc", Declared);
UE_FACT_FN(kRowReceiveTickPropDuc, "spawn_authority", L"prop_duc_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowReceiveTickMannequinTp, "spawn_authority", L"mannequinTp_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowEatShrimpEaterMaster, "spawn_authority", L"shrimpEaterMaster_C", L"eat", Declared);
UE_FACT_FN(kRowReceiveTickFprints, "spawn_authority", L"fprints_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowRunTriggerJamDoor, "spawn_authority", L"trigger_jamDoor_C", L"runTrigger", Declared);

// One row: the facts row a client must not run, and the tag its refusal log carries. `pre` is the verdict
// when the row refuses only some calls, null for a row refused whole.
struct Row {
    const ue_wrap::facts::Function* row;
    const char* tag;
    SG::PreFn pre = nullptr;
};

SG::Verdict OnRowPre(const SG::Call& call);
SG::Verdict OnMirrorSetEventPre(const SG::Call& call);
SG::Verdict OnCaveSpawnerPre(const SG::Call& call);
SG::Verdict OnTrifoSolarPre(const SG::Call& call);
SG::Verdict OnDoorCallPre(const SG::Call& call);
SG::Verdict OnDuckGraphPre(const SG::Call& call);
constexpr Row kRows[] = {
    {&kRowSpawnMushroomMaster,            "mushroomMaster.Spawn"},
    {&kRowSpawnMushroomSpawner,           "mushroomSpawner.Spawn"},
    // A late-game class: keyed by name, the watch holds from the class's first body, whenever it loads.
    {&kRowReceiveTickTickerYellowWispSpawner, "yellowWispSpawner.ReceiveTick"},
    // Sky wisps: world-anchored, so the host rolls; the source-gated catch and the variant
    // allowlist mirror the products.
    {&kRowReceiveTickTickerWispSpawner,   "wispSpawner.ReceiveTick"},
    // The night-egg ticker: the same world-anchored shape -- its tick re-arms a 60-300 s
    // interval and rolls a weighted night spawn of one eg_C at random absolute map coordinates,
    // writing gamemode->eg (ticker_egSpawner_C UG:6-44). The host rolls, the catch mirrors the
    // egg; a client's own tick would mint a second, unsynced egg.
    {&kRowReceiveTickTickerEgSpawner,     "egSpawner.ReceiveTick"},
    // The space jellyfish: the host's run is mirrored through the source-gated catch.
    {&kRowSpawnJellyfishPath,             "jellyfishPath.spawn"},
    // The roach sim's entries: the ticker's cross-object call and the three looping timer
    // delegates. The roach sync drives the client population instead.
    {&kRowSummonRoachCockroachMaster,     "cockroachMaster.summonRoach"},
    {&kRowAddRoachTimerCockroachMaster,   "cockroachMaster.addRoachTimer"},
    {&kRowSpawnNestTimerCockroachMaster,  "cockroachMaster.spawnNestTimer"},
    {&kRowCustomEventCockroachMaster,     "cockroachMaster.CustomEvent"},
    // The ticks.
    {&kRowReceiveTickTickerInsomniacSpawner,   "insomniacSpawner.ReceiveTick"},
    {&kRowReceiveTickTickerFossilhoundSpawner, "fossilhoundSpawner.ReceiveTick"},
    {&kRowReceiveTickCockroachMaster,     "cockroachMaster.ReceiveTick"},
    {&kRowReceiveTickTickerRoachSummoner, "roachSummoner.ReceiveTick"},

    // The story-event creatures: on a client every instance is a host mirror -- the local spawn
    // paths are suppressed through the allowlist or refused below -- so their gameplay entries
    // are refused class-wide; the eventer's own verbs are event_fire_sync's. The AnimBP and the
    // host's pose stream stay; what a refused BeginPlay also set up is lost on the mirror: a
    // ball's and a gray's mesh attach to its spring arm, a ball's ambient loop. The crawler's
    // steps, vent break and burst, vent swing and door power are coop/world/vent_crawler_sync's.
    // ventCrawler_C: the whole scene is the BeginPlay body -- it binds the prop_vent_C ref,
    // plays the moveTL timeline (which itself SetActorLocations the crawl), arms the vent-bang
    // and footstep events, and ends in the door writes, the setEvent flag and K2_DestroyActor
    // (ventCrawler_C UG @2669). The box overlap is the end-of-crawl contact: a client pawn
    // touching the mirror must not open the doors locally (@2363).
    {&kRowReceiveBeginPlayVentCrawler,    "ventCrawler.ReceiveBeginPlay"},
    {&kRowBoxBeginOverlapVentCrawler,     "ventCrawler.BoxBeginOverlap"},
    // grayTest_C: BeginPlay re-attaches the mesh, waits a second and starts the wander MoveTo
    // (@1117); the 3 s loops arm from gatherTeam and step. The sphere overlap is the player
    // catch: on a mainPlayer touch it calls grayController->despawn() (@1508). disableCams is the
    // timer entry that calls deacCams (@1620), which deactivates the client's cameras, rdrone and kerfur.
    {&kRowReceiveBeginPlayGrayTest,       "grayTest.ReceiveBeginPlay"},
    {&kRowSphereBeginOverlapGrayTest,     "grayTest.SphereBeginOverlap"},
    {&kRowDeacCamsGrayTest,               "grayTest.deacCams"},
    {&kRowDisableCamsGrayTest,            "grayTest.disableCams"},
    // eg_C: BeginPlay arms the wander (@1466); the tick bobs the mesh and calls retrieve() once
    // player 0's camera is within 5000 (@15) -- refusing it costs the bob, while letting it run
    // lets a client egg leave on its own. retrieve() is the fly-away despawn whose Delay loop
    // writes rendered and setEvent (@1024, @1476). OnLanded arms check and a 10-300 s
    // retrieve (@1869).
    {&kRowReceiveBeginPlayEg,             "eg.ReceiveBeginPlay"},
    {&kRowReceiveTickEg,                  "eg.ReceiveTick"},
    {&kRowOnLandedEg,                     "eg.OnLanded"},
    {&kRowRetrieveEg,                     "eg.retrieve"},
    // tentacleBall_C: BeginPlay resolves the anim instance, starts Timeline_0 and the ambient
    // loop, and arms the 15-30 s timer and the first move (@7603). The tick is the brain:
    // proximity scans, the pindown/chase decision, the stare updates (@8872). move() is the
    // MoveTo verb it reaches (@10046); the sphere hit is the melee contact (@10314). The
    // montage-notify talk/light callbacks and step() stay -- sound and light presentation.
    {&kRowReceiveBeginPlayTentacleBall,   "tentacleBall.ReceiveBeginPlay"},
    {&kRowReceiveTickTentacleBall,        "tentacleBall.ReceiveTick"},
    {&kRowMoveTentacleBall,               "tentacleBall.move"},
    {&kRowSphereHitTentacleBall,          "tentacleBall.SphereHit"},

    // The event spawners themselves: a client's copies must never mint children. Every
    // BeginDeferred inside these graphs is bytecode-internal, so the spawn interceptor never
    // sees them; refusing the bodies that contain them is the only suppression that holds.
    // grayEventController_C (level-placed on both sides): spawn() is the only body with the
    // grayTest BeginDeferred (@2617, the marker loop) -- it also plays the Audio, writes
    // isRaining=false and runs the deac-cams SphereOverlap. begin() (@3064) is the eventer's
    // entry -- the player punch, the transformer break, the car zap -- and turnedon (@3388)
    // and the triggerbox overlap (@3309) are its local activations.
    {&kRowBeginGrayEventController,       "grayEventController.begin"},
    {&kRowTurnedonGrayEventController,    "grayEventController.turnedon"},
    {&kRowSpawnGrayEventController,       "grayEventController.spawn"},
    {&kRowTriggerboxOverlapGrayEventController, "grayEventController.triggerboxOverlap"},
    // tentacleBallsFollower_C (level-placed on both sides): runTrigger is the five-ball spawn
    // loop plus the setBuddies pass and the cleanup timers (@3432). The tick moves the follow
    // marker along the spline and writes setEvent at the resume and end points (@2012); the
    // client's copy has an empty balls array and nothing to guide.
    {&kRowRunTriggerTentacleBallsFollower,  "tentacleBallsFollower.runTrigger"},
    {&kRowReceiveTickTentacleBallsFollower, "tentacleBallsFollower.ReceiveTick"},
    // superEgger_C: a client's copy is a world-actor mirror, but FinishSpawning runs its
    // BeginPlay, which builds the spawn grid and chains spwn() -- the only body with the eg_C
    // BeginDeferred (@1399, @1316). Refusing both keeps the mirror a husk.
    {&kRowReceiveBeginPlaySuperEgger,     "superEgger.ReceiveBeginPlay"},
    {&kRowSpwnSuperEgger,                 "superEgger.spwn"},
    // mainGamemode_C: the night roll. CustomEvent_5 is a one-shot timer's body, and the body is the roll AND its
    // own re-arm (eleven blocks of the graph: the 3 % night roll that mints a Geom Octa or a night egg, and the next
    // 120-300 s timer), so a client's timer simply ends here. The host rolls and the source-gated catch mirrors the
    // Octa and the egg; the level-placed egg roller is already a row above.
    {&kRowCustomEvent5Gamemode,           "gamemode.CustomEvent_5"},

    // The ambient creatures and fixtures, the host's: the deer and walking tree roll absolute map points and the npc lane
    // mirrors what the host rolled; the hexahive's field of dirt holes and the beehive's branches cross on the fixture lane.
    // The deer, tree and hexahive tickers roll in their tick; the beehive ticker's spawn is its looping timer's handler,
    // refused as the cockroach master's timer rows are.
    {&kRowReceiveTickTickerDeerSpawner,     "deerSpawner.ReceiveTick"},
    {&kRowReceiveTickTickerTreeSpawner,     "treeSpawner.ReceiveTick"},
    {&kRowReceiveTickTickerHexahiveSpawner, "hexahiveSpawner.ReceiveTick"},
    {&kRowSpawnTickerBeehiveSpawner,        "beehiveSpawner.spawn"},
    // The mannequin spawner: its tick is the only root of its graph (ubergraph entry 1788), and the walker's spawn, its
    // Delay resumes and the reap of the walkers it counts are all scheduled from that entry, in exclusive branches, so the
    // tick's row refuses every one. The host's walker, the statue's stand swap and its end cross as keyed props.
    {&kRowReceiveTickTickerMannequinSpawner, "mannequinSpawner.ReceiveTick"},
    // The plant spawners: each product is a growingPlant_C, which the host's plant_sync sends as PlantState, so a
    // client's own would stand apart from the host's under another key. The winter bush ticker's timer entry plants
    // one bush on a random trace (ticker_bushSpawning UG @206, spawnBush). A spore's hit plants a shroom_7 at the hit
    // point on grass, dirt or gravel (mushroomSpore UG @29); the client's spores are the host's mirrors, and its own
    // hit would plant beside them. The church's trySpawnHerb rolls the stolas herb while it rains, from its begin play
    // and its tick (outsideChurch UG @2116).
    {&kRowSpawnBushTickerBushSpawning,    "bushSpawning.spawnBush"},
    {&kRowSphereHitMushroomSpore,         "mushroomSpore.SphereHit"},
    {&kRowTrySpawnHerbOutsideChurch,      "outsideChurch.trySpawnHerb"},
    // The level's wandering fixtures, each placed in every peer's world (coop/world/level_fixture_sync places a client's
    // copies where the host's stand). The Cursed Church's tick rolls a point and moves the church there when unseen; the
    // Telephone Booth's two-hour timer shows it and may move it; the Siren Pole's tick walks it while unseen; the Hallway
    // White Door's begin play destroys it unless the save was loaded at 03:33, by this peer's own clock.
    {&kRowReceiveTickChurch,              "church.ReceiveTick"},
    {&kRowTimerTeleBooth,                 "teleBooth.timer"},
    {&kRowReceiveTickSirenPost,           "sirenPost.ReceiveTick"},
    {&kRowReceiveBeginPlayHallwayDoor,    "hallwayDoor.ReceiveBeginPlay"},
    // A Basalt Pillar's tick raises it and, in the same body, seeds a neighbour: the field is the host's, and a copy is
    // raised by the height the host sends (coop/world/basalt_pillar). A pillar a joiner's save loaded is refused here from
    // its first tick, before the lane adopts it.
    {&kRowReceiveTickHexPillar,           "hexPillar.ReceiveTick"},
    // The Easter hunt is the host's (coop/world/easter_hunt). The well's zone counts the carrots in its tick and starts the
    // animation that mints the bunny; an egg point's spawnEgg mints one frozen egg prop; the bunny's throwItems mints the
    // ears and the note; a bunny destroyed before its hunt ended mints the angry one. The host's eggs and items cross as
    // props and its bunnies as world actors, so a client's own would stand beside them.
    {&kRowReceiveTickEasterZone,          "easterZone.ReceiveTick"},
    {&kRowSpawnEggEasterEggSpawn,         "easterEggSpawn.spawnEgg"},
    {&kRowThrowItemsOminousBunny,         "ominousBunny.throwItems"},
    {&kRowReceiveDestroyedOminousBunny,   "ominousBunny.ReceiveDestroyed"},
    // The White Door (TheDoor_C): its 3600 s chain rolls a point in tp() and moves the door there when unseen, and its 10-15 s
    // chain rolls open() or close(). Those are the host's (the pose, shown and open state cross on the level-fixture lane), so tp
    // is refused whole and open and close are refused unless the lane's own call made them (it drives the copy to the host's
    // state). The near-sphere slam is not on this list: the two overlap entries run the close inline, so a copy something stands
    // beside slams on its own and the host's word comes with its next change.
    {&kRowTpTheDoor,                      "theDoor.tp"},
    {&kRowOpenTheDoor,                    "theDoor.open", &OnDoorCallPre},
    {&kRowCloseTheDoor,                   "theDoor.close", &OnDoorCallPre},
    // The Rorschach decal's move() (its 10 s loop, unseen), the Ghost Log's tick roll and its 10 s pawn check (both end in the
    // teleport), the Rubber Duck's 60-300 s chain (a delay resume of its ubergraph; its tick catches a fall out of the world) and
    // the tick of the trigger that carries the base's mannequins about while unseen: each moves a thing the host's copy carries
    // to every peer on the level-fixture lane.
    {&kRowMoveRorGraf,                    "rorGraf.move"},
    {&kRowReceiveTickLog,                 "log.ReceiveTick"},
    {&kRowCheckLog,                       "log.check"},
    {&kRowGraphPropDuc,                   "propDuc.chain", &OnDuckGraphPre},
    {&kRowReceiveTickPropDuc,             "propDuc.ReceiveTick"},
    {&kRowReceiveTickMannequinTp,         "mannequinTp.ReceiveTick"},
    // The shrimp eater's looping 15 s timer (its `eat` entry): it destroys the shrimp prop nearest its last place and mints the
    // empty pack, so only the host's run changes the shrimp props; a client's own would destroy and mint beside it.
    {&kRowEatShrimpEaterMaster,           "shrimpEater.eat"},
    // The footprints' tick flickers the actor while unseen; its shown state is the host's. A client's show() is asked of the host.
    {&kRowReceiveTickFprints,             "fprints.ReceiveTick"},
    // The Reactor Restart's trigger (trigger_jamDoor_C): its `runTrigger` jams the two base doors (`door_C::jam`, the door lane carries
    // their state) and sends the "Minor inconvenience" mail (the mail lane carries it). It is started by the level's armed trigger
    // box when the host's player walks in, so it is the host's; a client's own run would jam and mail twice.
    {&kRowRunTriggerJamDoor,              "jamDoor.runTrigger"},

    // The saucers the eventer flies in (ufoDropper_car_C, _body_C, _tank_C and _pig_C), a world-actor mirror each. The
    // car and the body declare no function of their own, so the base class's rows reach them; the tank and the pig
    // override the begin play and own theirs. The base begin play registers the actor as a save actor and arms the
    // 10 s kerfur drop; the tank's and the pig's breaks generator 0 after it, which the power lane carries. The
    // timeline's Drop event mints the dropped car or body, and the host's mint reaches a client as an ordinary prop,
    // so a client's own would stand beside it. The saucer's descent, particles and unhide stay:
    // world_actor_mirror deploys the copy.
    {&kRowReceiveBeginPlayUfoDropper,     "ufoDropper.ReceiveBeginPlay"},
    {&kRowReceiveBeginPlayUfoDropperTank, "ufoDropperTank.ReceiveBeginPlay"},
    {&kRowReceiveBeginPlayUfoDropperPig,  "ufoDropperPig.ReceiveBeginPlay"},
    {&kRowDropTimelineUfoDropper,         "ufoDropper.DropTimeline"},
    // The Soltomia craft (soltomiaCleaning_C), a world-actor mirror. Its begin play registers the event, finds the ATV and
    // zeroes its dirt, looks the doors up by key and binds the door's opened event (the cleaning montage and its sponge
    // throw hang from that bind). The box overlap is the entry: it teleports the ATV, closes a door, jams three, switches
    // the password locks off and starts the talk loop. Its destroy releases the jams and ends the event. The host's run
    // of each reaches a client by the ATV, door, prop and event lanes, so the mirror runs none: it keeps the actor and its
    // animation blueprint, and world_actor_mirror shows it once an ATV stands on it. With these three
    // refused, opened, talking, talk, repeat and the montage's delegates (the throw's sponge mint, the faces, the
    // destroy) are never entered; ReceiveTick only stores the cleaning mesh's location, and the mirror's tick is parked.
    {&kRowReceiveBeginPlaySoltomiaCleaning,  "soltomiaCleaning.ReceiveBeginPlay"},
    {&kRowBoxBeginOverlapSoltomiaCleaning,   "soltomiaCleaning.BoxBeginOverlap"},
    {&kRowReceiveDestroyedSoltomiaCleaning,  "soltomiaCleaning.ReceiveDestroyed"},
    // The night boar spawner of a combat save: its tick rolls a boar near the camera and reaps one far from it. The
    // host's boars cross as props and its claim streams them (coop/creatures/boar_sync), so a client's own roll would
    // stand beside them, with a brain the copy gate has parked.
    {&kRowReceiveTickGrayBoarSpawner,     "grayBoarSpawner.ReceiveTick"},
    // The Rozital hole's controller is a save actor, so a joiner loads one. Its box overlap mints the room, the
    // device, the patrol, the grate and the beacons, and swaps the fence to a dynamic prop: the host's, which reach
    // a client as props, a world actor and creatures.
    {&kRowBoxBeginOverlapHoleController,  "holeController.BoxBeginOverlap"},
    // lib_C::setEvent from one of the ships' own mirrors: the in-flight count, the sender list and the ambient track
    // are the host's, and a mirror whose host actor is destroyed first never reaches its own end call, so the count
    // would stay raised and hold off the save. The mothership's, the morning UFO's and the knocker's begin plays
    // register, as does the saucer's activation, and each ends in its own clearing call.
    {&kRowSetEventLib,                    "lib.setEvent(ship mirror)", &OnMirrorSetEventPre},
    // The Day-44 gray tank's box unfolds in spawnParts, the only body with its fire tank's BeginDeferred and the one
    // that mints the scrap props the box is made of. The box is the host's (the saucer's drop that mints it is refused
    // above, and no lane carries the box itself to a client); the host's tank crosses through the source-gated catch
    // and its scrap as props. A box a client somehow held would mint a second tank and a second heap beside them.
    {&kRowSpawnPartsFiretankDeploy,       "firetankDeploy.spawnParts"},
    // The Ariral treehouse (a level actor on both sides, anchored at its own point) and the dropped gun's event actor: the
    // host's mint their Arirals, which cross through the source-gated catch (npc_world_enum) as parked copies. The pig
    // shooter's roll and the tank shooter's are the two bodies with those spawns, the latter also called from the Ariral
    // crate's use (a client's opening then reads no shooter and goes on to its own slapper, bound to the player who opened
    // it). checkSurroundings is the eater sphere's timer: it destroys the prop_poo, gun, shrimp and food lying in it, pays
    // the Ariral reputation for each, mints the slapper for a prop_poo and throws a camera back, so a client's own run would
    // pay and destroy again beside the host's. The event actor's tick is the only root of its graph: from dusk to dawn, with
    // the gun about, it mints the stealer in a ring about player 0.
    {&kRowCheckSurroundingsTreehouse,     "treehouse.checkSurroundings"},
    {&kRowTrySpawnPigShooterTreehouse,    "treehouse.trySpawnPigShooter"},
    {&kRowTrySpawnTankShooterTreehouse,   "treehouse.trySpawnTankShooter"},
    {&kRowReceiveTickEventArirStealsGun,  "arirStealsGun.ReceiveTick"},
    // The cave's spawner (level-placed on both sides). Its begin play re-arms a 10 s Delay whose resume is the roll: the
    // antibreather at the spawner's point (or its removal), and `cencipede`, which mints Wolfgang or the egg. Both are the
    // host's, anchored at the cave (the products cross through the source-gated catch: coop/creatures/npc_world_enum), so a
    // client's loop and its `cencipede` entry are refused. The rest of its begin play stays: the 5 s `block` timer closes
    // the cave's rock on the client's own distance, and the music trigger and the cave's ambience are the client's own.
    {&kRowGraphCaveSpawner,               "caveSpawner.loop", &OnCaveSpawnerPre},

    // The gamemode's hourly and lake rolls and the downloading panel's ring roll, each a timer's handler: the host rolls and
    // coop/world/event_world_sync carries the result to every peer, so a client's own roll would stand apart from the host's.
    // ticker_lockerHead (every 3600 s: 8 % that a random locker holds a head for whoever opens it, 1 % a lockerCorpse_C at a random
    // locker, whose gore is props and a decal on their own lanes), ticker_radioTowerPoof (0.5 %: the radio tower vanishes while
    // nobody looks) and ticker_lakeMonsert (a 15 to 20 minute re-arming roll that lights the lake on a moonlit night). The
    // panel's redPhoneEventTimer rolls 2.5 % to start the red phone's ring. A client's timer simply ends here.
    {&kRowTickerLockerHeadGamemode,       "gamemode.ticker_lockerHead"},
    {&kRowTickerRadioTowerPoofGamemode,   "gamemode.ticker_radioTowerPoof"},
    {&kRowTickerLakeMonsterGamemode,      "gamemode.ticker_lakeMonsert"},
    // The gamemode's five UFO timers, the body's and the funguy's tickers: each a one-shot or looping timer handler that rolls
    // and re-arms itself in its own body, so a client's timer simply ends here. The host's roll is the one UFO, body or duende:
    // none of the four UFO classes (ufo_ballfo_C, ufo_boofo_spawn1_C, ufo_joel_C, ufo_pillfo_C) spawns another actor, and the
    // body's and funguy's mint a mushroom spawner or a dirt hole only from their own graphs, which a copy has parked. The UFOs
    // and the body are world-actor mirrors, the funguy an NPC mirror (npc_world_enum's mainGamemode_C outputs). The Midas UFO
    // is a level-placed spline flyer (midasUfoTest_C), so its timer only runs its `runTrigger`: the host's start reaches a
    // client as an EventCue that starts the client's own (coop/world/event_world_sync). Its overlap row refuses the copy's
    // 5 s self-destroy on a pawn's touch: the host's body ends the copy.
    {&kRowUfoBallGamemode,                "gamemode.ufo_ball"},
    {&kRowUfoBooGamemode,                 "gamemode.ufo_boo"},
    {&kRowUfoJoelGamemode,                "gamemode.ufo_joel"},
    {&kRowUfoPillGamemode,                "gamemode.ufo_pill"},
    {&kRowUfoMidasGamemode,               "gamemode.ufo_midas"},
    {&kRowTickerBodyGamemode,             "gamemode.tickerBody"},
    {&kRowTickerFunguyGamemode,           "gamemode.tickerFunguy"},
    {&kRowSphereBeginOverlapTheBody,      "theBody.SphereBeginOverlap"},
    {&kRowRedPhoneTimerAnalogScreen,      "analogScreen.redPhoneEventTimer"},
    // The Day-13 ship tower's timeline end (`a`, its fade in): with the arrow fully shown it mails the blank "outside" rows on a
    // random 10 s and up loop. The host's rows reach a client on the email lane, so a tower a client shows (on the host's cue)
    // would mail a second set. The fade, the radar point and the event count are the tower's own and stay.
    {&kRowFinishedArirShipAppear,         "arirShipAppear.finished"},
    // The base computer's TriFO: `spawn_trifo` is called by the desk's autoSave and by the save button's press, the two paths a
    // client's own refusals (desk_sim_sync, desk_verb_intent) already stop, and nothing else; refused whole as a third lock. The
    // host's TriFO is a world-actor copy on a client, whose begin play blacks the base out through powercontrol.solar() once its
    // flight starts (the lights and breakers reach the client by the host's lanes), so that call is refused when a TriFO makes it.
    {&kRowSpawnTrifoAnalogScreen,         "analogScreen.spawn_trifo"},
    {&kRowSolarPowerControl,              "powercontrol.solar(trifo copy)", &OnTrifoSolarPre},
    // The Erie plush is the host's (coop/world/erie_sync); a client's copy of it is a pose. `fuck` is the morph (the live Erie, the
    // achievement, the plush's end), `anger` the angry Erie's mint, `write` the notes' mint and the food's pick; `lonetimer` moves the
    // plush when no player is by it, `eatcheesetimer` eats and moves the cheese props about; fire, a microwave and an explosion each end
    // in `anger` or in `replaceProp` (the talking Erie). The hit handler is its own watch (the host is asked), and the host's results cross
    // as props. The held form's begin play places a live Erie or throws the drag form, the drag form's tick lands it (a live Erie again,
    // its own end), and the chop's clothes timeline ends in two live Erie: each is a mint the host's copy makes.
    {&kRowFuckEriePlush,                  "eriePlush.fuck"},
    {&kRowAngerEriePlush,                 "eriePlush.anger"},
    {&kRowWriteEriePlush,                 "eriePlush.write"},
    {&kRowLoneTimerEriePlush,             "eriePlush.lonetimer"},
    {&kRowEatCheeseTimerEriePlush,        "eriePlush.eatcheesetimer"},
    {&kRowFireDamageEriePlush,            "eriePlush.firedamage"},
    {&kRowMicrowaveEriePlush,             "eriePlush.microwave"},
    {&kRowExplodedEriePlush,              "eriePlush.exploded"},
    {&kRowReceiveBeginPlayErieHold,       "erieHold.ReceiveBeginPlay"},
    {&kRowReceiveTickErieDrag,            "erieDrag.ReceiveTick"},
    {&kRowClothesFinishedErieChop,        "erieChop.anim_clothes_finished"},
};
constexpr int kRowCount = static_cast<int>(std::size(kRows));
constexpr int kTagBase = 0x53410000;   // 'SA', then the row

// Refusals per row, for the log: the first and then each power of two, so a refused tick proves itself
// without a line a second. Game thread, as the gate's callbacks are.
std::uint64_t g_refused[kRowCount] = {};

SG::Verdict OnRowPre(const SG::Call& call) {
    if (!IsActiveClientSession()) return SG::Verdict::Run;
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount) return SG::Verdict::Run;
    const std::uint64_t n = ++g_refused[row];
    if ((n & (n - 1)) == 0)
        UE_LOGI("spawn_authority[%s]: client-refuse %p (call #%llu)", kRows[row].tag, call.object,
                static_cast<unsigned long long>(n));
    return SG::Verdict::Cancel;
}

// The cave spawner's ubergraph, called with an EntryPoint (a cooked byte offset of 0.10.0: it moves with a game update, and the
// row's fingerprint says so): 30 is the resume of the 10 s Delay of its begin play, whose body is the roll and which re-arms
// itself, and 3593 is `cencipede`. Every other entry runs: the begin play (2603), `block` (3069) and the sphere's overlap
// (3333). The first sight of each entry is said, so a drill can read the numbers against these.
constexpr int32_t kCaveLoopEntry = 30;
constexpr int32_t kCaveCencipedeEntry = 3593;

SG::Verdict OnCaveSpawnerPre(const SG::Call& call) {
    if (!IsActiveClientSession()) return SG::Verdict::Run;
    static R::ParamOffset s_entryParam{L"EntryPoint"};
    int32_t entry = -1;
    const int32_t off = s_entryParam.Of(call.function);
    if (off >= 0 && call.locals) std::memcpy(&entry, call.locals + off, sizeof(entry));
    static int32_t s_seen[8];
    static int s_seenCount = 0;
    bool known = false;
    for (int i = 0; i < s_seenCount; ++i) known = known || s_seen[i] == entry;
    if (!known && s_seenCount < 8) {
        s_seen[s_seenCount++] = entry;
        UE_LOGI("spawn_authority[caveSpawner]: a client's spawner graph entered at entry %d (refused: %d)", entry,
                (entry == kCaveLoopEntry || entry == kCaveCencipedeEntry) ? 1 : 0);
    }
    if (entry != kCaveLoopEntry && entry != kCaveCencipedeEntry) return SG::Verdict::Run;
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount) return SG::Verdict::Cancel;
    const std::uint64_t n = ++g_refused[row];
    if ((n & (n - 1)) == 0)
        UE_LOGI("spawn_authority[%s]: client-refuse %p entry %d (call #%llu)", kRows[row].tag, call.object, entry,
                static_cast<unsigned long long>(n));
    return SG::Verdict::Cancel;
}

// open() and close() of the White Door are refused on a client unless the level-fixture lane's own call made them.
SG::Verdict OnDoorCallPre(const SG::Call& call) {
    if (!IsActiveClientSession() || call.fromOurCode) return SG::Verdict::Run;
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount) return SG::Verdict::Cancel;
    const std::uint64_t n = ++g_refused[row];
    if ((n & (n - 1)) == 0)
        UE_LOGI("spawn_authority[%s]: client-refuse %p (call #%llu)", kRows[row].tag, call.object,
                static_cast<unsigned long long>(n));
    return SG::Verdict::Cancel;
}

// The Rubber Duck's ubergraph, entered with an EntryPoint (a cooked byte offset of 0.10.0): 137 is the resume of the 60-300 s delay
// of its begin play, whose body is the roll and the move and which re-arms itself. The others run: the use's quack (a presentation),
// the begin play's start and the quack's resume (1466, only reached from the move). The first sight of each entry is said.
constexpr int32_t kDuckChainEntry = 137;

SG::Verdict OnDuckGraphPre(const SG::Call& call) {
    if (!IsActiveClientSession()) return SG::Verdict::Run;
    static R::ParamOffset s_entryParam{L"EntryPoint"};
    int32_t entry = -1;
    const int32_t off = s_entryParam.Of(call.function);
    if (off >= 0 && call.locals) std::memcpy(&entry, call.locals + off, sizeof(entry));
    static int32_t s_seen[8];
    static int s_seenCount = 0;
    bool known = false;
    for (int i = 0; i < s_seenCount; ++i) known = known || s_seen[i] == entry;
    if (!known && s_seenCount < 8) {
        s_seen[s_seenCount++] = entry;
        UE_LOGI("spawn_authority[propDuc]: a client's duck graph entered at entry %d (refused: %d)", entry,
                entry == kDuckChainEntry ? 1 : 0);
    }
    if (entry != kDuckChainEntry) return SG::Verdict::Run;
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount) return SG::Verdict::Cancel;
    const std::uint64_t n = ++g_refused[row];
    if ((n & (n - 1)) == 0)
        UE_LOGI("spawn_authority[%s]: client-refuse %p entry %d (call #%llu)", kRows[row].tag, call.object, entry,
                static_cast<unsigned long long>(n));
    return SG::Verdict::Cancel;
}

// Whether `cls` names one of the eventer's ships: the ufoDropper saucers, the mothership, the morning UFO or the
// knocker, each a world-actor mirror on a client.
bool IsShipClass(const std::wstring& cls) {
    return _wcsnicmp(cls.c_str(), L"ufoDropper_", 11) == 0 || _wcsicmp(cls.c_str(), L"rozitBorg_C") == 0 ||
           _wcsicmp(cls.c_str(), L"morningUfo_C") == 0 || _wcsicmp(cls.c_str(), L"kocker_C") == 0 ||
           // the gamemode's UFOs: each registers its flight in its begin play and ends it in its own destroy
           _wcsicmp(cls.c_str(), L"ufo_ballfo_C") == 0 || _wcsnicmp(cls.c_str(), L"ufo_boofo", 9) == 0 ||
           _wcsicmp(cls.c_str(), L"ufo_joel_C") == 0 || _wcsicmp(cls.c_str(), L"ufo_pillfo_C") == 0 ||
           // the base computer's TriFO and Tardis: each registers in its begin play and ends in its own destroy
           _wcsicmp(cls.c_str(), L"ufo_trifo_C") == 0 || _wcsicmp(cls.c_str(), L"tardis_C") == 0;
}

// lib_C::setEvent is every event's own begin and end, so only a ship's call is refused: the sender is the call's
// __WorldContext parameter, as event_active_sync reads it.
SG::Verdict OnMirrorSetEventPre(const SG::Call& call) {
    if (!IsActiveClientSession()) return SG::Verdict::Run;
    static int32_t s_senderOff = -2;
    if (s_senderOff == -2) s_senderOff = R::FindParamOffset(call.function, L"__WorldContext");
    if (s_senderOff < 0 || !call.locals) return SG::Verdict::Run;
    void* sender = *reinterpret_cast<void* const*>(call.locals + s_senderOff);
    if (!sender || !R::IsLive(sender) || !IsShipClass(R::ClassNameOf(sender))) return SG::Verdict::Run;
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount) return SG::Verdict::Cancel;
    const std::uint64_t n = ++g_refused[row];
    if ((n & (n - 1)) == 0)
        UE_LOGI("spawn_authority[%s]: client-refuse %p (call #%llu)", kRows[row].tag, sender,
                static_cast<unsigned long long>(n));
    return SG::Verdict::Cancel;
}

// powercontrol_C::solar from the TriFO's own graph (its abduct's last step): the call frame's object is the TriFO. Any other caller
// (a generator's update, the desk's short circuit, the Sol event) is the host's lane and runs.
SG::Verdict OnTrifoSolarPre(const SG::Call& call) {
    if (!IsActiveClientSession() || !call.callerObject || !R::IsLive(call.callerObject)) return SG::Verdict::Run;
    if (_wcsicmp(R::ClassNameOf(call.callerObject).c_str(), L"ufo_trifo_C") != 0) return SG::Verdict::Run;
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount) return SG::Verdict::Cancel;
    const std::uint64_t n = ++g_refused[row];
    if ((n & (n - 1)) == 0)
        UE_LOGI("spawn_authority[%s]: client-refuse %p (call #%llu)", kRows[row].tag, call.callerObject,
                static_cast<unsigned long long>(n));
    return SG::Verdict::Cancel;
}

std::atomic<bool> g_watched{false};

// The drills' red arms leave rows out of the table, so a drill that reads the refusal counters must fail. The creature drill's
// arm leaves all four ambient rows out, the field drill's the hexahive's and the beehive's, the mannequin drill's (`no_row`) the
// mannequin spawner's. Read once, at the install.
bool LeftOutByRedArm(const char* tag) {
    const bool fixtureRow = std::strcmp(tag, "hexahiveSpawner.ReceiveTick") == 0 || std::strcmp(tag, "beehiveSpawner.spawn") == 0;
    const bool creatureRow = std::strcmp(tag, "deerSpawner.ReceiveTick") == 0 || std::strcmp(tag, "treeSpawner.ReceiveTick") == 0;
    const bool mannequinRow = std::strcmp(tag, "mannequinSpawner.ReceiveTick") == 0;
    static const bool creatureArm = coop::config::ResolveEnum(::coop::config_registry::rows::ambient_creature_red) == "no_rows";
    static const bool fieldArm = coop::config::ResolveEnum(::coop::config_registry::rows::ambient_field_red) == "no_rows";
    static const bool mannequinArm = coop::config::ResolveEnum(::coop::config_registry::rows::mannequin_drill) == "no_row";
    return (creatureArm && (creatureRow || fixtureRow)) || (fieldArm && fixtureRow) || (mannequinArm && mannequinRow);
}

}  // namespace

uint64_t RefusalCount(const char* tag) {
    for (int i = 0; i < kRowCount; ++i)
        if (std::strcmp(kRows[i].tag, tag) == 0) return g_refused[i];
    return 0;
}

// Refuse only while an active client session exists: the running flag flips true in the session
// start and false in the stop, which every disconnect path reaches; a bare role gate would bleed the
// refusal into single player after the session.
bool IsActiveClientSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->running() && s->role() == coop::net::Role::Client;
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    if (g_watched.exchange(true, std::memory_order_acq_rel)) return;
    int watched = 0;
    for (int i = 0; i < kRowCount; ++i) {
        if (LeftOutByRedArm(kRows[i].tag)) {
            UE_LOGI("spawn_authority: red arm -- the row %s is left out", kRows[i].tag);
            continue;
        }
        if (SG::WatchName(*kRows[i].row, kTagBase + i, kRows[i].pre ? kRows[i].pre : &OnRowPre, nullptr)) {
            ++watched;
        } else {
            UE_LOGE("spawn_authority: the watch on %ls::%ls did not register -- a client runs it",
                    kRows[i].row->Class(), kRows[i].row->Name());
        }
    }
    UE_LOGI("spawn_authority: %d/%d spawner watches registered by class and name, each live once the game "
            "thread resolves its names; refused only on an active client session", watched, kRowCount);
}

}  // namespace coop::spawn_authority

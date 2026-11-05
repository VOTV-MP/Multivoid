// coop/world/spawn_authority.cpp -- see the header. The rows rest on these bytecode facts: the
// mushroom master arms one looping spawn timer at begin-play, and its spawn mints spawner children
// with a lifespan, so refused children are reaped by the engine independently of the refused event;
// the mushroom spawner's own looping timer materialises the food cap and self-destroys, and with
// spawn refused the lifespan reaps it; the yellow-wisp ticker spawns at a navmesh random-walk point,
// not around the player, and its product is host-mirrored, so the client must not run its own
// spawner; the sky-wisp ticker spawns the sky wisps at absolute map coordinates, so the host rolls
// and clients mirror through the source-gated catch and the variant allowlist; the roach master's
// summon and its three looping timer entries fire independently of its tick. The tick rows: the
// insomniac and fossilhound tickers roll inside their tick with no delay chains or reap duties, so
// refusing the tick stops the roll and the product, and the products are host-mirrored; the roach
// master's tick drives roach movement, the food-eat mutation and crush traces, which a client running
// it would diverge, so it and its summoner are refused while the roach sync drives the client
// population. The jellyfish path's spawn makes seven fish inside its graph, and the host's are
// mirrored through the source-gated catch, so a client's own, its 18:00 roll's, is refused.

#include "coop/world/spawn_authority.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/session.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace coop::spawn_authority {
namespace {

namespace SG = ue_wrap::script_gate;

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
UE_FACT_FN(kRowSpawnBushTickerBushSpawning, "spawn_authority", L"ticker_bushSpawning_C", L"spawnBush", Declared);
UE_FACT_FN(kRowSphereHitMushroomSpore, "spawn_authority", L"mushroomSpore_C",
           L"BndEvt__mushroomSpore_Sphere_K2Node_ComponentBoundEvent_0_ComponentHitSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowTrySpawnHerbOutsideChurch, "spawn_authority", L"outsideChurch_C", L"trySpawnHerb", Declared);

// One row: the facts row a client must not run, and the tag its refusal log carries.
struct Row {
    const ue_wrap::facts::Function* row;
    const char* tag;
};
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
    // The three tickers roll in their tick; the beehive ticker's spawn is its looping timer's handler, refused as the
    // cockroach master's timer rows are.
    {&kRowReceiveTickTickerDeerSpawner,     "deerSpawner.ReceiveTick"},
    {&kRowReceiveTickTickerTreeSpawner,     "treeSpawner.ReceiveTick"},
    {&kRowReceiveTickTickerHexahiveSpawner, "hexahiveSpawner.ReceiveTick"},
    {&kRowSpawnTickerBeehiveSpawner,        "beehiveSpawner.spawn"},
    // The plant spawners: each product is a growingPlant_C, which the host's plant_sync sends as PlantState, so a
    // client's own would stand apart from the host's under another key. The winter bush ticker's timer entry plants
    // one bush on a random trace (ticker_bushSpawning UG @206, spawnBush). A spore's hit plants a shroom_7 at the hit
    // point on grass, dirt or gravel (mushroomSpore UG @29); the client's spores are the host's mirrors, and its own
    // hit would plant beside them. The church's trySpawnHerb rolls the stolas herb while it rains, from its begin play
    // and its tick (outsideChurch UG @2116).
    {&kRowSpawnBushTickerBushSpawning,    "bushSpawning.spawnBush"},
    {&kRowSphereHitMushroomSpore,         "mushroomSpore.SphereHit"},
    {&kRowTrySpawnHerbOutsideChurch,      "outsideChurch.trySpawnHerb"},
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

std::atomic<bool> g_watched{false};

// The drills' red arms leave rows out of the table, so a drill that reads the refusal counters must fail. The creature drill's
// arm leaves all four ambient rows out, the field drill's the hexahive's and the beehive's. Read once, at the install.
bool LeftOutByRedArm(const char* tag) {
    const bool fixtureRow = std::strcmp(tag, "hexahiveSpawner.ReceiveTick") == 0 || std::strcmp(tag, "beehiveSpawner.spawn") == 0;
    const bool creatureRow = std::strcmp(tag, "deerSpawner.ReceiveTick") == 0 || std::strcmp(tag, "treeSpawner.ReceiveTick") == 0;
    static const bool creatureArm = coop::config::ResolveEnum(::coop::config_registry::rows::ambient_creature_red) == "no_rows";
    static const bool fieldArm = coop::config::ResolveEnum(::coop::config_registry::rows::ambient_field_red) == "no_rows";
    return (creatureArm && (creatureRow || fixtureRow)) || (fieldArm && fixtureRow);
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
        if (SG::WatchName(*kRows[i].row, kTagBase + i, &OnRowPre, nullptr)) {
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

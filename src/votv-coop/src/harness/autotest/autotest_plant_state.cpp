// harness/autotest/autotest_plant_state.cpp -- TEST-ONLY: one state for every plant across two peers.
//
// A bush from its spawner's own body and a growingPlant_bush_C and a growingPlant_shroom_0_C (neither is in list_farmPlants,
// so the script spawns them) stand on the host before the client's checks. Then growth, a client's water, ripening, a
// client's harvest through the plant's own action (both on a plant the host plants beside the players: the spawner's bush
// stands out of the client's reach), death, a plant a client spawned, a new bush and two removals. The arms
// that hold what the client refuses (a sprinkler, the spawner, a pine's lethal hit, the growth area's glow, the plant tick)
// and the host's refusal of the plantings and edits a client crafts (the edit gate, term f1) are autotest_plant_gate.cpp's. The client tells
// the host what it reads; the host judges and logs `plant-state: VERDICT PASS|FAIL <terms>`. Gated by VOTVCOOP_RUN_PLANT_STATE;
// the peers' shared folder is VOTVCOOP_CAN_DIR. Every wait is on a state with a bound.

#include "harness/autotest.h"
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"
#include "plant_state_drill.h"   // co-located private header (src tree, not include/)

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/host_world_settle.h"
#include "coop/session/net_pump.h"
#include "coop/world/event_cue_sync.h"
#include "ue_wrap/actors/growing_plant.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace plant_drill {
namespace {
int g_fails = 0;
}

namespace GP = ue_wrap::growing_plant;
namespace R = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;

bool Check(bool ok, const char* what) {
    UE_LOGI("plant-state: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
    return ok;
}

int Fails() { return g_fails; }

std::string Narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }
std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

void* FirstOf(const wchar_t* cls) {
    void* found = nullptr;
    if (void* c = OI::ClassByName(cls))
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) *static_cast<void**>(ctx) = obj;
        }, &found);
    return found;
}

size_t CountOf(const wchar_t* cls) {
    size_t n = 0;
    if (void* c = OI::ClassByName(cls))
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) ++*static_cast<size_t*>(ctx);
        }, &n);
    return n;
}

void* ByKey(const std::string& key) {
    std::vector<void*> v;
    GP::List(v);
    const std::wstring want = Widen(key);
    for (void* a : v)
        if (GP::Key(a) == want) return a;
    return nullptr;
}

}  // namespace plant_drill

namespace {

using namespace plant_drill;
namespace E = ue_wrap::engine;
namespace PS = harness::autotest::peer_signal;
using PS::Await;
using PS::AwaitText;
using PS::Mark;

constexpr int kStep = 120;   // the bound on one step of the other peer's

// The classes under growingPlant_C that no pot or seed makes.
constexpr const wchar_t* kBushClass = L"growingPlant_bush_C";
constexpr const wchar_t* kBushPath = L"/Game/objects/growingPlant_bush.growingPlant_bush_C";
constexpr const wchar_t* kShroomClass = L"growingPlant_shroom_0_C";
constexpr const wchar_t* kShroomPath = L"/Game/objects/growingPlant_shroom_0.growingPlant_shroom_0_C";
constexpr const wchar_t* kPinePath = L"/Game/objects/growingPlant_pine.growingPlant_pine_C";
constexpr int kBerries = 3;   // a ripe bush's berry count, set where its row gives none

size_t PlantCount() {
    size_t n = 0;
    OnGameThread([&] { std::vector<void*> v; GP::List(v); n = v.size(); });
    return n;
}

// The spawner's own body, until its trace lands on a surface it takes. "" if it never does. The 20 ms between two rolls is
// pacing inside this peer's own step: each roll is a random trace and nothing signals which one lands.
std::string RollBush() {
    std::vector<void*> before;
    OnGameThread([&] { GP::List(before); });
    std::string key;
    for (int i = 0; i < 400 && key.empty(); ++i) {
        OnGameThread([&] {
            void* ticker = FirstOf(L"ticker_bushSpawning_C");
            void* fn = ticker ? R::FindDispatchFunctionCached(R::ClassOf(ticker), L"spawnBush") : nullptr;
            if (!fn) return;
            ue_wrap::ParamFrame f(fn);
            if (f.valid()) ue_wrap::Call(ticker, f);
            std::vector<void*> now;
            GP::List(now);
            for (void* a : now)
                if (std::find(before.begin(), before.end(), a) == before.end()) key = Narrow(GP::Key(a));
        });
        ::Sleep(20);
    }
    return key;
}

// A plant of `path`'s class by script, beside the host's player, with the class's own default row. "" when no plant of that class
// came of it.
std::string SpawnFamily(const wchar_t* path, const wchar_t* cls, float side) {
    std::string key;
    OnGameThread([&] {
        void* clsPtr = ue_wrap::asset_load::LoadObjectByPath(path);
        void* me = coop::players::Registry::Get().Local();
        void* cdo = R::FindClassDefaultObject(cls);
        ue_wrap::FVector at{};
        if (!me || !cdo || !E::TryGetActorLocation(me, at)) return;
        void* p = GP::Spawn(clsPtr, L"", GP::Row(cdo), L"", true, {at.X + 300.f, at.Y + side, at.Z - 80.f}, {});
        if (p && R::ClassNameOf(p) == cls) key = Narrow(GP::Key(p));
    });
    return key;
}

bool WaitGone(const std::string& key, int seconds = 15) {
    return PS::WaitFor("a plant's end", seconds, [&] {
        bool gone = false;
        OnGameThread([&] { gone = ByKey(key) == nullptr; });
        return gone;
    }, false);
}

// A plant of `row` by script, beside the host's player, of the class the game's own row plants (what a seed planted there would
// be). "" when the row, its class or the player's place is not there. The plant the client waters and harvests stands here, and
// not where the spawner's trace landed: the host judges every edit against the sender's reach.
std::string SpawnRowPlant(const std::string& row, float side) {
    std::string key;
    OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        void* cls = GP::FarmRowPlantClass(Widen(row));
        ue_wrap::FVector at{};
        if (!me || !cls || !E::TryGetActorLocation(me, at)) return;
        void* p = GP::Spawn(cls, L"", Widen(row), L"", true, {at.X + 300.f, at.Y + side, at.Z - 80.f}, {});
        if (p) key = Narrow(GP::Key(p));
    });
    return key;
}

// A negative `amount` leaves the plant's berry count as it is. False when the plant is not listed (yet) or cannot be read.
bool WriteHost(const std::string& key, float grow, float water, float fruit, int amount = -1) {
    bool wrote = false;
    OnGameThread([&] {
        void* p = ByKey(key);
        GP::Values v;
        if (!p || !GP::Read(p, v)) return;
        v.grow = grow;
        v.water = water;
        v.fruitGrow = fruit;
        if (amount >= 0) v.amount = amount;
        GP::Apply(p, GP::Row(p), v);
        wrote = true;
    });
    return wrote;
}

// WriteHost for a plant just spawned: the object index lists it once its birth drains, so the write waits for that.
bool WriteHostWhenListed(const std::string& key, float grow, float water, float fruit, int amount = -1) {
    return PS::WaitFor("the plant to be listed", 15, [&] { return WriteHost(key, grow, water, fruit, amount); }, false);
}

// The client's word at its end: "<fails> <plants here> <host family plants it holds> <tick> <sprinkler> <spawner> <pine> <glow>",
// each arm 1 when it held.
struct ClientWord {
    int fails = 1;   // text that is no count is a fail
    size_t plants = 0;
    int family = 0;
    int tick = 0, sprinkler = 0, spawner = 0, pine = 0, glow = 0;
};
ClientWord ParseWord(const std::string& text) {
    ClientWord w;
    std::istringstream in(text);
    if (!(in >> w.fails >> w.plants >> w.family >> w.tick >> w.sprinkler >> w.spawner >> w.pine >> w.glow)) w = ClientWord{};
    return w;
}

void RunHost() {
    if (!PS::WaitFor("the host's player", 240, [&] {
            void* pawn = nullptr;
            OnGameThread([&] { pawn = coop::players::Registry::Get().Local(); });
            return pawn != nullptr;
        }))
        return;
    // The save's plants are listed once the host's world has settled (the load's own removals are over: a drill that started
    // before it saw the game take its fresh plants away) and the object index holds every birth announced.
    if (!PS::WaitFor("the host's world settled and the object index caught up", 120, [&] {
            bool settled = false;
            OnGameThread([&] { settled = coop::host_world_settle::Settled() && OI::IsSeeded() && OI::Backlog() == 0; });
            return settled;
        }))
        return;
    UE_LOGI("plant-state: the save holds %zu plants", PlantCount());

    const std::string bush = RollBush();
    Check(!bush.empty(), "the bush spawner's own body made a plant");
    if (bush.empty()) return;
    const std::string familyBush = SpawnFamily(kBushPath, kBushClass, 150.f);
    const std::string familyShroom = SpawnFamily(kShroomPath, kShroomClass, -150.f);
    Check(!familyBush.empty(), "the script spawned a growingPlant_bush_C");
    Check(!familyShroom.empty(), "the script spawned a growingPlant_shroom_0_C");
    if (familyBush.empty() || familyShroom.empty()) return;
    std::string row;
    OnGameThread([&] { if (void* p = ByKey(bush)) row = Narrow(GP::Row(p)); });
    UE_LOGI("plant-state: bush '%hs' row '%hs', family '%hs' '%hs'", bush.c_str(), row.c_str(), familyBush.c_str(), familyShroom.c_str());
    const size_t atJoin = PlantCount();
    // The client reads the host's plants once it has joined and stands beside it: its signal, then the host's word.
    if (!Await("client_ready", 480)) return;
    Mark("bush", bush + " " + row + " " + std::to_string(atJoin) + " " + familyBush + " " + familyShroom);
    if (!Await("client_checked", kStep)) return;
    const size_t afterJoin = PlantCount();
    UE_LOGI("plant-state: plants at the join %zu, after it %zu", atJoin, afterJoin);
    Check(afterJoin == atJoin, "a join changed no plant on the host");

    // Growth: a slow change arrives within its gradual period.
    WriteHost(bush, 0.5f, 500.f, 0.f);
    Mark("grow");
    if (!Await("grow_done", kStep)) return;

    // A sprinkler beside the plant, switched off here: the client's copy must read no more water than the host's last
    // state gave it, with its own sprinkler switched on.
    Check(SpawnSprinklerBeside(bush), "the script spawned a sprinkler beside the host's plant");
    Mark("sprinkler");
    if (!Await("sprinkler_done", kStep)) return;

    // The plant the client waters and harvests: the drill's own, planted by script beside the met players, since the spawner's
    // bush stands wherever its trace landed and the host refuses an edit from beyond the sender's reach.
    const std::string drillPlant = SpawnRowPlant(row, 100.f);
    Check(!drillPlant.empty(), "the script planted the drill's own plant beside the host");
    if (drillPlant.empty()) return;
    Check(WriteHostWhenListed(drillPlant, 0.5f, 500.f, 0.f), "the drill's plant is listed and written");
    Mark("drill_plant", drillPlant);

    // A client's water.
    Check(WaitPlant(drillPlant, [](void*, const GP::Values& v) { return v.water > 5000.f; }), "the client's water reached the host's plant");
    Mark("water_seen");
    if (!Await("edit_done", kStep)) return;

    // Ripe, with berries: a bush shows amount, another plant makes fruit props. The harvest below is the client's, and a
    // plant without berries has none to take, so a bush that cannot be made to hold them fails the run.
    WriteHost(drillPlant, 1.f, 500.f, 1.f, kBerries);
    Check(WaitPlant(drillPlant, [](void*, const GP::Values& v) { return v.grow >= 1.f && v.fruitGrow >= 1.f && v.amount > 0; }),
          "the host's plant is ripe and bears berries");
    GP::Values ripe;
    OnGameThread([&] { if (void* p = ByKey(drillPlant)) GP::Read(p, ripe); });
    UE_LOGI("plant-state: ripe grow=%.2f fruit=%.2f amount=%d", ripe.grow, ripe.fruitGrow, ripe.amount);
    Mark("ripe", std::to_string(ripe.amount));
    if (!Await("ripe_here", kStep)) return;
    // Water the client has not heard of yet, as its slow change is still due: its harvest must not put back the old value.
    WriteHost(drillPlant, 1.f, 3000.f, 1.f);
    Mark("harvest_go");
    if (!Await("harvest_done", kStep)) return;
    if (ripe.amount > 0) {
        Check(WaitPlant(drillPlant, [](void*, const GP::Values& v) { return v.amount == 0; }), "the client's harvest emptied the host's plant");
        GP::Values after;
        OnGameThread([&] { if (void* p = ByKey(drillPlant)) GP::Read(p, after); });
        UE_LOGI("plant-state: host water after the harvest %.0f", after.water);
        Check(after.water > 2000.f, "the harvest left the host's newer water alone");
    }
    size_t foodsHost = 0, propsHost = 0;
    OnGameThread([&] { foodsHost = CountOf(L"prop_food_C"); propsHost = CountOf(L"prop_C"); });
    UE_LOGI("plant-state: host item counts after the harvest food=%zu prop=%zu", foodsHost, propsHost);
    Mark("harvest_seen");

    // A plant this client spawned stands on the host under the same key, and the host runs it.
    std::string planted;
    if (!AwaitText("planted", planted, kStep)) return;
    bool stands = false;
    PS::WaitFor("the client's plant", 10, [&] {
        OnGameThread([&] { stands = ByKey(planted) != nullptr; });
        return stands;
    }, false);
    Check(stands, "the plant a client planted stands on the host under its key");
    if (stands) {
        WriteHost(planted, 0.25f, 500.f, 0.f);
        Mark("planted_seen");
    }
    if (!Await("planted_done", kStep)) return;

    // Death: the row changes on the host and its copy takes it.
    std::string deadRow;
    OnGameThread([&] {
        if (void* p = ByKey(bush)) {
            if (void* fn = R::FindDispatchFunctionCached(R::ClassOf(p), L"turnDead")) {
                ue_wrap::ParamFrame f(fn);
                if (f.valid()) ue_wrap::Call(p, f);
            }
            deadRow = Narrow(GP::Row(p));
        }
    });
    UE_LOGI("plant-state: the bush died, row '%hs'", deadRow.c_str());
    Mark("dead", deadRow);
    if (!Await("dead_done", kStep)) return;

    // A new bush while the client is in, then the host's end of it.
    const std::string second = RollBush();
    Check(!second.empty(), "a second bush stands");
    Mark("second", second);
    if (!Await("second_seen", kStep)) return;
    OnGameThread([&] { if (void* p = ByKey(second)) GP::Destroy(p); });
    Mark("second_gone");
    if (!Await("removal", kStep)) return;
    Check(WaitGone(planted), "the plant the client removed left the host");
    Mark("removal_seen");

    // A pine: the client's lethal hit on its copy is refused, the host's own breaks it, and the end crosses.
    const std::string pine = SpawnFamily(kPinePath, kPineClass, 250.f);
    Check(!pine.empty(), "the script spawned a growingPlant_pine_C");
    if (pine.empty()) return;
    Mark("pine", pine);
    if (!Await("pine_hit", kStep)) return;
    Check(LethalHit(pine), "the host's lethal hit on the pine was delivered");
    Check(WaitGone(pine), "the host's lethal hit broke the pine");
    Mark("pine_broke");

    // A growth area over a watered plant: its glow is the host's alone to spawn (the client's tick is refused), and
    // crosses as a cue. The client's count is taken before the host's first tick.
    const std::string glowPlant = SpawnFamily(kBushPath, kBushClass, 500.f);
    Check(!glowPlant.empty(), "the script spawned a plant for the growth area");
    if (glowPlant.empty()) return;
    // The area acts only on a plant with water and no dryness (growthArea's tick, before it fertilises): watered here.
    const bool watered = WriteHostWhenListed(glowPlant, 0.5f, 600.f, 0.f);
    GP::Values glowValues;
    OnGameThread([&] { if (void* p = ByKey(glowPlant)) GP::Read(p, glowValues); });
    UE_LOGI("plant-state: the growth area's plant has water %.0f dryness %.1f", glowValues.water, glowValues.dryness);
    Check(watered && glowValues.water > 0.f && glowValues.dryness >= 0.f, "the growth area's plant reads watered and not dry");
    void* area = SpawnGrowthArea(glowPlant);
    Check(area != nullptr, "the script spawned a growthArea_C");
    Mark("glow_set");
    if (!Await("glow_base", kStep)) return;
    const std::string glowAt = area ? TickGrowthArea(area) : "none";
    Mark("glow", glowAt);
    if (!Await("glow_done", kStep)) return;

    // Plantings and edits no player's act could make, crafted by the client: the host's edit gate refuses or cuts each. The pot
    // the client names stands where no reach allows.
    const CraftBase craftBase = CraftBegin(drillPlant);
    Mark("craft_go", (craftBase.potKey.empty() ? std::string("-") : craftBase.potKey) + " " + drillPlant);
    if (!Await("craft_sent", kStep)) return;
    const int f1 = CraftJudge(craftBase) ? 1 : 0;

    std::string clientText;
    if (!AwaitText("client_done", clientText, kStep)) return;
    const ClientWord client = ParseWord(clientText);
    size_t hostPlants = PlantCount();
    PS::WaitFor("the plant counts to agree", 15, [&] { return (hostPlants = PlantCount()) == client.plants; }, false);
    const bool pass = Fails() == 0 && client.fails == 0 && client.plants == hostPlants && client.family == 2 &&
                      client.tick == 1 && client.sprinkler == 1 && client.spawner == 1 && client.pine == 1 && client.glow == 1 &&
                      f1 == 1;
    ue_wrap::log::Write(pass ? ue_wrap::log::Level::Info : ue_wrap::log::Level::Warn,
                        "plant-state: VERDICT %hs host=%d client=%d plants=%zu/%zu family=%d tick=%d sprinkler=%d spawner=%d "
                        "pine=%d glow=%d f1=%d",
                        pass ? "PASS" : "FAIL", Fails(), client.fails, hostPlants, client.plants, client.family,
                        client.tick, client.sprinkler, client.spawner, client.pine, client.glow, f1);
    UE_LOGI("plant-state: DONE on host fails=%d", Fails());
}

// The world is up and the host's puppet exists here.
bool HostPuppetUp() {
    if (!PS::WaitFor("the client's world", 240, [&] {
            bool ready = false;
            OnGameThread([&] { ready = coop::players::Registry::Get().Local() && coop::net_pump::HasAnnouncedWorldReady(); });
            return ready;
        }))
        return false;
    return PS::WaitFor("the host's puppet", 60, [&] {
        bool have = false;
        OnGameThread([&] {
            coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
            have = p && p->valid();
        });
        return have;
    });
}

void RunClient() {
    if (!HostPuppetUp()) return;
    if (!meet::WalkToHost("plant-state")) return;
    Mark("client_ready");
    std::string body;
    if (!AwaitText("bush", body, kStep)) return;
    std::istringstream in(body);
    std::string bush, row, familyBush, familyShroom;
    size_t hostPlants = 0;
    in >> bush >> row >> hostPlants >> familyBush >> familyShroom;
    bool same = WaitPlant(bush, [&](void* p, const GP::Values&) { return Narrow(GP::Row(p)) == row; });
    Check(same, "a joiner has the bush from before its join, with its row");
    int family = 0;
    const bool bushHere = WaitPlant(familyBush, [](void* p, const GP::Values&) { return R::ClassNameOf(p) == kBushClass; });
    Check(bushHere, "the host's growingPlant_bush_C stands here under its key");
    family += bushHere ? 1 : 0;
    const bool shroomHere = WaitPlant(familyShroom, [](void* p, const GP::Values&) { return R::ClassNameOf(p) == kShroomClass; });
    Check(shroomHere, "the host's growingPlant_shroom_0_C stands here under its key");
    family += shroomHere ? 1 : 0;
    const size_t herePlants = PlantCount();
    UE_LOGI("plant-state: plants here %zu, host %zu", herePlants, hostPlants);
    Check(herePlants == hostPlants, "this client has the host's plants, no more and no fewer");
    Mark("client_checked");

    if (!Await("grow", kStep)) return;
    Check(WaitPlant(bush, [](void*, const GP::Values& v) { return Near(v.grow, 0.5f, 0.05f) && v.water > 400.f; }), "the host's growth and water arrived");
    Mark("grow_done");

    if (!Await("sprinkler", kStep)) return;
    const int sprinklerOk = SprinklerArm(bush) ? 1 : 0;
    Mark("sprinkler_done");

    // The host's own plant beside the players, by the key it names.
    if (!AwaitText("drill_plant", body, kStep)) return;
    const std::string drillPlant = body;
    Check(WaitPlant(drillPlant, [](void*, const GP::Values& v) { return Near(v.grow, 0.5f, 0.05f) && v.water > 400.f; }),
          "the host's plant beside this player arrived");

    // This player's water, as a can or a toolgun writes it.
    OnGameThread([&] {
        void* p = ByKey(drillPlant);
        GP::Values v;
        if (p && GP::Read(p, v)) {
            v.water = 7000.f;
            GP::Apply(p, GP::Row(p), v);
        }
    });
    if (!Await("water_seen", kStep)) return;
    Mark("edit_done");

    if (!AwaitText("ripe", body, kStep)) return;
    const int hostAmount = std::atoi(body.c_str());
    Check(WaitPlant(drillPlant, [&](void*, const GP::Values& v) { return v.grow >= 1.f && v.amount == hostAmount; }), "the ripe plant reads the same here");
    Mark("ripe_here");
    if (!Await("harvest_go", kStep)) return;
    // A bush the host could not make bear berries has nothing to harvest: that is the run's failure, not a step to skip.
    if (Check(hostAmount > 0, "the host's ripe bush bears berries, so there is a harvest to run")) {
        size_t foodsBefore = 0, propsBefore = 0, foodsAfter = 0, propsAfter = 0;
        OnGameThread([&] {
            foodsBefore = CountOf(L"prop_food_C");
            propsBefore = CountOf(L"prop_C");
            void* p = ByKey(drillPlant);
            void* fn = p ? R::FindDispatchFunctionCached(R::ClassOf(p), L"actionOptionIndex") : nullptr;
            if (!fn) return;
            ue_wrap::ParamFrame f(fn);
            if (!f.valid()) return;
            f.Set<void*>(L"player", coop::players::Registry::Get().Local());
            f.Set<uint8_t>(L"action", 4);
            ue_wrap::Call(p, f);
        });
        Check(WaitPlant(drillPlant, [](void*, const GP::Values& v) { return v.amount == 0; }), "this player's harvest emptied this copy");
        OnGameThread([&] { foodsAfter = CountOf(L"prop_food_C"); propsAfter = CountOf(L"prop_C"); });
        UE_LOGI("plant-state: client item counts food %zu->%zu prop %zu->%zu", foodsBefore, foodsAfter, propsBefore, propsAfter);
    }
    Mark("harvest_done");
    if (!Await("harvest_seen", kStep)) return;

    // A plant of this player's, as a pot or a seed makes one: of the class its row plants, which is what the host plants for it.
    std::string planted;
    OnGameThread([&] {
        void* pawn = coop::players::Registry::Get().Local();
        ue_wrap::FVector me{};
        if (!pawn || !E::TryGetActorLocation(pawn, me)) return;
        void* rowClass = GP::FarmRowPlantClass(Widen(row));
        void* p = GP::Spawn(rowClass, L"", Widen(row), L"", true, {me.X + 300.f, me.Y, me.Z - 80.f}, {});
        if (p) planted = Narrow(GP::Key(p));
    });
    Check(!planted.empty(), "this player's plant stands");
    if (planted.empty()) return;
    UE_LOGI("plant-state: planted '%hs'", planted.c_str());
    Mark("planted", planted);
    if (!Await("planted_seen", kStep)) return;
    Check(WaitPlant(planted, [](void*, const GP::Values& v) { return Near(v.grow, 0.25f, 0.05f); }), "the host's state of this player's plant arrived");
    Mark("planted_done");

    if (!AwaitText("dead", body, kStep)) return;
    Check(WaitPlant(bush, [&](void* p, const GP::Values& v) { return Narrow(GP::Row(p)) == body && v.dryness < 0.f; }), "the dead plant's row and state arrived");
    Mark("dead_done");

    if (!AwaitText("second", body, kStep)) return;
    Check(WaitPlant(body, [](void*, const GP::Values&) { return true; }), "the new bush stands here");
    const int spawnerOk = SpawnerArm() ? 1 : 0;
    Mark("second_seen");
    const std::string second = body;
    if (!Await("second_gone", kStep)) return;
    Check(WaitGone(second), "the host's removal ended it here");

    OnGameThread([&] { if (void* p = ByKey(planted)) GP::Destroy(p); });
    Mark("removal");
    if (!Await("removal_seen", kStep)) return;

    if (!AwaitText("pine", body, kStep)) return;
    const std::string pine = body;
    int pineOk = PineHitArm(pine) ? 1 : 0;
    Mark("pine_hit");
    if (!Await("pine_broke", kStep)) return;
    pineOk &= Check(WaitGone(pine), "the host's break ended the pine here") ? 1 : 0;

    if (!Await("glow_set", kStep)) return;
    const unsigned glowBase = coop::event_cue_sync::ReplayCount();
    Mark("glow_base");
    if (!AwaitText("glow", body, kStep)) return;
    const int glowOk = GlowArm(glowBase, body) ? 1 : 0;
    Mark("glow_done");

    if (!AwaitText("craft_go", body, kStep)) return;
    std::istringstream craftIn(body);
    std::string potKey, plantKey;
    craftIn >> potKey >> plantKey;
    CraftArm(row, potKey, plantKey);
    Mark("craft_sent");

    const int tickOk = TickArm() ? 1 : 0;
    Mark("client_done", std::to_string(Fails()) + " " + std::to_string(PlantCount()) + " " + std::to_string(family) + " " +
                            std::to_string(tickOk) + " " + std::to_string(sprinklerOk) + " " + std::to_string(spawnerOk) +
                            " " + std::to_string(pineOk) + " " + std::to_string(glowOk));
    UE_LOGI("plant-state: DONE on client fails=%d", Fails());
}

}  // namespace

DWORD WINAPI PlantStateThread(LPVOID /*arg*/) {
    if (!PS::Begin("plant-state")) return 0;
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest

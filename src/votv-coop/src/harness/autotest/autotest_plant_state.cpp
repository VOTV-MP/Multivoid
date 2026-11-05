// harness/autotest/autotest_plant_state.cpp -- TEST-ONLY: one state for every plant across two peers.
//
// A bush from its spawner's own body and a growingPlant_bush_C and a growingPlant_shroom_0_C (neither is in list_farmPlants,
// so the script spawns them) stand on the host before the client's checks. Then growth, a client's water, ripening, a
// client's harvest through the plant's own action, death, a plant a client spawned, a new bush and two removals. Three arms
// hold what the client refuses (coop/world/plant_gate, coop/world/spawn_authority): a sprinkler the host spawned beside the
// plant, switched on here, adds no water to the client's copy; the client's own spawnBush calls are refused and plant
// nothing; a lethal hit on a pine here is refused and the copy ends with the host's break; and at the end no plant tick
// ran here while the gate refused them. The client tells the host what it reads; the
// host judges and logs `plant-state: VERDICT PASS|FAIL <terms>`. Gated by VOTVCOOP_RUN_PLANT_STATE; the peers' shared
// folder is VOTVCOOP_CAN_DIR. Every wait is on a state with a bound.

#include "harness/autotest.h"
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"

#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_damage_gate.h"
#include "coop/session/net_pump.h"
#include "coop/world/plant_gate.h"
#include "coop/world/spawn_authority.h"
#include "ue_wrap/actors/growing_plant.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace GP = ue_wrap::growing_plant;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace PS = harness::autotest::peer_signal;
using PS::Await;
using PS::AwaitText;
using PS::Mark;

constexpr int kStep = 120;   // the bound on one step of the other peer's

// The two classes under growingPlant_C that no pot or seed makes.
constexpr const wchar_t* kBushClass = L"growingPlant_bush_C";
constexpr const wchar_t* kBushPath = L"/Game/objects/growingPlant_bush.growingPlant_bush_C";
constexpr const wchar_t* kShroomClass = L"growingPlant_shroom_0_C";
constexpr const wchar_t* kShroomPath = L"/Game/objects/growingPlant_shroom_0.growingPlant_shroom_0_C";
constexpr const wchar_t* kPineClass = L"growingPlant_pine_C";
constexpr const wchar_t* kPinePath = L"/Game/objects/growingPlant_pine.growingPlant_pine_C";
constexpr const wchar_t* kSprinklerClass = L"prop_sprinkler_C";
constexpr const wchar_t* kSprinklerPath = L"/Game/objects/prop_sprinkler.prop_sprinkler_C";
constexpr const char* kBushSpawnTag = "bushSpawning.spawnBush";   // spawn_authority's row of the winter bush ticker

int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

bool Check(bool ok, const char* what) {
    UE_LOGI("plant-state: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
    return ok;
}

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
        ue_wrap::asset_load::LoadObjectByPath(path);
        void* me = coop::players::Registry::Get().Local();
        void* cdo = R::FindClassDefaultObject(cls);
        ue_wrap::FVector at{};
        if (!me || !cdo || !E::TryGetActorLocation(me, at)) return;
        void* p = GP::Spawn(cls, L"", GP::Row(cdo), L"", true, {at.X + 300.f, at.Y + side, at.Z - 80.f}, {});
        if (p && R::ClassNameOf(p) == cls) key = Narrow(GP::Key(p));
    });
    return key;
}

bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// Waits for a plant here to meet a test; a miss is the caller's Check, not an abort.
template <class Test>
bool WaitPlant(const std::string& key, Test&& test, int seconds = 15) {
    return PS::WaitFor("a plant's state", seconds, [&] {
        bool ok = false;
        OnGameThread([&] {
            void* p = ByKey(key);
            GP::Values v;
            if (p && GP::Read(p, v)) ok = test(p, v);
        });
        return ok;
    }, false);
}

bool WaitGone(const std::string& key, int seconds = 15) {
    return PS::WaitFor("a plant's end", seconds, [&] {
        bool gone = false;
        OnGameThread([&] { gone = ByKey(key) == nullptr; });
        return gone;
    }, false);
}

void WriteHost(const std::string& key, float grow, float water, float fruit) {
    OnGameThread([&] {
        void* p = ByKey(key);
        GP::Values v;
        if (!p || !GP::Read(p, v)) return;
        v.grow = grow;
        v.water = water;
        v.fruitGrow = fruit;
        GP::Apply(p, GP::Row(p), v);
    });
}

// The host's sprinkler: spawned beside the plant and left switched off, so the plant's water on the host holds its value.
// The host's own timer would add to it; the client's is the one under test.
bool SpawnSprinklerBeside(const std::string& plantKey) {
    bool made = false;
    OnGameThread([&] {
        ue_wrap::asset_load::LoadObjectByPath(kSprinklerPath);
        void* cls = R::FindClass(kSprinklerClass);
        void* plant = ByKey(plantKey);
        ue_wrap::FVector at{};
        if (!cls || !plant || !E::TryGetActorLocation(plant, at)) return;
        at.X += 60.f;
        at.Z += 40.f;
        void* s = E::BeginDeferredSpawn(cls, at, ue_wrap::FRotator{});
        made = s && E::FinishDeferredSpawn(s, at, ue_wrap::FRotator{});
    });
    return made;
}

// Every sprinkler on this peer switched on or off: `active` is what the watering timer reads first, and the game sets it
// from a hose.
void SetSprinklersActive(bool on) {
    void* cls = OI::ClassByName(kSprinklerClass);
    if (!cls) return;
    OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) {
        if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        int32_t off = -1;
        uint8_t mask = 0;
        if (!R::FindBoolProperty(R::ClassOf(obj), L"active", off, mask)) return;
        uint8_t& b = static_cast<uint8_t*>(obj)[off];
        b = *static_cast<bool*>(ctx) ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
    }, &on);
}

// CLIENT: a sprinkler switched on beside a plant whose host water holds. Its 0.1 s watering timer is refused here, so the
// copy never rises above the host's value. The window is 30 refused firings of that timer, not a time.
bool SprinklerArm(const std::string& plantKey, float hostWater) {
    constexpr uint64_t kFirings = 30;
    const bool here = PS::WaitFor("a sprinkler here", 20, [&] {
        size_t n = 0;
        OnGameThread([&] { n = CountOf(kSprinklerClass); });
        return n > 0;
    }, false);
    if (!Check(here, "the sprinkler the host spawned stands here")) return false;
    uint64_t before = 0, refused = 0;
    OnGameThread([&] {
        SetSprinklersActive(true);
        before = coop::plant_gate::GetCounts().sprinklerWater;
    });
    float peak = 0.f;
    const bool fired = PS::WaitFor("the sprinkler's timer", 20, [&] {
        OnGameThread([&] {
            refused = coop::plant_gate::GetCounts().sprinklerWater - before;
            void* p = ByKey(plantKey);
            GP::Values v;
            if (p && GP::Read(p, v)) peak = (std::max)(peak, v.water);
        });
        return refused >= kFirings;
    }, false);
    OnGameThread([&] { SetSprinklersActive(false); });
    UE_LOGI("plant-state: sprinkler arm refused %llu timer firings, plant water peak %.1f, host water %.1f",
            static_cast<unsigned long long>(refused), peak, hostWater);
    const bool timer = Check(fired, "this client refused the sprinkler's watering timer");
    const bool held = Check(peak <= hostWater + 1.f, "a switched-on sprinkler added no water to this client's copy");
    return timer && held;
}

// CLIENT: the winter ticker's own body, called here, is refused each time and plants nothing.
bool SpawnerArm() {
    constexpr int kCalls = 20;
    std::vector<void*> before;
    int calls = 0;
    uint64_t refused = 0;
    size_t born = 0;
    OnGameThread([&] {
        GP::List(before);
        const uint64_t was = coop::spawn_authority::RefusalCount(kBushSpawnTag);
        void* ticker = FirstOf(L"ticker_bushSpawning_C");
        void* fn = ticker ? R::FindDispatchFunctionCached(R::ClassOf(ticker), L"spawnBush") : nullptr;
        for (int i = 0; fn && i < kCalls; ++i) {
            ue_wrap::ParamFrame f(fn);
            if (f.valid() && ue_wrap::Call(ticker, f)) ++calls;
        }
        refused = coop::spawn_authority::RefusalCount(kBushSpawnTag) - was;
        std::vector<void*> now;
        GP::List(now);
        for (void* a : now)
            if (std::find(before.begin(), before.end(), a) == before.end()) ++born;
    });
    UE_LOGI("plant-state: spawner arm made %d calls, %llu refused, %zu plants born", calls,
            static_cast<unsigned long long>(refused), born);
    const bool all = Check(calls == kCalls && refused == static_cast<uint64_t>(calls), "every spawnBush call here was refused");
    const bool none = Check(born == 0, "no bush was planted here by this client's own spawner");
    return all && none;
}

// CLIENT: by the end the gate has refused plant ticks, and none of a plant's own ran.
bool TickArm() {
    coop::plant_gate::Counts c;
    OnGameThread([&] { c = coop::plant_gate::GetCounts(); });
    UE_LOGI("plant-state: tick arm refused plant ticks %llu, ran %llu; herb grown %llu, sprinkler %llu, growth area %llu",
            static_cast<unsigned long long>(c.plantTick), static_cast<unsigned long long>(c.plantTickRan),
            static_cast<unsigned long long>(c.herbGrown), static_cast<unsigned long long>(c.sprinklerWater),
            static_cast<unsigned long long>(c.growthAreaTick));
    return Check(c.plantTick > 0 && c.plantTickRan == 0, "this client ran no plant tick of its own while the gate refused them");
}

// addDamage on a plant with damage no health survives: the plant's own entry, which hands the call to its
// comp_physicsImpact_C, the component that keeps the health and breaks its owner.
bool LethalHit(const std::string& key) {
    bool called = false;
    OnGameThread([&] {
        void* p = ByKey(key);
        void* fn = p ? R::FindDispatchFunctionCached(R::ClassOf(p), L"addDamage") : nullptr;
        ue_wrap::ParamFrame f(fn);
        if (!fn || !f.valid()) return;
        f.Set<float>(L"damage", 1.0e7f);
        called = ue_wrap::Call(p, f);
    });
    return called;
}

// CLIENT: a lethal hit on the host's pine is refused here (the plant is the host's), so the copy stands and spawns no logs
// of its own until the host's break ends it.
bool PineHitArm(const std::string& key) {
    const bool here = Check(WaitPlant(key, [](void* p, const GP::Values&) { return R::ClassNameOf(p) == kPineClass; }),
                            "the host's growingPlant_pine_C stands here under its key");
    if (!here) return false;
    unsigned long long refusedWas = 0, refusedNow = 0;
    size_t logsWas = 0, logsNow = 0;
    bool stands = false;
    OnGameThread([&] {
        refusedWas = coop::props::prop_damage_gate::GetCounts().plant;
        logsWas = CountOf(L"prop_food_C");
    });
    const bool called = LethalHit(key);
    OnGameThread([&] {
        refusedNow = coop::props::prop_damage_gate::GetCounts().plant;
        logsNow = CountOf(L"prop_food_C");
        stands = ByKey(key) != nullptr;
    });
    UE_LOGI("plant-state: pine hit called=%d, plant refusals %llu->%llu, logs %zu->%zu, copy stands=%d", called ? 1 : 0,
            refusedWas, refusedNow, logsWas, logsNow, stands ? 1 : 0);
    const bool refused = Check(called && refusedNow > refusedWas, "this client's lethal hit on the pine was refused");
    const bool intact = Check(stands && logsNow == logsWas, "this client's pine copy stood and spawned no logs of its own");
    return refused && intact;
}

// The client's word at its end: "<fails> <plants here> <host family plants it holds> <tick> <sprinkler> <spawner> <pine>",
// each arm 1 when it held.
struct ClientWord {
    int fails = 1;   // text that is no count is a fail
    size_t plants = 0;
    int family = 0;
    int tick = 0, sprinkler = 0, spawner = 0, pine = 0;
};
ClientWord ParseWord(const std::string& text) {
    ClientWord w;
    std::istringstream in(text);
    if (!(in >> w.fails >> w.plants >> w.family >> w.tick >> w.sprinkler >> w.spawner >> w.pine)) w = ClientWord{};
    return w;
}

void RunHost() {
    if (!PS::WaitFor("the host's player", 240, [&] {
            void* pawn = nullptr;
            OnGameThread([&] { pawn = coop::players::Registry::Get().Local(); });
            return pawn != nullptr;
        }))
        return;
    // The save's plants are listed once the object index holds every birth the load announced.
    if (!PS::WaitFor("the object index settled", 120, [&] {
            bool settled = false;
            OnGameThread([&] { settled = OI::IsSeeded() && OI::Backlog() == 0; });
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

    // A sprinkler beside the plant, switched off here: the host's water holds at the value it was written, and the
    // client's copy must read no more than that with its own sprinkler switched on.
    Check(SpawnSprinklerBeside(bush), "the script spawned a sprinkler beside the host's plant");
    GP::Values held;
    OnGameThread([&] { if (void* p = ByKey(bush)) GP::Read(p, held); });
    Mark("sprinkler", std::to_string(held.water));
    if (!Await("sprinkler_done", kStep)) return;

    // A client's water.
    Check(WaitPlant(bush, [](void*, const GP::Values& v) { return v.water > 5000.f; }), "the client's water reached the host's plant");
    Mark("water_seen");
    if (!Await("edit_done", kStep)) return;

    // Ripe: a bush shows amount, another plant makes fruit props.
    WriteHost(bush, 1.f, 500.f, 1.f);
    Check(WaitPlant(bush, [](void*, const GP::Values& v) { return v.grow >= 1.f && v.fruitGrow >= 1.f; }), "the host's plant is ripe");
    GP::Values ripe;
    OnGameThread([&] { if (void* p = ByKey(bush)) GP::Read(p, ripe); });
    UE_LOGI("plant-state: ripe grow=%.2f fruit=%.2f amount=%d", ripe.grow, ripe.fruitGrow, ripe.amount);
    Mark("ripe", std::to_string(ripe.amount));
    if (!Await("ripe_here", kStep)) return;
    // Water the client has not heard of yet, as its slow change is still due: its harvest must not put back the old value.
    WriteHost(bush, 1.f, 3000.f, 1.f);
    Mark("harvest_go");
    if (!Await("harvest_done", kStep)) return;
    if (ripe.amount > 0) {
        Check(WaitPlant(bush, [](void*, const GP::Values& v) { return v.amount == 0; }), "the client's harvest emptied the host's plant");
        GP::Values after;
        OnGameThread([&] { if (void* p = ByKey(bush)) GP::Read(p, after); });
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

    std::string clientText;
    if (!AwaitText("client_done", clientText, kStep)) return;
    const ClientWord client = ParseWord(clientText);
    size_t hostPlants = PlantCount();
    PS::WaitFor("the plant counts to agree", 15, [&] { return (hostPlants = PlantCount()) == client.plants; }, false);
    const bool pass = g_fails == 0 && client.fails == 0 && client.plants == hostPlants && client.family == 2 &&
                      client.tick == 1 && client.sprinkler == 1 && client.spawner == 1 && client.pine == 1;
    ue_wrap::log::Write(pass ? ue_wrap::log::Level::Info : ue_wrap::log::Level::Warn,
                        "plant-state: VERDICT %hs host=%d client=%d plants=%zu/%zu family=%d tick=%d sprinkler=%d spawner=%d "
                        "pine=%d",
                        pass ? "PASS" : "FAIL", g_fails, client.fails, hostPlants, client.plants, client.family,
                        client.tick, client.sprinkler, client.spawner, client.pine);
    UE_LOGI("plant-state: DONE on host fails=%d", g_fails);
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

    if (!AwaitText("sprinkler", body, kStep)) return;
    const int sprinklerOk = SprinklerArm(bush, static_cast<float>(std::atof(body.c_str()))) ? 1 : 0;
    Mark("sprinkler_done");

    // This player's water, as a can or a toolgun writes it.
    OnGameThread([&] {
        void* p = ByKey(bush);
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
    Check(WaitPlant(bush, [&](void*, const GP::Values& v) { return v.grow >= 1.f && v.amount == hostAmount; }), "the ripe plant reads the same here");
    Mark("ripe_here");
    if (!Await("harvest_go", kStep)) return;
    if (hostAmount > 0) {
        size_t foodsBefore = 0, propsBefore = 0, foodsAfter = 0, propsAfter = 0;
        OnGameThread([&] {
            foodsBefore = CountOf(L"prop_food_C");
            propsBefore = CountOf(L"prop_C");
            void* p = ByKey(bush);
            void* fn = p ? R::FindDispatchFunctionCached(R::ClassOf(p), L"actionOptionIndex") : nullptr;
            if (!fn) return;
            ue_wrap::ParamFrame f(fn);
            if (!f.valid()) return;
            f.Set<void*>(L"player", coop::players::Registry::Get().Local());
            f.Set<uint8_t>(L"action", 4);
            ue_wrap::Call(p, f);
        });
        Check(WaitPlant(bush, [](void*, const GP::Values& v) { return v.amount == 0; }), "this player's harvest emptied this copy");
        OnGameThread([&] { foodsAfter = CountOf(L"prop_food_C"); propsAfter = CountOf(L"prop_C"); });
        UE_LOGI("plant-state: client item counts food %zu->%zu prop %zu->%zu", foodsBefore, foodsAfter, propsBefore, propsAfter);
    } else {
        UE_LOGI("plant-state: this row ripens no berries, so the harvest step is not run");
    }
    Mark("harvest_done");
    if (!Await("harvest_seen", kStep)) return;

    // A plant of this player's, as a pot or a seed makes one.
    std::string planted;
    OnGameThread([&] {
        void* pawn = coop::players::Registry::Get().Local();
        ue_wrap::FVector me{};
        if (!pawn || !E::TryGetActorLocation(pawn, me)) return;
        void* p = GP::Spawn(L"growingPlant_C", L"", Widen(row), L"", true, {me.X + 300.f, me.Y, me.Z - 80.f}, {});
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

    const int tickOk = TickArm() ? 1 : 0;
    Mark("client_done", std::to_string(g_fails) + " " + std::to_string(PlantCount()) + " " + std::to_string(family) + " " +
                            std::to_string(tickOk) + " " + std::to_string(sprinklerOk) + " " + std::to_string(spawnerOk) +
                            " " + std::to_string(pineOk));
    UE_LOGI("plant-state: DONE on client fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI PlantStateThread(LPVOID /*arg*/) {
    if (!PS::Begin("plant-state")) return 0;
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest

// harness/autotest/autotest_plant_state.cpp -- TEST-ONLY: one state for every plant across two peers.
//
// A bush stands before the client joins, from its spawner's own body. Then growth, a client's water, ripening, a client's harvest
// through the plant's own action, death, a plant a client spawned, a new bush and two removals. Gated by
// VOTVCOOP_RUN_PLANT_STATE; the folder is VOTVCOOP_CAN_DIR.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/growing_plant.h"
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
#include <fstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace GP = ue_wrap::growing_plant;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;

std::string g_dir;
int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Mark(const char* name, const std::string& body = "x") {
    std::string tmp = g_dir + "\\" + name + ".tmp";
    { std::ofstream(tmp, std::ios::binary | std::ios::trunc) << body; }
    ::MoveFileExA(tmp.c_str(), (g_dir + "\\" + name).c_str(), MOVEFILE_REPLACE_EXISTING);
}

bool Await(const char* name, std::string* body = nullptr, int seconds = 300) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) {
            if (body) *body = s;
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("plant-state: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void Check(bool ok, const char* what) {
    UE_LOGI("plant-state: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
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

// The spawner's own body, until its trace lands on a surface it takes. "" if it never does.
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

bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// Waits for a plant here to meet a test.
template <class Test>
bool WaitFor(const std::string& key, Test&& test, int seconds = 15) {
    for (int i = 0; i < seconds * 10; ++i) {
        bool ok = false;
        OnGameThread([&] {
            void* p = ByKey(key);
            GP::Values v;
            if (p && GP::Read(p, v)) ok = test(p, v);
        });
        if (ok) return true;
        ::Sleep(100);
    }
    return false;
}

bool WaitGone(const std::string& key, int seconds = 15) {
    for (int i = 0; i < seconds * 10; ++i) {
        bool gone = false;
        OnGameThread([&] { gone = ByKey(key) == nullptr; });
        if (gone) return true;
        ::Sleep(100);
    }
    return false;
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

void RunHost() {
    void* pawn = nullptr;
    for (int i = 0; i < 480 && !pawn; ++i) {
        OnGameThread([&] { pawn = coop::players::Registry::Get().Local(); });
        if (!pawn) ::Sleep(500);
    }
    if (!pawn) return;
    ::Sleep(4000);
    std::vector<void*> have;
    OnGameThread([&] { GP::List(have); });
    UE_LOGI("plant-state: the save holds %zu plants", have.size());

    const std::string bush = RollBush();
    Check(!bush.empty(), "the bush spawner's own body made a plant");
    if (bush.empty()) return;
    std::string row;
    OnGameThread([&] { if (void* p = ByKey(bush)) row = Narrow(GP::Row(p)); });
    UE_LOGI("plant-state: bush '%hs' row '%hs'", bush.c_str(), row.c_str());
    size_t atJoin = 0;
    OnGameThread([&] { std::vector<void*> v; GP::List(v); atJoin = v.size(); });
    Mark("bush", bush + " " + row + " " + std::to_string(atJoin));
    if (!Await("client_ready", nullptr, 480)) return;
    size_t afterJoin = 0;
    OnGameThread([&] { std::vector<void*> v; GP::List(v); afterJoin = v.size(); });
    UE_LOGI("plant-state: plants at the join %zu, after it %zu", atJoin, afterJoin);
    Check(afterJoin == atJoin, "a join changed no plant on the host");

    // Growth: a slow change arrives within its gradual period.
    WriteHost(bush, 0.5f, 500.f, 0.f);
    Mark("grow");
    if (!Await("grow_done")) return;

    // A client's water.
    Check(WaitFor(bush, [](void*, const GP::Values& v) { return v.water > 5000.f; }), "the client's water reached the host's plant");
    Mark("water_seen");
    if (!Await("edit_done")) return;

    // Ripe: a bush shows amount, another plant makes fruit props.
    WriteHost(bush, 1.f, 500.f, 1.f);
    ::Sleep(8000);
    GP::Values ripe;
    OnGameThread([&] { if (void* p = ByKey(bush)) GP::Read(p, ripe); });
    UE_LOGI("plant-state: ripe grow=%.2f fruit=%.2f amount=%d", ripe.grow, ripe.fruitGrow, ripe.amount);
    // Water the client has not heard of yet, as its slow change is still due: its harvest must not put back the old value.
    WriteHost(bush, 1.f, 3000.f, 1.f);
    Mark("ripe", std::to_string(ripe.amount));
    if (!Await("harvest_done")) return;
    if (ripe.amount > 0) {
        Check(WaitFor(bush, [](void*, const GP::Values& v) { return v.amount == 0; }), "the client's harvest emptied the host's plant");
        GP::Values after;
        OnGameThread([&] { if (void* p = ByKey(bush)) GP::Read(p, after); });
        UE_LOGI("plant-state: host water after the harvest %.0f", after.water);
        Check(after.water > 2000.f, "the harvest left the host's newer water alone");
    }
    size_t foodsHost = 0, propsHost = 0;
    OnGameThread([&] { foodsHost = CountOf(L"prop_food_C"); propsHost = CountOf(L"prop_C"); });
    UE_LOGI("plant-state: host item counts after the harvest food=%zu prop=%zu", foodsHost, propsHost);
    Mark("harvest_seen");
    if (!Await("planted", nullptr, 120)) return;

    // A plant this client spawned stands on the host under the same key, and the host runs it.
    std::string planted;
    { std::ifstream f(g_dir + "\\planted", std::ios::binary); std::getline(f, planted); }
    bool stands = false;
    for (int i = 0; i < 100 && !stands; ++i) {
        OnGameThread([&] { stands = ByKey(planted) != nullptr; });
        if (!stands) ::Sleep(100);
    }
    Check(stands, "the plant a client planted stands on the host under its key");
    if (stands) {
        WriteHost(planted, 0.25f, 500.f, 0.f);
        Mark("planted_seen");
    }
    if (!Await("planted_done")) return;

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
    if (!Await("dead_done")) return;

    // A new bush while the client is in, then the host's end of it.
    const std::string second = RollBush();
    Check(!second.empty(), "a second bush stands");
    Mark("second", second);
    if (!Await("second_seen")) return;
    OnGameThread([&] { if (void* p = ByKey(second)) GP::Destroy(p); });
    Mark("second_gone");
    if (!Await("removal")) return;
    Check(WaitGone(planted), "the plant the client removed left the host");
    Mark("removal_seen");
    Await("client_done");
    UE_LOGI("plant-state: DONE on host fails=%d", g_fails);
}

void RunClient() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(8000);
    std::string body;
    if (!Await("bush", &body)) return;
    const size_t sp1 = body.find(' '), sp2 = body.find(' ', sp1 + 1);
    const std::string bush = body.substr(0, sp1);
    const std::string row = body.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t hostPlants = std::strtoul(body.c_str() + sp2 + 1, nullptr, 10);
    OnGameThread([&] {
        void* t = FirstOf(L"ticker_bushSpawning_C");
        Check(t && !E::IsActorTickEnabled(t), "this client's bush spawner is parked");
    });
    bool same = WaitFor(bush, [&](void* p, const GP::Values&) { return Narrow(GP::Row(p)) == row; });
    Check(same, "a joiner has the bush from before its join, with its row");
    bool parked = WaitFor(bush, [](void* p, const GP::Values&) { return !E::IsActorTickEnabled(p); });
    Check(parked, "its copy's tick is off");
    size_t herePlants = 0;
    OnGameThread([&] { std::vector<void*> v; GP::List(v); herePlants = v.size(); });
    UE_LOGI("plant-state: plants here %zu, host %zu", herePlants, hostPlants);
    Check(herePlants == hostPlants, "this client has the host's plants, no more and no fewer");
    Mark("client_ready");

    if (!Await("grow")) return;
    Check(WaitFor(bush, [](void*, const GP::Values& v) { return Near(v.grow, 0.5f, 0.05f) && v.water > 400.f; }), "the host's growth and water arrived");
    Mark("grow_done");

    // This player's water, as a can or a toolgun writes it.
    OnGameThread([&] {
        void* p = ByKey(bush);
        GP::Values v;
        if (p && GP::Read(p, v)) {
            v.water = 7000.f;
            GP::Apply(p, GP::Row(p), v);
        }
    });
    if (!Await("water_seen")) return;
    Mark("edit_done");

    if (!Await("ripe", &body)) return;
    const int hostAmount = std::atoi(body.c_str());
    Check(WaitFor(bush, [&](void*, const GP::Values& v) { return v.grow >= 1.f && v.amount == hostAmount; }), "the ripe plant reads the same here");
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
        ::Sleep(500);
        OnGameThread([&] { foodsAfter = CountOf(L"prop_food_C"); propsAfter = CountOf(L"prop_C"); });
        UE_LOGI("plant-state: client item counts food %zu->%zu prop %zu->%zu", foodsBefore, foodsAfter, propsBefore, propsAfter);
        Check(WaitFor(bush, [](void*, const GP::Values& v) { return v.amount == 0; }), "this player's harvest emptied this copy");
    } else {
        UE_LOGI("plant-state: this row ripens no berries, so the harvest step is not run");
    }
    Mark("harvest_done");
    if (!Await("harvest_seen")) return;

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
    if (!Await("planted_seen")) return;
    Check(WaitFor(planted, [](void* p, const GP::Values& v) { return Near(v.grow, 0.25f, 0.05f) && !E::IsActorTickEnabled(p); }), "the host now runs this player's plant");
    Mark("planted_done");

    if (!Await("dead", &body)) return;
    Check(WaitFor(bush, [&](void* p, const GP::Values& v) { return Narrow(GP::Row(p)) == body && v.dryness < 0.f; }), "the dead plant's row and state arrived");
    Mark("dead_done");

    if (!Await("second", &body)) return;
    Check(WaitFor(body, [](void* p, const GP::Values&) { return !E::IsActorTickEnabled(p); }), "the new bush stands here, parked");
    Mark("second_seen");
    const std::string second = body;
    if (!Await("second_gone")) return;
    Check(WaitGone(second), "the host's removal ended it here");

    OnGameThread([&] { if (void* p = ByKey(planted)) GP::Destroy(p); });
    Mark("removal");
    if (!Await("removal_seen")) return;
    Mark("client_done");
    UE_LOGI("plant-state: DONE on client fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI PlantStateThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("plant-state: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest

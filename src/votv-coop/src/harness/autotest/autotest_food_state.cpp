// harness/autotest/autotest_food_state.cpp -- TEST-ONLY: a world food's temperature, ripeness and uses across two peers.
//
// The host spawns four foods and names them in a folder both peers read. A takes written conditions and bites from each
// peer, B the client's last bite, C the host's burn, D a burn written on the client's parked copy alone. Each peer
// checks every step against the host's numbers, written to the folder, and logs `food-drill: <step> PASS|FAIL`. After
// the host writes a new condition the client rejoins and checks it. D then shows a parked copy's looks, and each peer
// holds and throws a food the other checks. Gated by VOTVCOOP_RUN_FOOD_STATE; the folder is
// VOTVCOOP_CAN_DIR; VOTVCOOP_FOOD_REJOIN=1 runs the rejoined client's half.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/food.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace F = ue_wrap::food;

constexpr float   kTemperatureSlack = 3.f;
constexpr float   kRipenessSlack = 8.f;  // the lane's step, and the seconds a step takes to cross
constexpr uint8_t kActionEat = 4;  // enum_interactionactions: the food's action branch tests this value

std::string g_dir;
int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

void Mark(const char* name, const std::string& body = "x") {
    std::ofstream(g_dir + "\\" + name, std::ios::binary | std::ios::trunc) << body;
}

bool Await(const char* name, std::string* body = nullptr, int seconds = 240) {
    for (int i = 0; i < seconds * 10; ++i) {
        std::ifstream f(g_dir + "\\" + name, std::ios::binary);
        std::string s;
        if (f && std::getline(f, s) && !s.empty()) {
            if (body) *body = s;
            return true;
        }
        ::Sleep(100);
    }
    UE_LOGW("food-drill: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void Settle() { ::Sleep(1500); }

void* Find(const std::string& key) {
    const std::wstring w(key.begin(), key.end());
    return coop::prop_key_index::Find(w);
}

bool ReadFood(const std::string& key, F::Condition& out) {
    bool ok = false;
    OnGameThread([&] { ok = F::Read(Find(key), out); });
    return ok;
}

void WriteFood(const std::string& key, const F::Condition& c) {
    OnGameThread([&] { F::Write(Find(key), c); });
}

bool Exists(const std::string& key) {
    bool there = false;
    OnGameThread([&] { there = Find(key) != nullptr; });
    return there;
}

int CountBurnt() {
    int n = 0;
    OnGameThread([&] {
        void* cls = R::FindClass(L"prop_burntFood_C");
        if (cls) n = static_cast<int>(ue_wrap::object_index::ForEachInstance(cls, [](void*, void*, int32_t) {}, nullptr));
    });
    return n;
}

void Verdict(const char* step, bool pass, const std::string& detail) {
    if (!pass) ++g_fails;
    UE_LOGI("food-drill: %hs %hs %hs", step, pass ? "PASS" : "FAIL", detail.c_str());
}

std::string Describe(const F::Condition& c) {
    char line[96];
    std::snprintf(line, sizeof(line), "%.2f %.2f %d", c.temperature, c.ripeness, c.uses);
    return line;
}

// The host's half of a step: its numbers go in the folder under the step's name.
void Publish(const char* step, const std::string& key) {
    F::Condition c;
    if (ReadFood(key, c)) {
        Verdict(step, true, "host " + Describe(c));
        Mark(step, Describe(c));
    } else {
        Verdict(step, false, "the food is gone on the host");
        Mark(step, "gone");
    }
}

// The client's half: the same food read here against the host's numbers.
bool Compare(const char* step, const std::string& key) {
    std::string body;
    if (!Await(step, &body)) return false;
    F::Condition here;
    const bool read = ReadFood(key, here);
    if (body == "live") {
        Verdict(step, false, "the host's lives");
        return true;
    }
    if (body == "gone") {
        Verdict(step, !read, read ? "the host's is gone, this copy lives" : "gone on both");
        return true;
    }
    std::istringstream in(body);
    F::Condition host;
    in >> host.temperature >> host.ripeness >> host.uses;
    const bool same = read && here.uses == host.uses && std::fabs(here.temperature - host.temperature) <= kTemperatureSlack &&
                      std::fabs(here.ripeness - host.ripeness) <= kRipenessSlack;
    Verdict(step, same, "client " + (read ? Describe(here) : std::string("gone")) + " host " + body);
    return true;
}

void Bite(const std::string& key) {
    OnGameThread([&] {
        ue_wrap::vitals::Write(ue_wrap::vitals::Field::Food, 10.f);  // a full player's bite ends before the count
        void* food = Find(key);
        void* local = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!food || !local || !E::TryGetActorLocation(food, at)) {
            UE_LOGW("food-drill: nothing to bite");
            return;
        }
        F::Condition before, after;
        F::Read(food, before);
        const bool ok = ue_wrap::prop_use::CallActionOptionIndex(food, local, ue_wrap::prop::GetStaticMesh(food), at,
                                                                 kActionEat, ue_wrap::prop::GetStaticMesh(food));
        const bool still = F::Read(Find(key), after);
        UE_LOGI("food-drill: bit '%hs' (dispatched=%d) uses %d->%d", key.c_str(), ok ? 1 : 0, before.uses,
                still ? after.uses : 0);
    });
}

// Whether a food's steam and frozen flag read as wanted, in a verdict of its own.
void CheckLooks(const char* step, const std::string& key, bool steam, bool frozen) {
    bool readSteam = false, readFrozen = false, read = false;
    // A particle system reads active until its last particle has gone, so steam takes a while to stop.
    for (int i = 0; i < 80; ++i) {
        OnGameThread([&] { read = F::ReadLooks(Find(key), readSteam, readFrozen); });
        if (read && readSteam == steam && readFrozen == frozen) {
            UE_LOGI("food-drill: %hs settled after %d ms", step, i * 250);
            break;
        }
        ::Sleep(250);
    }
    Verdict(step, read && readSteam == steam && readFrozen == frozen,
            "steam=" + std::to_string(readSteam) + " frozen=" + std::to_string(readFrozen));
}

// This peer's player picks the food up. A client's pickup of a shared prop is asked of the host, so it waits.
bool TakeInHand(const std::string& key) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        OnGameThread([&] {
            void* player = coop::players::Registry::Get().Local();
            bool collected = false;
            if (player && Find(key)) E::CallMainPlayerHoldObject(player, Find(key), collected);
        });
        for (int i = 0; i < 30; ++i) {
            bool held = false;
            OnGameThread([&] { held = F::IsFood(coop::hand_item::LocalHandActor()); });
            if (held) return true;
            ::Sleep(100);
        }
    }
    return false;
}

void WriteHand(const F::Condition& c) {
    OnGameThread([&] { F::Write(coop::hand_item::LocalHandActor(), c); });
}

// The copy this peer shows in `slot`'s hand, checked against what the holder wrote, tick off and steam on.
void CheckMirror(const char* step, uint8_t slot, const F::Condition& want) {
    F::Condition got;
    bool read = false, ticking = true, steam = false, frozen = false;
    for (int i = 0; i < 40 && !read; ++i) {
        OnGameThread([&] {
            void* mirror = coop::hand_item::MirrorActorForSlot(slot);
            read = F::Read(mirror, got) && F::ReadLooks(mirror, steam, frozen);
            if (read) ticking = E::IsActorTickEnabled(mirror);
        });
        if (read && std::fabs(got.temperature - want.temperature) > kTemperatureSlack) read = false;
        if (!read) ::Sleep(250);
    }
    Verdict(step, read && !ticking && steam && got.uses == want.uses,
            (read ? Describe(got) : std::string("no food there")) + " ticking=" + std::to_string(ticking) +
                " steam=" + std::to_string(steam));
}

// This peer's player throws what it holds; the thrown food's key goes in the folder.
void ThrowHeld(const char* keyFile) {
    std::string key;
    OnGameThread([&] {
        void* player = coop::players::Registry::Get().Local();
        if (!player) return;
        ue_wrap::component_calls::CallParamlessNamed(player, L"throwHoldingProp");
        void* thrown = E::ReadMainPlayerDroppedItem(player);
        const std::wstring k = thrown ? ue_wrap::prop::GetInteractableKeyString(thrown) : L"";
        key.assign(k.begin(), k.end());
    });
    Mark(keyFile, key.empty() ? "none" : key);
}

// The thrown food here, against what its holder wrote into the hand; the copy has to exist and the uses have to match.
void CheckThrown(const char* step, const std::string& key, const F::Condition& want) {
    F::Condition got;
    bool read = false;
    for (int i = 0; i < 80 && !read; ++i) {
        read = ReadFood(key, got);
        if (!read) ::Sleep(250);
    }
    Settle();
    read = ReadFood(key, got);
    Verdict(step, read && got.uses == want.uses && std::fabs(got.ripeness - want.ripeness) <= 30.f,
            (read ? Describe(got) : std::string("no such food")) + " wrote " + Describe(want));
}

void RunHost() {
    if (!Await("client_ready")) return;
    ::Sleep(3000);
    std::string keys[4];
    OnGameThread([&] {
        void* cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/prop_food.prop_food_C");
        void* local = coop::players::Registry::Get().Local();
        ue_wrap::FVector me{};
        if (!cls || !local || !E::TryGetActorLocation(local, me)) return;
        for (int i = 0; i < 4; ++i) {
            void* a = E::SpawnActor(cls, {me.X + 120.f, me.Y + i * 60.f, me.Z + 40.f});
            const std::wstring k = a ? ue_wrap::prop::GetInteractableKeyString(a) : L"";
            keys[i].assign(k.begin(), k.end());
        }
    });
    for (const auto& k : keys)
        if (k.empty() || k == "None") {
            UE_LOGW("food-drill: VERDICT ABORT -- the food did not spawn with a key");
            return;
        }
    ::Sleep(2000);
    Mark("keys", keys[0] + " " + keys[1] + " " + keys[2] + " " + keys[3]);
    if (!Await("client_saw_born")) return;

    Settle();
    Publish("00-born", keys[0]);
    ::Sleep(2000);  // the client reads this step before the next write
    WriteFood(keys[0], {300.f, 3400.f, 5});
    Settle();
    Publish("01-host-wrote", keys[0]);
    ::Sleep(6000);
    Publish("02-idle-six-seconds", keys[0]);
    if (!Await("client_bit_a")) return;
    Settle();
    {
        F::Condition c;
        const bool ok = ReadFood(keys[0], c) && c.uses == 4;
        Verdict("03-host-took-client-bite", ok, ok ? Describe(c) : "uses not 4");
    }
    Publish("03-client-bit", keys[0]);
    Bite(keys[0]);
    Settle();
    {
        F::Condition c;
        const bool ok = ReadFood(keys[0], c) && c.uses == 3;
        Verdict("04-host-bit-itself", ok, ok ? Describe(c) : "uses not 3");
    }
    Publish("04-host-bit", keys[0]);
    WriteFood(keys[1], {300.f, 3400.f, 1});
    Settle();
    Mark("b_ready");
    if (!Await("client_bit_b")) return;
    Settle();
    Verdict("05-host-took-last-bite", !Exists(keys[1]), Exists(keys[1]) ? "the food lives" : "gone");
    Mark("05-client-last-bite", Exists(keys[1]) ? "live" : "gone");
    const int burntBefore = CountBurnt();
    WriteFood(keys[2], {1000.f, 3400.f, 3});
    ::Sleep(4000);
    Verdict("06-host-burnt", CountBurnt() == burntBefore + 1 && !Exists(keys[2]),
            "burnt " + std::to_string(burntBefore) + "->" + std::to_string(CountBurnt()));
    Mark("burn_counts", std::to_string(burntBefore) + " " + std::to_string(CountBurnt()));
    Mark("06-burnt-food", "gone");
    if (!Await("client_wrote_d")) return;
    ::Sleep(4000);
    Publish("07-client-burn-stays", keys[3]);

    // The looks of a parked copy: steam, then frozen, then neither.
    WriteFood(keys[3], {340.f, 3000.f, 4});
    Settle();
    Mark("hot");
    if (!Await("client_saw_hot")) return;
    WriteFood(keys[3], {250.f, 3000.f, 4});
    Settle();
    Mark("frozen");
    if (!Await("client_saw_frozen")) return;
    WriteFood(keys[3], {300.f, 3000.f, 4});
    Settle();
    Mark("thawed");
    if (!Await("client_saw_thawed")) return;

    // The client holds the food: the host shows it, parked, with the holder's numbers.
    if (!Await("client_holds")) return;
    CheckMirror("10-host-shows-client-hand", 1, {340.f, 2500.f, 3});
    Mark("host_saw_hand");
    std::string thrown;
    if (!Await("client_thrown", &thrown)) return;
    if (thrown == "none") Verdict("11-client-threw", false, "no thrown food");
    else CheckThrown("11-host-took-release", thrown, {300.f, 2500.f, 3});
    Mark("host_saw_thrown");

    // The host holds one: the client shows it the same way, and its throw reaches the client.
    if (!Await("client_ready_for_host_hand")) return;
    std::string hostFood;
    OnGameThread([&] {
        void* cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/prop_food.prop_food_C");
        void* local = coop::players::Registry::Get().Local();
        ue_wrap::FVector me{};
        if (!cls || !local || !E::TryGetActorLocation(local, me)) return;
        void* a = E::SpawnActor(cls, {me.X + 100.f, me.Y - 60.f, me.Z + 40.f});
        const std::wstring k = a ? ue_wrap::prop::GetInteractableKeyString(a) : L"";
        hostFood.assign(k.begin(), k.end());
    });
    ::Sleep(1000);
    if (!TakeInHand(hostFood)) {
        Verdict("12-host-holds", false, "the pickup did not land");
        Mark("host_holds", "no");
    } else {
        WriteHand({340.f, 2600.f, 2});
        Mark("host_holds");
    }
    if (!Await("client_saw_host_hand")) return;
    WriteHand({300.f, 2600.f, 2});
    ::Sleep(600);
    ThrowHeld("host_thrown");
    if (!Await("client_done")) return;
    UE_LOGI("food-drill: DONE on host fails=%d", g_fails);

    if (!Await("client_killed", nullptr, 600)) return;
    WriteFood(keys[0], {295.f, 3000.f, 2});
    if (!Await("rejoin_ready", nullptr, 600)) return;
    Publish("08-while-away", keys[0]);
    if (!Await("rejoin_done", nullptr, 600)) return;
    UE_LOGI("food-drill: REJOIN DONE on host fails=%d", g_fails);
}

void WaitForWorld() {
    bool ready = false;
    for (int i = 0; i < 480 && !ready; ++i) {
        OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
        if (!ready) ::Sleep(500);
    }
    ::Sleep(8000);
}

void RunClient() {
    WaitForWorld();
    Mark("client_ready");
    std::string keyLine;
    if (!Await("keys", &keyLine)) return;
    std::istringstream in(keyLine);
    std::string keys[4];
    for (auto& k : keys) in >> k;
    for (int i = 0; i < 100 && !(Exists(keys[0]) && Exists(keys[1]) && Exists(keys[2]) && Exists(keys[3])); ++i) ::Sleep(100);
    Mark("client_saw_born");

    if (!Compare("00-born", keys[0])) return;
    if (!Compare("01-host-wrote", keys[0])) return;
    if (!Compare("02-idle-six-seconds", keys[0])) return;
    Bite(keys[0]);
    Mark("client_bit_a");
    if (!Compare("03-client-bit", keys[0])) return;
    if (!Compare("04-host-bit", keys[0])) return;
    if (!Await("b_ready")) return;
    Bite(keys[1]);
    Mark("client_bit_b");
    if (!Compare("05-client-last-bite", keys[1])) return;
    std::string counts;
    if (!Await("burn_counts", &counts)) return;
    {
        std::istringstream c(counts);
        int before = 0, after = 0;
        c >> before >> after;
        Verdict("06-one-burnt-on-client", CountBurnt() == after,
                "client " + std::to_string(CountBurnt()) + " host " + std::to_string(after));
    }
    if (!Compare("06-burnt-food", keys[2])) return;
    WriteFood(keys[3], {1000.f, 3400.f, 3});
    Mark("client_wrote_d");
    ::Sleep(3000);
    Verdict("07-parked-copy-did-not-burn", Exists(keys[3]), Exists(keys[3]) ? "it lives" : "it burnt on its own");
    std::string d;
    if (!Await("07-client-burn-stays", &d)) return;
    Verdict("07-host-copy-lives", d != "gone", d);

    if (!Await("hot")) return;
    CheckLooks("09-parked-steam", keys[3], true, false);
    Mark("client_saw_hot");
    if (!Await("frozen")) return;
    CheckLooks("09-parked-frozen", keys[3], false, true);
    Mark("client_saw_frozen");
    if (!Await("thawed")) return;
    CheckLooks("09-parked-thawed", keys[3], false, false);
    Mark("client_saw_thawed");

    if (!TakeInHand(keys[3])) {
        Verdict("10-client-holds", false, "the pickup did not land");
        Mark("client_holds", "no");
    } else {
        WriteHand({340.f, 2500.f, 3});
        Mark("client_holds");
    }
    if (!Await("host_saw_hand")) return;
    WriteHand({300.f, 2500.f, 3});
    ::Sleep(600);
    ThrowHeld("client_thrown");
    {
        std::string thrown;
        if (Await("client_thrown", &thrown) && thrown != "none") CheckThrown("11-client-keeps-own", thrown, {300.f, 2500.f, 3});
    }
    if (!Await("host_saw_thrown")) return;
    Mark("client_ready_for_host_hand");
    if (!Await("host_holds")) return;
    CheckMirror("12-client-shows-host-hand", 0, {340.f, 2600.f, 2});
    Mark("client_saw_host_hand");
    {
        std::string thrown;
        if (Await("host_thrown", &thrown) && thrown != "none") CheckThrown("13-client-saw-host-throw", thrown, {300.f, 2600.f, 2});
        else Verdict("13-client-saw-host-throw", false, "no thrown food");
    }
    UE_LOGI("food-drill: DONE on client fails=%d", g_fails);
    Mark("client_done");
}

void RunRejoin() {
    WaitForWorld();
    std::string keyLine;
    if (!Await("keys", &keyLine)) return;
    std::istringstream in(keyLine);
    std::string keyA;
    in >> keyA;
    ::Sleep(4000);
    Mark("rejoin_ready");
    if (!Compare("08-while-away", keyA)) return;
    UE_LOGI("food-drill: REJOIN DONE fails=%d", g_fails);
    Mark("rejoin_done");
}

}  // namespace

DWORD WINAPI FoodStateThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("food-drill: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (!IsClientRole()) RunHost();
    else if (coop::config::ReadEnv("VOTVCOOP_FOOD_REJOIN") == "1") RunRejoin();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest

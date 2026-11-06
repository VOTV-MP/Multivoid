// harness/autotest/autotest_cook_state.cpp -- TEST-ONLY: a microwave, a mini oven and cooking progress across two peers.
//
// The host spawns the machines and two foods, one in the microwave, and names them in a folder both peers read. Each peer
// presses each machine's controls by its own action in turn, and each step is checked on both against the expected
// setting. Then a cooking food's progress is read on both while the host's tick cooks it, and the client rejoins to read
// the oven's controls, which no save carries. A grill takes coals, a light and its lid from each peer, burns through two
// coals on the host's count, and is left lit for the rejoin. Machines are powered by a write, since a cord is another lane. Gated by
// VOTVCOOP_RUN_COOK_STATE; the folder is VOTVCOOP_CAN_DIR; VOTVCOOP_COOK_REJOIN=1 runs the rejoined client's half.

#include "harness/autotest.h"

#include "coop/config/config.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/props/food_tick_gate.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/food.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/component_calls.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/devices/kitchen.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_component.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace F = ue_wrap::food;
namespace K = ue_wrap::kitchen;

constexpr uint8_t kActionNext = 4;   // the microwave's time, and the oven's control under the player's look
constexpr uint8_t kActionStart = 8;  // the microwave's start

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
    UE_LOGW("cook-drill: VERDICT ABORT -- no '%hs' from the other peer", name);
    return false;
}

void* Find(const std::string& key) {
    const std::wstring w(key.begin(), key.end());
    return coop::prop_key_index::Find(w);
}

void Verdict(const char* step, bool pass, const std::string& detail) {
    if (!pass) ++g_fails;
    UE_LOGI("cook-drill: %hs %hs %hs", step, pass ? "PASS" : "FAIL", detail.c_str());
}

void SetBool(void* actor, const wchar_t* name, bool value) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!actor || !R::FindBoolProperty(R::ClassOf(actor), name, off, mask) || off < 0 || !mask) return;
    auto& b = static_cast<uint8_t*>(actor)[off];
    b = value ? (b | mask) : (b & ~mask);
}

// The microwave's own inside: the centre of its Box, whose overlap fills the objectsInside its Wave heats.
bool InsideOf(void* micro, ue_wrap::FVector& at) {
    const int32_t off = micro ? R::FindPropertyOffset(R::ClassOf(micro), L"Box") : -1;
    void* box = nullptr;
    if (off >= 0) std::memcpy(&box, static_cast<uint8_t*>(micro) + off, sizeof(box));
    return box && E::TryGetComponentLocation(box, at);
}

// Whether `obj` is in the microwave's objectsInside.
bool IsInside(void* micro, void* obj) {
    const int32_t off = micro ? R::FindPropertyOffset(R::ClassOf(micro), L"objectsInside") : -1;
    if (off < 0 || !obj) return false;
    struct Arr { void** data; int32_t num, max; } a{};
    std::memcpy(&a, static_cast<uint8_t*>(micro) + off, sizeof(a));
    for (int32_t i = 0; a.data && i < a.num && i < 64; ++i)
        if (a.data[i] == obj) return true;
    return false;
}

void Power(const std::string& key) {
    OnGameThread([&] { SetBool(Find(key), L"powered", true); });
}

void Press(const std::string& key, uint8_t action, const wchar_t* lookFlag = nullptr, const wchar_t* otherFlag = nullptr) {
    OnGameThread([&] {
        void* machine = Find(key);
        void* local = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!machine || !local || !E::TryGetActorLocation(machine, at)) {
            UE_LOGW("cook-drill: nothing to press");
            return;
        }
        if (lookFlag) SetBool(machine, lookFlag, true);
        if (otherFlag) SetBool(machine, otherFlag, false);
        ue_wrap::prop_use::CallActionOptionIndex(machine, local, ue_wrap::prop::GetStaticMesh(machine), at, action,
                                                 ue_wrap::prop::GetStaticMesh(machine));
    });
}

// The machine's state must reach what is wanted in time: a microwave's run lasts 15 s of its own timer.
void Check(const char* step, const std::string& key, int32_t mode, bool active, int quarterSeconds = 40) {
    K::State s;
    bool read = false;
    for (int i = 0; i < quarterSeconds; ++i) {
        OnGameThread([&] { read = K::Read(Find(key), s); });
        if (read && s.mode == mode && s.active == active) break;
        ::Sleep(250);
    }
    Verdict(step, read && s.mode == mode && s.active == active,
            read ? "mode=" + std::to_string(s.mode) + " active=" + std::to_string(s.active) +
                       " left=" + std::to_string(s.remaining)
                 : std::string("no such machine"));
}

void CheckGrill(const char* step, const std::string& key, int32_t coals, bool active, bool lid, int quarterSeconds = 40) {
    K::State s;
    bool read = false;
    for (int i = 0; i < quarterSeconds; ++i) {
        OnGameThread([&] { read = K::Read(Find(key), s); });
        if (read && s.mode == coals && s.active == active && s.lid == lid) break;
        ::Sleep(250);
    }
    Verdict(step, read && s.mode == coals && s.active == active && s.lid == lid,
            read ? "coals=" + std::to_string(s.mode) + " active=" + std::to_string(s.active) + " lid=" + std::to_string(s.lid) +
                       " burn=" + std::to_string(s.remaining)
                 : std::string("no such grill"));
}

void GrillVerb(const std::string& key, const wchar_t* verb) {
    OnGameThread([&] {
        void* grill = Find(key);
        void* fn = grill ? R::FindDispatchFunctionCached(R::ClassOf(grill), verb) : nullptr;
        if (!fn) {
            UE_LOGW("cook-drill: %ls did not resolve", verb);
            return;
        }
        ue_wrap::ParamFrame f(fn);
        if (f.valid()) ue_wrap::Call(grill, f);
    });
}

void SetLid(const std::string& key, bool lid) {
    OnGameThread([&] {
        void* grill = Find(key);
        void* fn = grill ? R::FindDispatchFunctionCached(R::ClassOf(grill), L"setLid") : nullptr;
        if (!fn) return;
        ue_wrap::ParamFrame f(fn);
        if (f.valid() && f.Set<bool>(L"lid", lid)) ue_wrap::Call(grill, f);
    });
}

// The host's burn, nearly through its coal, so the next tick takes one.
void NearlyBurnt(const std::string& key) {
    OnGameThread([&] {
        void* grill = Find(key);
        static R::InstanceOffset sBurn{L"a"};
        const int32_t off = grill ? sBurn.Of(grill) : -1;
        const float value = 0.99f;
        if (off >= 0) std::memcpy(static_cast<uint8_t*>(grill) + off, &value, sizeof(value));
    });
}

constexpr uint8_t kActionTakeLid = 7;
constexpr uint8_t kActionOff = 4;

// World lids: actors named bbq_1 of the two classes the press can make, outside any hand.
int CountWorldLids() {
    int n = 0;
    for (const wchar_t* name : {L"prop_C", L"prop_food_C"}) {
        void* cls = R::FindClass(name);
        if (!cls) continue;
        ue_wrap::object_index::ForEachInstance(
            cls,
            [](void* ctx, void* obj, int32_t) {
                // The walk lists a destroyed actor until the collector takes it.
                if (R::IsLive(obj) && ue_wrap::prop::GetPropNameString(obj) == L"bbq_1" &&
                    !coop::hand_item::IsHandAxisActor(obj))
                    ++*static_cast<int*>(ctx);
            },
            &n);
    }
    return n;
}

std::string HandName(void* actor) {
    const std::wstring name = actor ? ue_wrap::prop::GetPropNameString(actor) : L"nothing";
    return std::string(name.begin(), name.end());
}

// The holder's own hand, or the copy this peer shows in `slot`'s, within a few seconds.
void CheckHand(const char* step, int slot, const std::string& want) {
    std::string got;
    int lids = -1;
    for (int i = 0; i < 24; ++i) {
        OnGameThread([&] {
            void* actor = slot < 0 ? coop::hand_item::LocalHandActor()
                                   : coop::hand_item::MirrorActorForSlot(static_cast<uint8_t>(slot));
            got = HandName(actor);
            lids = CountWorldLids();
        });
        if (got == want && lids == 0) break;
        ::Sleep(250);
    }
    Verdict(step, got == want && lids == 0, "hand=" + got + " world lids=" + std::to_string(lids));
}

void PressGrill(const std::string& key, uint8_t action) { Press(key, action); }

// The lid back on as the game's own use of a held item does it.
void ReturnLid(const std::string& key) {
    OnGameThread([&] {
        void* grill = Find(key);
        void* local = coop::players::Registry::Get().Local();
        void* hand = coop::hand_item::LocalHandActor();
        ue_wrap::FVector at{};
        if (!grill || !local || !hand || !E::TryGetActorLocation(grill, at)) {
            UE_LOGW("cook-drill: no lid in hand to return");
            return;
        }
        void* mesh = ue_wrap::prop::GetStaticMesh(grill);
        ue_wrap::prop_use::CallPlayerUsedOn(grill, local, mesh, at, mesh, hand, ue_wrap::prop::GetPropName(hand));
    });
}

// Coals in by the grill's own addCoal, with the host's count of them.
void AddCoals(const std::string& key, int n) {
    for (int i = 0; i < n; ++i) GrillVerb(key, L"addCoal");
}

// The door's own functions on this peer's microwave, as its door's delegates call them.
void DoorFn(const std::string& key, const wchar_t* fn) {
    OnGameThread([&] { ue_wrap::component_calls::CallParamlessNamed(Find(key), fn); });
}

void CheckDoor(const char* step, const std::string& key, bool active, bool open) {
    K::State s;
    bool read = false;
    for (int i = 0; i < 16; ++i) {
        OnGameThread([&] { read = K::Read(Find(key), s); });
        if (read && s.active == active && s.doorOpen == open) break;
        ::Sleep(250);
    }
    Verdict(step, read && s.active == active && s.doorOpen == open,
            read ? "active=" + std::to_string(s.active) + " open=" + std::to_string(s.doorOpen) +
                       " left=" + std::to_string(s.remaining)
                 : std::string("no such machine"));
}

float ReadTemperature(const std::string& key) {
    F::Condition c;
    bool ok = false;
    OnGameThread([&] { ok = F::Read(Find(key), c); });
    return ok ? c.temperature : -1.f;
}

// Both sides of a step: this peer checks, says so, and waits for the other's mark.
bool Sync(const char* mine, const char* theirs) {
    Mark(mine);
    return Await(theirs);
}

void* SpawnHere(const wchar_t* path, float dx, float dy) {
    void* cls = ue_wrap::asset_load::LoadObjectByPath(path);
    void* local = coop::players::Registry::Get().Local();
    ue_wrap::FVector me{};
    if (!cls || !local || !E::TryGetActorLocation(local, me)) return nullptr;
    return E::SpawnActor(cls, {me.X + dx, me.Y + dy, me.Z + 40.f});
}

int CountInstances(const std::string& className) {
    const std::wstring w(className.begin(), className.end());
    void* cls = R::FindClass(w.c_str());
    return cls ? static_cast<int>(ue_wrap::object_index::ForEachInstance(cls, [](void*, void*, int32_t) {}, nullptr)) : 0;
}

// Every actor of the food family's plain class by Key, to name what a cook left behind.
std::string ListPlainFoods() {
    std::string out;
    void* cls = R::FindClass(L"prop_food_C");
    if (!cls) return out;
    ue_wrap::object_index::ForEachInstance(
        cls,
        [](void* ctx, void* obj, int32_t) {
            const std::wstring k = ue_wrap::prop::GetInteractableKeyString(obj);
            auto* text = static_cast<std::string*>(ctx);
            text->append(k.begin(), k.end());
            text->push_back(' ');
        },
        &out);
    return out;
}

std::string KeyString(void* actor) {
    const std::wstring k = actor ? ue_wrap::prop::GetInteractableKeyString(actor) : L"";
    return std::string(k.begin(), k.end());
}

void RunGrill(const std::string& grill, const std::string& onGrill, bool host) {
    const char* me = host ? "gh" : "gc";
    const char* them = host ? "gc" : "gh";
    const auto step = [&](const char* name) {
        return Sync((std::string(me) + name).c_str(), (std::string(them) + name).c_str());
    };
    K::State start;
    OnGameThread([&] { K::Read(Find(grill), start); });
    const bool lid = start.lid;

    if (!host) AddCoals(grill, 1);
    CheckGrill("20-client-adds-coal", grill, 1, false, lid);
    if (!step("20")) return;
    if (host) AddCoals(grill, 2);
    CheckGrill("21-host-adds-coals", grill, 3, false, lid);
    if (!step("21")) return;
    const float before = ReadTemperature(onGrill);
    if (!host) GrillVerb(grill, L"attemptIgnite");
    CheckGrill("22-client-lights-grill", grill, 3, true, lid);
    // The heat is the overlap of the grill's box, whose collision the light switches on: the food goes in once it is lit.
    if (host) {
        OnGameThread([&] {
            void* g = Find(grill);
            void* food = Find(onGrill);
            static R::InstanceOffset sBox{L"box"};
            const int32_t off = g ? sBox.Of(g) : -1;
            void* box = nullptr;
            if (off >= 0) std::memcpy(&box, static_cast<const uint8_t*>(g) + off, sizeof(box));
            if (box && food) E::SetActorLocation(food, E::GetComponentLocation(box));
        });
    }
    float hot = before;
    for (int i = 0; i < 32 && hot <= before + 1.f; ++i) {
        ::Sleep(250);
        hot = ReadTemperature(onGrill);
    }
    Verdict("22-food-on-grill-heats", hot > before + 1.f, std::to_string(before) + "->" + std::to_string(hot));
    if (!step("22")) return;
    if (host) NearlyBurnt(grill);
    CheckGrill("23-a-coal-burns-on-the-host-count", grill, 2, true, lid, 60);
    if (!step("23")) return;
    if (host) NearlyBurnt(grill);
    CheckGrill("24-a-second-coal", grill, 1, true, lid, 60);
    if (!step("24")) return;
    if (host) NearlyBurnt(grill);
    CheckGrill("25-the-last-coal-puts-it-out", grill, 0, false, lid, 60);
    if (!step("25")) return;
    if (!host) SetLid(grill, !lid);
    CheckGrill("26-client-moves-the-lid", grill, 0, false, !lid);
    if (!step("26")) return;
    if (host) SetLid(grill, lid);
    CheckGrill("27-host-moves-the-lid", grill, 0, false, lid);
    if (!step("27")) return;

    // The real press: the lid comes off into the presser's hand, and its use on the grill puts it back.
    if (host) SetLid(grill, true);
    CheckGrill("40-the-lid-is-on", grill, 0, false, true);
    if (!step("40")) return;
    if (!host) PressGrill(grill, kActionTakeLid);
    CheckGrill("41-client-takes-the-lid", grill, 0, false, false);
    CheckHand(host ? "41-host-shows-it-in-client-hand" : "41-client-holds-it", host ? 1 : -1, "bbq_1");
    if (!step("41")) return;
    if (!host) ReturnLid(grill);
    CheckGrill("42-client-puts-it-back", grill, 0, false, true);
    CheckHand(host ? "42-client-hand-empty-here" : "42-client-hand-empty", host ? 1 : -1, "nothing");
    if (!step("42")) return;
    if (host) PressGrill(grill, kActionTakeLid);
    CheckGrill("43-host-takes-the-lid", grill, 0, false, false);
    CheckHand(host ? "43-host-holds-it" : "43-client-shows-it-in-host-hand", host ? -1 : 0, "bbq_1");
    if (!step("43")) return;
    if (host) ReturnLid(grill);
    CheckGrill("44-host-puts-it-back", grill, 0, false, true);
    CheckHand(host ? "44-host-hand-empty" : "44-host-hand-empty-here", host ? -1 : 0, "nothing");
    if (!step("44")) return;

    // A press with coals in puts the fire out on every peer, and the host's next count must not light it again.
    if (host) AddCoals(grill, 3);
    CheckGrill("45-coals-for-the-off-press", grill, 3, false, true);
    if (!step("45")) return;
    if (host) GrillVerb(grill, L"attemptIgnite");
    CheckGrill("46-host-lights-it", grill, 3, true, true);
    if (!step("46")) return;
    if (!host) PressGrill(grill, kActionOff);
    CheckGrill("47-client-press-puts-it-out", grill, 3, false, true);
    if (!step("47")) return;
    ::Sleep(2000);
    CheckGrill("47b-it-stays-out", grill, 3, false, true, 4);
    if (!step("47b")) return;
    if (host) SetLid(grill, lid);
    CheckGrill("48-lid-as-it-was", grill, 3, false, lid);
    if (!step("48")) return;
    if (host) {
        OnGameThread([&] { K::ApplyMode(Find(grill), 0); });
    }
    CheckGrill("49-coals-cleared", grill, 0, false, lid);
    if (!step("49")) return;
    if (host) {
        AddCoals(grill, 8);
        GrillVerb(grill, L"attemptIgnite");
    }
    CheckGrill("28-left-lit-for-the-rejoin", grill, 8, true, lid, 60);
    (void)step("28");
}

void RunShared(const std::string& micro, const std::string& oven, const std::string& inside, bool host) {
    const char* me = host ? "h" : "c";
    const char* them = host ? "c" : "h";
    auto mine = [&](const char* s) { return std::string(me) + s; };
    auto theirs = [&](const char* s) { return std::string(them) + s; };
    const auto step = [&](const char* name) { return Sync(mine(name).c_str(), theirs(name).c_str()); };

    Power(micro);
    Power(oven);
    if (!step("powered")) return;

    // The client sets the microwave's time, then the host does.
    if (!host) Press(micro, kActionNext);
    Check("01-client-sets-microwave", micro, 1, false);
    if (!step("01")) return;
    if (host) Press(micro, kActionNext);
    Check("02-host-sets-microwave", micro, 2, false);
    if (!step("02")) return;

    // The client starts it: both run, and the food inside heats on the host, the food's one author.
    const float before = ReadTemperature(inside);
    if (!host) Press(micro, kActionStart);
    Check("03-client-starts-microwave", micro, 2, true);
    ::Sleep(5000);
    const float during = ReadTemperature(inside);
    Verdict("03-food-heats", during > before + 1.f, std::to_string(before) + "->" + std::to_string(during));
    if (!step("03")) return;
    // Its 15 s end on each peer's own timer.
    Check("04-microwave-ends", micro, 2, false, 100);
    if (!step("04")) return;
    if (host) Press(micro, kActionStart);
    Check("05-host-starts-microwave", micro, 2, true);
    if (!step("05")) return;
    Check("05-microwave-ends", micro, 2, false, 100);
    if (!step("05b")) return;

    // A door opened mid-run stops every peer's, each way; an open door refuses a start on every peer.
    if (!host) Press(micro, kActionStart);
    CheckDoor("30-client-starts", micro, true, false);
    if (!step("30")) return;
    if (!host) DoorFn(micro, L"openDoor");
    CheckDoor("31-client-door-stops-it-everywhere", micro, false, false);
    if (!step("31")) return;
    if (host) Press(micro, kActionStart);
    CheckDoor("32-host-starts", micro, true, false);
    if (!step("32")) return;
    if (host) DoorFn(micro, L"openDoor");
    CheckDoor("33-host-door-stops-it-everywhere", micro, false, false);
    if (!step("33")) return;
    if (!host) DoorFn(micro, L"openDoor");
    CheckDoor("34-client-opens-idle-door", micro, false, true);
    if (!step("34")) return;
    if (host) Press(micro, kActionStart);
    ::Sleep(1000);
    CheckDoor("35-open-door-refuses-host-start", micro, false, true);
    if (!step("35")) return;
    if (host) DoorFn(micro, L"closeDoor");
    CheckDoor("36-host-closes-door", micro, false, false);
    if (!step("36")) return;
    // A setting press from a peer whose copy is off leaves another peer's run alone.
    if (host) Press(micro, kActionStart);
    CheckDoor("37-host-starts", micro, true, false);
    if (!step("37")) return;
    if (!host) {
        DoorFn(micro, L"forceOpenDoor");  // this copy alone, off without a press: the message below says it stopped
        ::Sleep(600);
    }
    if (!step("37b")) return;
    CheckDoor("38-a-stop-on-one-copy-crosses", micro, false, false);
    if (!step("38")) return;
    if (!host) Press(micro, kActionNext);
    Check("39-setting-after-the-stops", micro, 3, false);
    if (!step("39")) return;
    if (!host) { Press(micro, kActionNext); Press(micro, kActionNext); Press(micro, kActionNext); Press(micro, kActionNext); }
    Check("39b-setting-back-to-2", micro, 2, false);
    if (!step("39b")) return;

    // The oven: the client's setting, the host's switch, the client's switch.
    if (!host) Press(oven, kActionNext, L"input_temp", L"input_switch");
    Check("06-client-sets-oven", oven, 1, false);
    if (!step("06")) return;
    if (host) Press(oven, kActionNext, L"input_switch", L"input_temp");
    Check("07-host-starts-oven", oven, 1, true);
    if (!step("07")) return;
    if (!host) Press(oven, kActionNext, L"input_switch", L"input_temp");
    Check("08-client-stops-oven", oven, 1, false);
    if (!step("08")) return;
    if (host) {
        Press(oven, kActionNext, L"input_switch", L"input_temp");
        Press(oven, kActionNext, L"input_temp", L"input_switch");
    }
    Check("09-host-leaves-oven-on-mode-2", oven, 2, true);
    (void)step("09");
}

void RunHost() {
    if (!Await("client_ready")) return;
    ::Sleep(3000);
    std::string micro, oven, inside, cooking, grill, onGrill;
    OnGameThread([&] {
        void* g = SpawnHere(L"/Game/objects/prop_bbq.prop_bbq_C", 300.f, -60.f);
        ue_wrap::FVector grillAt{};
        void* food = nullptr;
        if (g && E::TryGetActorLocation(g, grillAt)) {
            void* cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/prop_food.prop_food_C");
            if (cls) food = E::SpawnActor(cls, grillAt);
        }
        grill = KeyString(g);
        onGrill = KeyString(food);
        void* m = SpawnHere(L"/Game/objects/prop_microwave.prop_microwave_C", 150.f, 0.f);
        void* o = SpawnHere(L"/Game/objects/prop_minioven.prop_minioven_C", 150.f, 120.f);
        void* c = SpawnHere(L"/Game/objects/prop_cookingFood_shrimp.prop_cookingFood_shrimp_C", 150.f, -120.f);
        ue_wrap::FVector at{};
        void* f = nullptr;
        if (m && (InsideOf(m, at) || E::TryGetActorLocation(m, at))) {
            void* cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/prop_food.prop_food_C");
            if (cls) f = E::SpawnActor(cls, at);
        }
        micro = KeyString(m);
        oven = KeyString(o);
        cooking = KeyString(c);
        inside = KeyString(f);
    });
    if (micro.empty() || oven.empty() || inside.empty() || cooking.empty() || grill.empty() || onGrill.empty()) {
        UE_LOGW("cook-drill: VERDICT ABORT -- spawned micro='%hs' oven='%hs' inside='%hs' cooking='%hs'", micro.c_str(),
                oven.c_str(), inside.c_str(), cooking.c_str());
        Mark("keys", "none");
        return;
    }
    ::Sleep(2000);
    bool in = false;
    OnGameThread([&] { in = IsInside(Find(micro), Find(inside)); });
    Verdict("00-food-in-the-microwave", in, inside);
    Mark("keys", micro + " " + oven + " " + inside + " " + cooking + " " + grill + " " + onGrill);
    if (!Await("client_saw_born")) return;
    RunShared(micro, oven, inside, true);
    RunGrill(grill, onGrill, true);

    // The host's tick cooks a hot food; the client's copy is parked and reads the progress. It ends in one plain food.
    if (!Await("client_counted")) return;
    int before = 0, after = 0;
    OnGameThread([&] { before = CountInstances("prop_food_C"); });
    OnGameThread([&] {
        F::Condition c;
        if (F::Read(Find(cooking), c)) {
            c.temperature = 340.f;
            F::Write(Find(cooking), c);
        }
    });
    std::string listBefore, listAfter;
    OnGameThread([&] { listBefore = ListPlainFoods(); });
    if (!Await("client_read_progress")) return;
    OnGameThread([&] {
        F::Condition c;
        if (F::Read(Find(cooking), c)) {
            c.cookProgress = 100000.f;  // a shrimp cools below the cooking heat long before its own end
            F::Write(Find(cooking), c);
        }
    });
    bool rawGone = false;
    for (int i = 0; i < 40 && !rawGone; ++i) {
        OnGameThread([&] { rawGone = Find(cooking) == nullptr; });
        if (!rawGone) ::Sleep(250);
    }
    ::Sleep(3000);
    OnGameThread([&] {
        after = CountInstances("prop_food_C");
        listAfter = ListPlainFoods();
    });
    UE_LOGI("cook-drill: plain foods before: %hs", listBefore.c_str());
    UE_LOGI("cook-drill: plain foods after: %hs", listAfter.c_str());
    Verdict("12-host-one-cooked", rawGone && after == before + 1,
            "prop_food_C " + std::to_string(before) + "->" + std::to_string(after));
    if (!Await("client_cooked")) return;
    UE_LOGI("cook-drill: DONE on host fails=%d", g_fails);
    if (!Await("rejoin_done", nullptr, 600)) return;
    UE_LOGI("cook-drill: REJOIN DONE on host fails=%d", g_fails);
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
    std::string line;
    if (!Await("keys", &line) || line == "none") return;
    std::istringstream in(line);
    std::string micro, oven, inside, cooking, grill, onGrill;
    in >> micro >> oven >> inside >> cooking >> grill >> onGrill;
    Mark("client_saw_born");
    RunShared(micro, oven, inside, false);
    RunGrill(grill, onGrill, false);

    int before = 0, after = 0;
    OnGameThread([&] { before = CountInstances("prop_food_C"); });
    Mark("client_counted");
    float progress = -1.f;
    bool rawGone = false;
    coop::food_tick_gate::Counts gateBefore{}, gateAfter{};
    OnGameThread([&] { gateBefore = coop::food_tick_gate::GetCounts(); });
    for (int i = 0; i < 160 && progress < 1.f; ++i) {
        F::Condition c;
        bool ok = false;
        OnGameThread([&] { ok = F::Read(Find(cooking), c); });
        if (ok && c.cookProgress > progress) progress = c.cookProgress;
        if (progress < 1.f) ::Sleep(125);
    }
    OnGameThread([&] { gateAfter = coop::food_tick_gate::GetCounts(); });
    Verdict("10-client-reads-progress", progress > 0.f, std::to_string(progress));
    // The client's cooking copy runs no tick of its own: the food tick gate refuses it (coop/props/food_tick_gate).
    const auto refused = gateAfter.refused - gateBefore.refused, ran = gateAfter.ran - gateBefore.ran;
    Verdict("10-cooking-copy-refused-its-tick", refused > 0 && ran == 0,
            "refused=" + std::to_string(refused) + " ran=" + std::to_string(ran));
    Mark("client_read_progress");
    for (int i = 0; i < 80 && !rawGone; ++i) {
        OnGameThread([&] { rawGone = Find(cooking) == nullptr; });
        if (!rawGone) ::Sleep(250);
    }
    ::Sleep(3000);
    std::string listAfter;
    OnGameThread([&] {
        after = CountInstances("prop_food_C");
        listAfter = ListPlainFoods();
    });
    UE_LOGI("cook-drill: plain foods after: %hs", listAfter.c_str());
    Verdict("12-client-one-cooked", rawGone && after == before + 1,
            "prop_food_C " + std::to_string(before) + "->" + std::to_string(after));
    Mark("client_cooked");
    UE_LOGI("cook-drill: DONE on client fails=%d", g_fails);
}

void RunRejoin() {
    WaitForWorld();
    std::string line;
    if (!Await("keys", &line)) return;
    std::istringstream in(line);
    std::string micro, oven, inside, cooking, grill, onGrill;
    in >> micro >> oven >> inside >> cooking >> grill >> onGrill;
    ::Sleep(3000);
    Check("11-joiner-reads-oven", oven, 2, true);
    K::State g;
    bool read = false;
    OnGameThread([&] { read = K::Read(Find(grill), g); });
    Verdict("29-joiner-reads-lit-grill", read && g.active && g.mode > 0 && g.mode <= 8,
            read ? "coals=" + std::to_string(g.mode) + " active=" + std::to_string(g.active) : std::string("no such grill"));
    UE_LOGI("cook-drill: REJOIN DONE fails=%d", g_fails);
    Mark("rejoin_done");
}

}  // namespace

DWORD WINAPI CookStateThread(LPVOID /*arg*/) {
    g_dir = coop::config::ReadEnv("VOTVCOOP_CAN_DIR");
    if (g_dir.empty()) {
        UE_LOGW("cook-drill: VERDICT ABORT -- VOTVCOOP_CAN_DIR is unset");
        return 0;
    }
    if (!IsClientRole()) RunHost();
    else if (coop::config::ReadEnv("VOTVCOOP_COOK_REJOIN") == "1") RunRejoin();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest

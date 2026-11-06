// harness/autotest/autotest_food_state.cpp -- TEST-ONLY: a world food's temperature, ripeness and uses across two peers.
//
// The client meets the host, then the host spawns seven foods and names them in a folder both peers read. A takes
// written conditions and bites from each peer, B the client's last bite, C the host's burn, D a burn written on the
// client's copy alone, whose tick the gate refuses, then its steam and frozen looks. Each peer then holds a food, writes its numbers into
// the hand, and lets it go twice: thrown, then placed down. The other peer checks the hand's mirror and the food that
// lands. Each peer checks every step against the host's numbers, written to the folder, and logs
// `food-drill: <step> PASS|FAIL`; the client hands its failed steps to the host, which logs `food-drill: VERDICT
// PASS|FAIL`, then `DONE`; each peer also judges what the food tick gate did (nothing ran against its rule, and the
// client's gate refused some). After a new condition the host writes while the client is away, the client rejoins and
// checks it. Gated by VOTVCOOP_RUN_FOOD_STATE; the folder is VOTVCOOP_CAN_DIR; VOTVCOOP_FOOD_REJOIN=1 runs the
// rejoined client's half. Every wait is on a game state or the other peer's signal, with a bound; the two dwells
// that stand for an absence or an unobservable order are named where they sit.

#include "harness/autotest.h"
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"
#include "harness/session_runtime.h"

#include "coop/config/config.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/food_tick_gate.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/net_pump.h"
#include "ue_wrap/actors/food.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/prop_use.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
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
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;
namespace F = ue_wrap::food;
namespace PS = harness::autotest::peer_signal;
using PS::Await;
using PS::AwaitText;
using PS::Mark;
using PS::WaitFor;

constexpr float   kTemperatureSlack = 3.f;
constexpr float   kRipenessSlack = 8.f;  // the lane's step, and the seconds a step takes to cross
constexpr float   kRipenessStep = 5.f;   // the lane's publish step: a host food that fell this far has rolled on
constexpr uint8_t kActionEat = 4;  // enum_interactionactions: the food's action branch tests this value

constexpr int kFoods = 7;          // 0 bites, 1 last bite, 2 burn, 3 looks and the client's throw, 4 the host's throw,
                                   // 5 the client's place, 6 the host's place
constexpr int kStepBoundSec = 30;  // a step's effect on this peer's copy, once the other peer has done it
constexpr int kReadyBoundSec = 400;  // the client's walk to the host, or the other peer's whole life
constexpr int kHandBoundSec = 10;   // the other peer's hand shown here

// A client's copy, its tick refused, must not burn on its own after the client wrote 1000 K onto it: an absence, and a burn that does not
// come has no signal. The host's own burn (step 06) runs on the same tick, which is what bounds the wait.
constexpr int kNoBurnDwellMs = 3000;
// Between the second hand write and the let-go. The game's let-go reads the hand itself (checkEquip calls getData),
// so nothing here waits on the game; what is left is the hand's repeat of the new numbers reaching the other peer
// first, a send nothing signals. A clock whose purpose is unproved, kept.
constexpr int kHandDwellMs = 600;

int g_fails = 0;
std::vector<std::string> g_failed;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

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

bool AllExist(const std::string* keys) {
    for (int i = 0; i < kFoods; ++i)
        if (!Exists(keys[i])) return false;
    return true;
}

void Verdict(const char* step, bool pass, const std::string& detail) {
    if (!pass) {
        ++g_fails;
        g_failed.push_back(step);
    }
    UE_LOGI("food-drill: %hs %hs %hs", step, pass ? "PASS" : "FAIL", detail.c_str());
}

// The steps this peer failed, as one line for the other peer: `<count> <step,step>`, `0 -` for none.
std::string FailText() {
    std::string names;
    for (const std::string& s : g_failed) names += (names.empty() ? "" : ",") + s;
    return std::to_string(g_fails) + " " + (names.empty() ? "-" : names);
}

int FailCount(const std::string& text) { return std::atoi(text.c_str()); }

std::string Describe(const F::Condition& c) {
    char line[96];
    std::snprintf(line, sizeof(line), "%.2f %.2f %d", c.temperature, c.ripeness, c.uses);
    return line;
}

bool Matches(const F::Condition& here, const F::Condition& host) {
    return here.uses == host.uses && std::fabs(here.temperature - host.temperature) <= kTemperatureSlack &&
           std::fabs(here.ripeness - host.ripeness) <= kRipenessSlack;
}

// The reader of a step answers it, so the writer's next change cannot land before the read.
void Ack(const char* step) { Mark((std::string("ack-") + step).c_str()); }

bool AwaitAck(const char* step) { return Await((std::string("ack-") + step).c_str()); }

// The host's half of a step: `text` goes in the folder under the step's name, and the client's read of it is awaited.
bool Announce(const char* step, const std::string& text) {
    Mark(step, text);
    return AwaitAck(step);
}

// The host's numbers go in the folder under the step's name.
bool Publish(const char* step, const std::string& key) {
    F::Condition c;
    if (ReadFood(key, c)) {
        Verdict(step, true, "host " + Describe(c));
        return Announce(step, Describe(c));
    }
    Verdict(step, false, "the food is gone on the host");
    return Announce(step, "gone");
}

// The client's half: the same food, once it has reached the host's numbers, read against them. A copy that never
// does is judged as it stands.
bool Compare(const char* step, const std::string& key) {
    std::string body;
    if (!AwaitText(step, body)) return false;
    F::Condition here;
    bool read = false;
    if (body == "live") {
        Verdict(step, false, "the host's lives");
    } else if (body == "gone") {
        WaitFor(step, kStepBoundSec, [&] { return !Exists(key); }, false);
        read = ReadFood(key, here);
        Verdict(step, !read, read ? "the host's is gone, this copy lives" : "gone on both");
    } else {
        std::istringstream in(body);
        F::Condition host;
        in >> host.temperature >> host.ripeness >> host.uses;
        WaitFor(step, kStepBoundSec, [&] { return ReadFood(key, here) && Matches(here, host); }, false);
        read = ReadFood(key, here);
        Verdict(step, read && Matches(here, host), "client " + (read ? Describe(here) : std::string("gone")) + " host " + body);
    }
    Ack(step);
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

// Whether a food's steam and frozen flag read as wanted, in a verdict of its own. A particle system reads active until
// its last particle has gone, so steam takes a while to stop.
void CheckLooks(const char* step, const std::string& key, bool steam, bool frozen) {
    bool readSteam = false, readFrozen = false, read = false;
    WaitFor(step, 20, [&] {
        OnGameThread([&] { read = F::ReadLooks(Find(key), readSteam, readFrozen); });
        return read && readSteam == steam && readFrozen == frozen;
    }, false);
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
        if (WaitFor("the food in the hand", 3, [&] {
                bool held = false;
                OnGameThread([&] { held = F::IsFood(coop::hand_item::LocalHandActor()); });
                return held;
            }, false))
            return true;
    }
    return false;
}

void WriteHand(const F::Condition& c) {
    OnGameThread([&] { F::Write(coop::hand_item::LocalHandActor(), c); });
}

// The copy this peer shows in `slot`'s hand, checked against what the holder wrote, with the steam on. That its own
// tick does not run is the gate's count, judged at the end (coop/props/food_tick_gate).
void CheckMirror(const char* step, uint8_t slot, const F::Condition& want) {
    F::Condition got;
    bool read = false, steam = false, frozen = false;
    const bool there = WaitFor(step, kHandBoundSec, [&] {
        OnGameThread([&] {
            void* mirror = coop::hand_item::MirrorActorForSlot(slot);
            read = F::Read(mirror, got) && F::ReadLooks(mirror, steam, frozen);
        });
        return read && std::fabs(got.temperature - want.temperature) <= kTemperatureSlack;
    }, false);
    Verdict(step, there && steam && got.uses == want.uses,
            (there ? Describe(got) : std::string("no food there at the written numbers")) + " steam=" +
                std::to_string(steam));
}

// What the food tick gate did on this peer over the whole run: no body ran against its rule, and on the client the gate
// refused some (a dead watch would also read zero ran). The host's refusals are its hand copies of the client's food.
void CheckTickGate(bool clientSide) {
    coop::food_tick_gate::Counts c;
    OnGameThread([&] { c = coop::food_tick_gate::GetCounts(); });
    Verdict("18-gate-refused-food-ticks", c.ran == 0 && (!clientSide || c.refused > 0),
            "refused " + std::to_string(c.refused) + " ran " + std::to_string(c.ran));
}

// This peer's player lets go of what it holds, thrown or placed down; the dropped food's key goes in the folder and
// comes back (`none` when nothing was dropped).
std::string LetGo(const char* keyFile, bool place) {
    std::string key;
    OnGameThread([&] {
        void* player = coop::players::Registry::Get().Local();
        if (!player) return;
        void* const before = E::ReadMainPlayerDroppedItem(player);
        if (place) {
            // simulateDrop(dontWakeup, place, dontCollect): the drop the R key and the inventory run, with the hand's
            // item spawned where the player aims.
            void* fn = R::FindDispatchFunctionCached(R::ClassOf(player), L"simulateDrop");
            ue_wrap::ParamFrame f(fn);
            if (fn && f.valid()) {
                f.Set<bool>(L"dontWakeup", false);
                f.Set<bool>(L"place", true);
                f.Set<bool>(L"dontCollect", true);
                ue_wrap::Call(player, f);
            }
        } else {
            ue_wrap::component_calls::CallParamlessNamed(player, L"throwHoldingProp");
        }
        void* dropped = E::ReadMainPlayerDroppedItem(player);
        if (dropped == before) dropped = nullptr;  // nothing was dropped: the field still names an older one
        const std::wstring k = dropped ? ue_wrap::prop::GetInteractableKeyString(dropped) : L"";
        key.assign(k.begin(), k.end());
    });
    if (key.empty()) key = "none";
    Mark(keyFile, key);
    return key;
}

// The dropped food here, against what its holder wrote into the hand; the copy has to exist and the uses have to match.
void CheckLetGo(const char* step, const std::string& key, const F::Condition& want) {
    F::Condition got;
    auto lands = [&] { return ReadFood(key, got) && got.uses == want.uses && std::fabs(got.ripeness - want.ripeness) <= 30.f; };
    WaitFor(step, kStepBoundSec, lands, false);
    const bool read = ReadFood(key, got);
    Verdict(step, read && got.uses == want.uses && std::fabs(got.ripeness - want.ripeness) <= 30.f,
            (read ? Describe(got) : std::string("no such food")) + " wrote " + Describe(want));
}

// One way a held food leaves a hand, and the names of its steps and signals.
struct Leg {
    const char* tag;               // the signals' suffix
    bool        place;             // simulateDrop with place, else throwHoldingProp
    const char* clientHolds;       // the client's pickup
    const char* hostShowsHand;     // the host's mirror of the client's hand
    const char* clientKeeps;       // the client's own dropped food
    const char* hostTookIt;        // the host's copy of the client's dropped food
    const char* hostHolds;         // the host's pickup
    const char* clientShowsHand;   // the client's mirror of the host's hand
    const char* clientSawIt;       // the client's copy of the host's dropped food
};

constexpr Leg kThrow = {"throw", false, "10-client-holds", "10-host-shows-client-hand", "11-client-keeps-own",
                        "11-host-took-release", "12-host-holds", "12-client-shows-host-hand",
                        "13-client-saw-host-throw"};
constexpr Leg kPlace = {"place", true, "14-client-holds", "14-host-shows-client-hand", "15-client-keeps-placed",
                        "15-host-took-release", "16-host-holds", "16-client-shows-host-hand",
                        "17-client-saw-host-place"};

std::string Sig(const char* name, const Leg& leg) { return std::string(name) + "_" + leg.tag; }

constexpr F::Condition kClientHot = {340.f, 2500.f, 3};
constexpr F::Condition kClientLet = {300.f, 2500.f, 3};
constexpr F::Condition kHostHot = {340.f, 2600.f, 2};
constexpr F::Condition kHostLet = {300.f, 2600.f, 2};

// The host's half of a leg: the client holds and lets go, then the host holds `hostKey` and lets go.
bool HostLeg(const Leg& leg, const std::string& hostKey) {
    if (!Await(Sig("client_holds", leg).c_str())) return false;
    CheckMirror(leg.hostShowsHand, 1, kClientHot);
    Mark(Sig("host_saw_hand", leg).c_str());
    std::string dropped;
    if (!AwaitText(Sig("client_let_go", leg).c_str(), dropped)) return false;
    if (dropped == "none") Verdict(leg.hostTookIt, false, "no dropped food");
    else CheckLetGo(leg.hostTookIt, dropped, kClientLet);
    Mark(Sig("host_saw_let_go", leg).c_str());

    if (!TakeInHand(hostKey)) {
        Verdict(leg.hostHolds, false, "the pickup did not land");
        Mark(Sig("host_holds", leg).c_str(), "no");
    } else {
        WriteHand(kHostHot);
        Mark(Sig("host_holds", leg).c_str());
    }
    if (!Await(Sig("client_saw_host_hand", leg).c_str())) return false;
    WriteHand(kHostLet);
    ::Sleep(kHandDwellMs);
    LetGo(Sig("host_let_go", leg).c_str(), leg.place);
    return true;
}

// The client's half of the same leg.
bool ClientLeg(const Leg& leg, const std::string& clientKey) {
    if (!TakeInHand(clientKey)) {
        Verdict(leg.clientHolds, false, "the pickup did not land");
        Mark(Sig("client_holds", leg).c_str(), "no");
    } else {
        WriteHand(kClientHot);
        Mark(Sig("client_holds", leg).c_str());
    }
    if (!Await(Sig("host_saw_hand", leg).c_str())) return false;
    WriteHand(kClientLet);
    ::Sleep(kHandDwellMs);
    const std::string own = LetGo(Sig("client_let_go", leg).c_str(), leg.place);
    if (own != "none") CheckLetGo(leg.clientKeeps, own, kClientLet);
    if (!Await(Sig("host_saw_let_go", leg).c_str())) return false;

    if (!Await(Sig("host_holds", leg).c_str())) return false;
    CheckMirror(leg.clientShowsHand, 0, kHostHot);
    Mark(Sig("client_saw_host_hand", leg).c_str());
    std::string thrown;
    if (!AwaitText(Sig("host_let_go", leg).c_str(), thrown)) return false;
    if (thrown != "none") CheckLetGo(leg.clientSawIt, thrown, kHostLet);
    else Verdict(leg.clientSawIt, false, "no dropped food");
    return true;
}

// A food's ripeness has fallen a step from `from`: the host's tick has rolled the food on.
bool RolledOn(const std::string& key, const F::Condition& from) {
    F::Condition now;
    return ReadFood(key, now) && std::fabs(now.ripeness - from.ripeness) >= kRipenessStep;
}

void RunHost() {
    if (!Await("client_ready", kReadyBoundSec)) return;
    // Life 1's occupancy of slot 1: the rejoin waits for it to change, never for the slot to read closed.
    const uint32_t life1Gen = harness::session_runtime::Session().peerGenerationForSlot(1);
    std::string keys[kFoods];
    OnGameThread([&] {
        void* cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/prop_food.prop_food_C");
        void* local = coop::players::Registry::Get().Local();
        ue_wrap::FVector me{};
        if (!cls || !local || !E::TryGetActorLocation(local, me)) return;
        for (int i = 0; i < kFoods; ++i) {
            void* a = E::SpawnActor(cls, {me.X + 120.f, me.Y + i * 60.f, me.Z + 40.f});
            const std::wstring k = a ? ue_wrap::prop::GetInteractableKeyString(a) : L"";
            keys[i].assign(k.begin(), k.end());
        }
    });
    std::string line;
    for (const auto& k : keys) {
        if (k.empty() || k == "None") {
            UE_LOGW("food-drill: VERDICT ABORT -- the food did not spawn with a key");
            return;
        }
        line += (line.empty() ? "" : " ") + k;
    }
    if (!WaitFor("the foods indexed here", kStepBoundSec, [&] { return AllExist(keys); })) return;
    Mark("keys", line);
    if (!Await("client_saw_born")) return;

    if (!Publish("00-born", keys[0])) return;
    WriteFood(keys[0], {300.f, 3400.f, 5});
    if (!Publish("01-host-wrote", keys[0])) return;
    // The step's subject is time: the host's tick rolls the food on while the client's copy, its tick refused, holds its numbers.
    F::Condition wrote;
    ReadFood(keys[0], wrote);
    if (!WaitFor("the host's food to roll on", kStepBoundSec, [&] { return RolledOn(keys[0], wrote); }, false))
        UE_LOGW("food-drill: the host's food did not roll on in %d s -- step 02 is a plain hold", kStepBoundSec);
    if (!Publish("02-host-rolled-on", keys[0])) return;
    if (!Await("client_bit_a")) return;
    {
        F::Condition c;
        const bool ok = WaitFor("the host to take the client's bite", kStepBoundSec,
                                [&] { return ReadFood(keys[0], c) && c.uses == 4; }, false);
        Verdict("03-host-took-client-bite", ok, ok ? Describe(c) : "uses not 4");
    }
    if (!Publish("03-client-bit", keys[0])) return;
    Bite(keys[0]);
    {
        F::Condition c;
        const bool ok = WaitFor("the host's own bite", kStepBoundSec, [&] { return ReadFood(keys[0], c) && c.uses == 3; }, false);
        Verdict("04-host-bit-itself", ok, ok ? Describe(c) : "uses not 3");
    }
    if (!Publish("04-host-bit", keys[0])) return;
    WriteFood(keys[1], {300.f, 3400.f, 1});
    Mark("b_ready");
    if (!Await("client_bit_b")) return;
    {
        const bool gone = WaitFor("the host to take the last bite", kStepBoundSec, [&] { return !Exists(keys[1]); }, false);
        Verdict("05-host-took-last-bite", gone, gone ? "gone" : "the food lives");
    }
    if (!Announce("05-client-last-bite", Exists(keys[1]) ? "live" : "gone")) return;
    const int burntBefore = CountBurnt();
    WriteFood(keys[2], {1000.f, 3400.f, 3});
    int burntAfter = burntBefore;
    const bool burnt = WaitFor("the host's food to burn", kStepBoundSec, [&] {
        burntAfter = CountBurnt();
        return burntAfter == burntBefore + 1 && !Exists(keys[2]);
    }, false);
    Verdict("06-host-burnt", burnt, "burnt " + std::to_string(burntBefore) + "->" + std::to_string(burntAfter));
    Mark("burn_counts", std::to_string(burntBefore) + " " + std::to_string(burntAfter));
    if (!Announce("06-burnt-food", "gone")) return;
    if (!Await("client_checked_d")) return;   // after its dwell: a client's write that was going to cross has crossed
    if (!Publish("07-client-burn-stays", keys[3])) return;

    // The looks of a client's copy: steam, then frozen, then neither.
    WriteFood(keys[3], {340.f, 3000.f, 4});
    Mark("hot");
    if (!Await("client_saw_hot")) return;
    WriteFood(keys[3], {250.f, 3000.f, 4});
    Mark("frozen");
    if (!Await("client_saw_frozen")) return;
    WriteFood(keys[3], {300.f, 3000.f, 4});
    Mark("thawed");
    if (!Await("client_saw_thawed")) return;

    // The client holds a food and lets it go, the host shows it and takes its release; then the host holds one and its
    // let-go reaches the client. Thrown first, placed down second.
    if (!HostLeg(kThrow, keys[4])) return;
    if (!HostLeg(kPlace, keys[6])) return;
    CheckTickGate(false);

    std::string clientText;
    if (!AwaitText("client_rows", clientText)) return;
    const int clientFails = FailCount(clientText);
    std::string hostSteps;
    for (const std::string& s : g_failed) hostSteps += (hostSteps.empty() ? "" : ",") + s;
    if (g_fails == 0 && clientFails == 0) UE_LOGI("food-drill: VERDICT PASS");
    else UE_LOGW("food-drill: VERDICT FAIL host=%d [%hs] client=[%hs]", g_fails, hostSteps.c_str(), clientText.c_str());
    UE_LOGI("food-drill: DONE on host fails=%d", g_fails);
    Mark("host_ack");   // now watching slot 1: life 1 may end

    if (!WaitFor("the client's life 1 gone from slot 1", kReadyBoundSec, [life1Gen] {
            auto& s = harness::session_runtime::Session();
            return !s.IsSlotConnected(1) || s.peerGenerationForSlot(1) != life1Gen;
        }))
        return;
    WriteFood(keys[0], {295.f, 3000.f, 2});
    // The rejoined life's drill begins when its process boots: its world is up and the signals below are written since.
    if (!Await("rejoin_ready", kReadyBoundSec)) return;
    Mark("rejoin_key", keys[0]);
    if (!Publish("08-while-away", keys[0])) return;
    std::string rejoinText;
    if (!AwaitText("rejoin_done", rejoinText)) return;
    const int rejoinFails = FailCount(rejoinText);
    if (g_fails == 0 && clientFails == 0 && rejoinFails == 0) UE_LOGI("food-drill: VERDICT PASS after the rejoin");
    else UE_LOGW("food-drill: VERDICT FAIL after the rejoin host=%d client=%d rejoin=[%hs]", g_fails, clientFails, rejoinText.c_str());
    UE_LOGI("food-drill: REJOIN DONE on host fails=%d", g_fails);
}

// The world is up, the host's puppet is here, the join's load tail has quiesced (a client's change before it is not
// published, as prop_food_sync's pending list holds), and the client stands beside the host, where the foods are spawned.
bool ClientReady() {
    if (!WaitFor("the world announced", 240, [&] {
            bool ready = false;
            OnGameThread([&] { ready = coop::net_pump::HasAnnouncedWorldReady(); });
            return ready;
        }))
        return false;
    if (!WaitFor("the host's puppet", 60, [&] {
            bool have = false;
            OnGameThread([&] {
                coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
                have = p && p->valid();
            });
            return have;
        }))
        return false;
    if (!WaitFor("the join's load tail to quiesce", 180, [&] {
            bool quiet = false;
            OnGameThread([&] { quiet = coop::join_membership_sweep::HasLoadTailQuiesced(); });
            return quiet;
        }))
        return false;
    return meet::WalkToHost("food-drill");
}

// Life 1 leaves as a player's quit does (network first), so the host sees slot 1 empty at once: a killed process only
// stops sending, and the host learns of it when the next life connects.
void LeaveForRejoin() {
    OnGameThread([] {
        auto& s = harness::session_runtime::Session();
        if (s.running()) coop::net_pump::FleeAfterNativeMenuTravel(s, "food drill: life 1 leaves for the rejoin");
    });
    WaitFor("this life's session stopped", 15, [] { return !harness::session_runtime::Session().running(); }, false);
}

void RunClient() {
    if (!ClientReady()) return;
    Mark("client_ready");
    std::string keyLine;
    if (!AwaitText("keys", keyLine)) return;
    std::istringstream in(keyLine);
    std::string keys[kFoods];
    for (auto& k : keys) in >> k;
    if (!WaitFor("the foods here", kStepBoundSec, [&] { return AllExist(keys); })) return;
    Mark("client_saw_born");

    if (!Compare("00-born", keys[0])) return;
    if (!Compare("01-host-wrote", keys[0])) return;
    if (!Compare("02-host-rolled-on", keys[0])) return;
    Bite(keys[0]);
    Mark("client_bit_a");
    if (!Compare("03-client-bit", keys[0])) return;
    if (!Compare("04-host-bit", keys[0])) return;
    if (!Await("b_ready")) return;
    // The copy has the host's one use before the bite, or the bite is not the last one.
    if (!WaitFor("the one-use food here", kStepBoundSec, [&] {
            F::Condition c;
            return ReadFood(keys[1], c) && c.uses == 1;
        }))
        return;
    Bite(keys[1]);
    Mark("client_bit_b");
    if (!Compare("05-client-last-bite", keys[1])) return;
    std::string counts;
    if (!AwaitText("burn_counts", counts)) return;
    {
        std::istringstream c(counts);
        int before = 0, after = 0;
        c >> before >> after;
        WaitFor("the burnt food here", kStepBoundSec, [&] { return CountBurnt() == after; }, false);
        Verdict("06-one-burnt-on-client", CountBurnt() == after,
                "client " + std::to_string(CountBurnt()) + " host " + std::to_string(after));
    }
    if (!Compare("06-burnt-food", keys[2])) return;
    WriteFood(keys[3], {1000.f, 3400.f, 3});
    Mark("client_wrote_d");
    ::Sleep(kNoBurnDwellMs);
    Verdict("07-parked-copy-did-not-burn", Exists(keys[3]), Exists(keys[3]) ? "it lives" : "it burnt on its own");
    Mark("client_checked_d");
    std::string d;
    if (!AwaitText("07-client-burn-stays", d)) return;
    Verdict("07-host-copy-lives", d != "gone", d);
    Ack("07-client-burn-stays");

    if (!Await("hot")) return;
    CheckLooks("09-parked-steam", keys[3], true, false);
    Mark("client_saw_hot");
    if (!Await("frozen")) return;
    CheckLooks("09-parked-frozen", keys[3], false, true);
    Mark("client_saw_frozen");
    if (!Await("thawed")) return;
    CheckLooks("09-parked-thawed", keys[3], false, false);
    Mark("client_saw_thawed");

    if (!ClientLeg(kThrow, keys[3])) return;
    if (!ClientLeg(kPlace, keys[5])) return;
    CheckTickGate(true);

    Mark("client_rows", FailText());
    if (!Await("host_ack")) return;
    LeaveForRejoin();
    UE_LOGI("food-drill: DONE on client fails=%d", g_fails);
}

void RunRejoin() {
    if (!ClientReady()) return;
    Mark("rejoin_ready");
    std::string keyA;
    if (!AwaitText("rejoin_key", keyA)) return;
    if (!Compare("08-while-away", keyA)) return;
    Mark("rejoin_done", FailText());
    UE_LOGI("food-drill: REJOIN DONE fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI FoodStateThread(LPVOID /*arg*/) {
    if (!PS::Begin("food-drill")) return 0;
    if (!IsClientRole()) RunHost();
    else if (coop::config::ReadEnv("VOTVCOOP_FOOD_REJOIN") == "1") RunRejoin();
    else RunClient();
    return 0;
}

}  // namespace harness::autotest

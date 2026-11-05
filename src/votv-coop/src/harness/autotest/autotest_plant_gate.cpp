// harness/autotest/autotest_plant_gate.cpp -- TEST-ONLY: the plant-state drill's arms that hold what a client refuses.
//
// A sprinkler the host spawned beside a plant, switched on here, adds no water to the client's copy; the client's own
// spawnBush calls are refused and plant nothing; a lethal hit on a pine here is refused and the copy ends with the host's
// break; a growth area the host spawned glows at the plant it picked, here too; and at the end no plant tick ran here
// while the gate refused them (coop/world/plant_gate, coop/world/spawn_authority, coop/props/prop_damage_gate,
// coop/world/event_cue_sync). The flows that call them are autotest_plant_state.cpp's.

#include "harness/autotest/peer_signal.h"
#include "plant_state_drill.h"   // co-located private header (src tree, not include/)

#include "coop/props/prop_damage_gate.h"
#include "coop/world/event_cue_sync.h"
#include "coop/world/plant_gate.h"
#include "coop/world/spawn_authority.h"
#include "ue_wrap/actors/growing_plant.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace harness::autotest::plant_drill {
namespace {

namespace GP = ue_wrap::growing_plant;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace PS = harness::autotest::peer_signal;

constexpr const wchar_t* kSprinklerClass = L"prop_sprinkler_C";
constexpr const wchar_t* kSprinklerPath = L"/Game/objects/prop_sprinkler.prop_sprinkler_C";
constexpr const wchar_t* kGrowthClass = L"growthArea_C";
constexpr const wchar_t* kGrowthPath = L"/Game/objects/misc/growthArea.growthArea_C";
constexpr const char* kBushSpawnTag = "bushSpawning.spawnBush";   // spawn_authority's row of the winter bush ticker

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

// Whether some plant stands within a few units of `at`.
bool AtAPlant(const float (&at)[3]) {
    std::vector<void*> plants;
    GP::List(plants);
    for (void* p : plants) {
        ue_wrap::FVector l{};
        if (E::TryGetActorLocation(p, l) && Near(l.X, at[0], 5.f) && Near(l.Y, at[1], 5.f) && Near(l.Z, at[2], 5.f)) return true;
    }
    return false;
}

}  // namespace

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

// HOST: a growth area by script, at the place of a plant that has water and no dryness. nullptr when it did not stand.
void* SpawnGrowthArea(const std::string& plantKey) {
    void* area = nullptr;
    OnGameThread([&] {
        ue_wrap::asset_load::LoadObjectByPath(kGrowthPath);
        void* cls = R::FindClass(kGrowthClass);
        void* plant = ByKey(plantKey);
        ue_wrap::FVector at{};
        if (!cls || !plant || !E::TryGetActorLocation(plant, at)) return;
        void* a = E::BeginDeferredSpawn(cls, at, ue_wrap::FRotator{});
        if (a && E::FinishDeferredSpawn(a, at, ue_wrap::FRotator{})) area = a;
    });
    return area;
}

// HOST: the growth area's own tick, called until its glow is sent. The body looks at one random plant of those in its box
// and does nothing for one that is dead or dry, and the game's own tick comes every 20 s, so the drill calls it at its own
// pace; each call waits for the glow to be sent, and the tries are bounded. Returns "<x> <y> <z>" of the sent place, or
// "none".
std::string TickGrowthArea(void* area) {
    constexpr int kTries = 40;
    const unsigned was = coop::event_cue_sync::SentCount();
    int tries = 0;
    bool sent = false;
    while (tries < kTries && !sent) {
        ++tries;
        OnGameThread([&] {
            void* fn = R::IsLive(area) ? R::FindDispatchFunctionCached(R::ClassOf(area), L"ReceiveTick") : nullptr;
            ue_wrap::ParamFrame f(fn);
            if (!fn || !f.valid()) return;
            f.Set<float>(L"DeltaSeconds", 0.f);
            ue_wrap::Call(area, f);
        });
        sent = PS::WaitFor("the glow sent", 1, [&] { return coop::event_cue_sync::SentCount() > was; }, false);
    }
    float at[3] = {};
    bool placed = false;
    OnGameThread([&] {
        coop::event_cue_sync::LastSentAt(at);
        placed = sent && AtAPlant(at);
    });
    UE_LOGI("plant-state: growth area ticked %d times, glow sent=%d at (%.0f, %.0f, %.0f)", tries, sent ? 1 : 0, at[0], at[1],
            at[2]);
    Check(sent, "the growth area's tick sent its glow to the peer");
    Check(placed, "the glow was sent at the place of a plant");
    if (!sent) return "none";
    return std::to_string(at[0]) + " " + std::to_string(at[1]) + " " + std::to_string(at[2]);
}

// CLIENT: the host's glow replays here, at the place the host sent. `base` is the replay count before the host's tick.
bool GlowArm(unsigned base, const std::string& sentAt) {
    float want[3] = {};
    std::istringstream in(sentAt);
    if (!Check(static_cast<bool>(in >> want[0] >> want[1] >> want[2]), "the host sent its growth area's glow")) return false;
    const bool replayed = Check(PS::WaitFor("the glow replayed", 20, [&] { return coop::event_cue_sync::ReplayCount() > base; }, false),
                                "the host's growth glow replayed here");
    float got[3] = {};
    OnGameThread([&] { coop::event_cue_sync::LastReplayAt(got); });
    UE_LOGI("plant-state: glow replayed at (%.0f, %.0f, %.0f), host sent (%.0f, %.0f, %.0f)", got[0], got[1], got[2], want[0],
            want[1], want[2]);
    const bool placed = Check(Near(got[0], want[0], 2.f) && Near(got[1], want[1], 2.f) && Near(got[2], want[2], 2.f),
                              "the glow replayed at the place the host sent");
    return replayed && placed;
}

}  // namespace harness::autotest::plant_drill

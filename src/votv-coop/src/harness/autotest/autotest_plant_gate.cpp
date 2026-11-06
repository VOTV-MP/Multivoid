// harness/autotest/autotest_plant_gate.cpp -- TEST-ONLY: the plant-state drill's arms that hold what a client refuses.
//
// A sprinkler the host spawned beside a plant, switched on here, adds no water to the client's copy; the client's own
// spawnBush calls are refused and plant nothing; a lethal hit on a pine here is refused and the copy ends with the host's
// break; a growth area the host spawned glows at the plant it picked, here too; and at the end no plant tick ran here
// while the gate refused them (coop/world/plant_gate, coop/world/spawn_authority, coop/props/prop_damage_gate,
// coop/world/event_cue_sync). The last arm is the host's edit gate: a client sends six plantings and two edits no player's
// act could make (a class that is no plant, a place 50000 uu away, an unknown row, no key, the key "None", a pot out of reach;
// a plant's row changed to an unknown one, its water set past every bound), and the host refuses or cuts each
// (coop/world/plant_edit_gate). The flows that call them are autotest_plant_state.cpp's.

#include "harness/autotest/peer_signal.h"
#include "plant_state_drill.h"   // co-located private header (src tree, not include/)

#include "harness/session_runtime.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_damage_gate.h"
#include "coop/world/event_cue_sync.h"
#include "coop/world/plant_edit_gate.h"
#include "coop/world/plant_gate.h"
#include "coop/world/plant_sync.h"
#include "coop/world/spawn_authority.h"
#include "ue_wrap/actors/growing_plant.h"
#include "ue_wrap/actors/prop.h"
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

// CLIENT: a sprinkler switched on beside a plant. Its 0.1 s watering timer is refused here, so the copy's water never rises
// above the last value the host's state gave it (plant_sync's inbox, which this client's own plant never reaches): each
// sample reads the copy and that value in one game-thread step, since the host's plant keeps changing and its news arrives
// whole. The window is 30 refused firings of the timer, not a time.
bool SprinklerArm(const std::string& plantKey) {
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
    float excess = 0.f;   // the most the copy's water stood above the host's last value
    int samples = 0;
    const bool fired = PS::WaitFor("the sprinkler's timer", 20, [&] {
        OnGameThread([&] {
            refused = coop::plant_gate::GetCounts().sprinklerWater - before;
            void* p = ByKey(plantKey);
            GP::Values v, host;
            if (p && GP::Read(p, v) && coop::plant_sync::LastHostValues(Widen(plantKey), host)) {
                ++samples;
                excess = (std::max)(excess, v.water - host.water);
            }
        });
        return refused >= kFirings;
    }, false);
    OnGameThread([&] { SetSprinklersActive(false); });
    UE_LOGI("plant-state: sprinkler arm refused %llu timer firings, %d samples, copy water above the host's last value by at most %.1f",
            static_cast<unsigned long long>(refused), samples, excess);
    const bool timer = Check(fired, "this client refused the sprinkler's watering timer");
    const bool held = Check(samples > 0 && excess <= 1.f, "a switched-on sprinkler added no water to this client's copy");
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
    namespace PDG = coop::props::prop_damage_gate;
    // The pine's addDamage hands the call to its physicsImpact component's impactDamage, so the one hit is one refused impact on
    // a plant: exactly one on each count, where a bump on some other plant would make two.
    PDG::Counts was, now;
    size_t logsWas = 0, logsNow = 0;
    bool stands = false;
    OnGameThread([&] {
        was = PDG::GetCounts();
        logsWas = CountOf(L"prop_food_C");
    });
    const bool called = LethalHit(key);
    OnGameThread([&] {
        now = PDG::GetCounts();
        logsNow = CountOf(L"prop_food_C");
        stands = ByKey(key) != nullptr;
    });
    const unsigned long long impacts = now.impact - was.impact, plants = now.plant - was.plant;
    UE_LOGI("plant-state: pine hit called=%d, refused impacts %llu, refused on plants %llu, logs %zu->%zu, copy stands=%d",
            called ? 1 : 0, impacts, plants, logsWas, logsNow, stands ? 1 : 0);
    const bool refused = Check(called && impacts == 1 && plants == 1, "this client's lethal hit on the pine was refused, once");
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

// ---- the host's edit gate (coop/world/plant_edit_gate): a client's crafted plantings and edits ----------------------------------
// The keys of the crafted plantings; the host must hold no plant under any. A planting under an empty key or "None" cannot be
// looked up by key, so the host's plant count says it was not planted.
constexpr const char* kCraftedNonPlantKey = "F1DRILL_NONPLANT";
constexpr const char* kCraftedFarKey = "F1DRILL_FAR";
constexpr const char* kCraftedRowKey = "F1DRILL_ROW";
constexpr const char* kCraftedPotKey = "F1DRILL_POT";
constexpr const wchar_t* kNoSuchRow = L"no_such_row";
constexpr float kFarUU = 50000.f;   // far beyond any reach the host's authority allows (400 + bounds + 600)
constexpr float kPotAboveUU = 8000.f;   // a pot this far above the host's player is out of every reach the gate allows
constexpr float kWaterBeyondUU = 1.0e9f;   // far past any water a plant holds (the gate's bound is 20000)
constexpr const wchar_t* kPlantClass = L"growingPlant_C";
constexpr const wchar_t* kPotClass = L"prop_farmPot_C";
constexpr const wchar_t* kPotPath = L"/Game/objects/prop_farmPot.prop_farmPot_C";

bool SendPayload(const coop::net::PlantStatePayload& p) {
    return harness::session_runtime::Session().SendReliable(coop::net::ReliableKind::PlantEdit, &p, sizeof(p));
}

bool SendCrafted(const wchar_t* key, const std::wstring& cls, const std::wstring& row, const ue_wrap::FVector& at,
                 const std::wstring& parent = L"") {
    coop::net::PlantStatePayload p{};
    coop::net::WireKeyFromString(key, p.key);
    coop::net::WireKeyFromString(row, p.row);
    coop::net::WireKeyFromString(parent, p.attachParent);
    p.cls.len = static_cast<uint8_t>((std::min)(cls.size(), sizeof(p.cls.data)));
    for (size_t i = 0; i < p.cls.len; ++i) p.cls.data[i] = static_cast<char>(cls[i]);
    p.x = at.X;
    p.y = at.Y;
    p.z = at.Z;
    p.grow = 0.1f;
    p.alive = 1;
    p.hideDirt = 1;
    return SendPayload(p);
}

// An edit of a plant the host holds: only the masked field is meant, the rest is zero.
bool SendCraftedEdit(const std::string& plantKey, uint8_t mask, const std::wstring& row, float water) {
    coop::net::PlantStatePayload p{};
    coop::net::WireKeyFromString(Widen(plantKey), p.key);
    coop::net::WireKeyFromString(row, p.row);
    p.editMask = mask;
    p.water = water;
    p.alive = 1;
    return SendPayload(p);
}

// CLIENT: PlantEdits no player's act could make, sent as plant_sync sends its own. Plantings: one names a loaded class that is no
// plant (the class of a prop standing here) at this player's own place; one a plant class 50000 uu away; one a plant class at this
// player's own place with a row that is none of list_farmPlants'; one with no key; one under the key "None"; one in the host's pot
// 8000 uu above the host's player. Edits of the host's plant `plantKey`: its row changed to none of list_farmPlants', and its
// water set to a billion.
bool CraftArm(const std::string& row, const std::string& potKey, const std::string& plantKey) {
    std::wstring nonPlant, rowClass;
    ue_wrap::FVector me{};
    bool placed = false;
    OnGameThread([&] {
        void* pawn = coop::players::Registry::Get().Local();
        void* prop = FirstOf(kSprinklerClass);
        placed = pawn && prop && E::TryGetActorLocation(pawn, me);
        if (prop) nonPlant = R::ClassNameOf(prop);
        // The far planting is right in all but its place: it names the class its row plants, read from this peer's own table.
        if (void* cls = GP::FarmRowPlantClass(Widen(row))) rowClass = R::ToString(R::NameOf(cls));
    });
    if (!Check(placed && !nonPlant.empty(), "this client has a prop of a non-plant class beside it to name")) return false;
    if (!Check(!rowClass.empty(), "this client reads the class the bush's row plants")) return false;
    if (!Check(potKey != "-", "the host named a pot to plant in")) return false;
    bool sent = false;
    OnGameThread([&] {
        ue_wrap::FVector atNear = me;
        atNear.X += 200.f;
        ue_wrap::FVector atFar = me;
        atFar.X += kFarUU;
        const std::wstring plantRow = Widen(row);
        sent = SendCrafted(Widen(kCraftedNonPlantKey).c_str(), nonPlant, plantRow, atNear);
        sent = SendCrafted(Widen(kCraftedFarKey).c_str(), rowClass, plantRow, atFar) && sent;
        sent = SendCrafted(Widen(kCraftedRowKey).c_str(), kPlantClass, kNoSuchRow, atNear) && sent;
        sent = SendCrafted(L"", rowClass, plantRow, atNear) && sent;
        sent = SendCrafted(L"None", rowClass, plantRow, atNear) && sent;
        sent = SendCrafted(Widen(kCraftedPotKey).c_str(), rowClass, plantRow, atNear, Widen(potKey)) && sent;
        sent = SendCraftedEdit(plantKey, coop::net::plant_edit::kRow, kNoSuchRow, 0.f) && sent;
        sent = SendCraftedEdit(plantKey, coop::net::plant_edit::kWater, plantRow, kWaterBeyondUU) && sent;
    });
    UE_LOGI("plant-state: crafted edits sent=%d: class '%ls' at this player's place, '%ls' %.0f uu away, row '%ls', no key, key None, "
            "pot '%hs', and two edits of '%hs' (row '%ls', water %.0f)",
            sent ? 1 : 0, nonPlant.c_str(), rowClass.c_str(), kFarUU, kNoSuchRow, potKey.c_str(), plantKey.c_str(), kNoSuchRow,
            static_cast<double>(kWaterBeyondUU));
    return Check(sent, "this client sent all eight crafted edits");
}

// HOST: a pot far above the host's player, spawned by the script. "" when it is not made or keyless.
std::string SpawnPotAbove() {
    std::string key;
    OnGameThread([&] {
        ue_wrap::asset_load::LoadObjectByPath(kPotPath);
        void* cls = R::FindClass(kPotClass);
        void* pawn = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!cls || !pawn || !E::TryGetActorLocation(pawn, at)) return;
        at.Z += kPotAboveUU;
        void* pot = E::BeginDeferredSpawn(cls, at, ue_wrap::FRotator{});
        if (pot && E::FinishDeferredSpawn(pot, at, ue_wrap::FRotator{}))
            key = Narrow(ue_wrap::prop::GetInteractableKeyString(pot));  // a pot is a prop: its key is the prop key, not actor_save's
    });
    return key;
}

// HOST: what the gate and the world hold before the client crafts, and the pot the client will name.
CraftBase CraftBegin(const std::string& plantKey) {
    CraftBase b;
    b.plantKey = plantKey;
    OnGameThread([&] {
        b.counts = coop::plant_edit_gate::GetCounts();
        b.nonPlants = CountOf(kSprinklerClass);
        std::vector<void*> plants;
        GP::List(plants);
        b.plants = plants.size();
        if (void* p = ByKey(plantKey)) b.plantRow = Narrow(GP::Row(p));
    });
    b.potKey = SpawnPotAbove();
    Check(!b.potKey.empty(), "the script spawned a pot above the host's player");
    return b;
}

// HOST: the gate refused the class once, the reach once, the unknown row twice (a planting's and an edit's), the key twice (none
// and "None"), the pot once, and cut the water once; and nothing of any planting appeared, the edited plant kept its row, and its
// water went in at the bound.
bool CraftJudge(const CraftBase& base) {
    coop::plant_edit_gate::Counts c;
    const coop::plant_edit_gate::Counts& was = base.counts;
    const bool judged = PS::WaitFor("the crafted edits judged", 20, [&] {
        OnGameThread([&] { c = coop::plant_edit_gate::GetCounts(); });
        return c.refusedClass > was.refusedClass && c.refusedReach > was.refusedReach && c.refusedRow >= was.refusedRow + 2 &&
               c.refusedKey >= was.refusedKey + 2 && c.refusedParent > was.refusedParent && c.clamped > was.clamped;
    }, false);
    size_t nonPlants = 0, plants = 0;
    bool keyed = false;
    std::string row;
    float water = 0.f;
    OnGameThread([&] {
        nonPlants = CountOf(kSprinklerClass);
        keyed = ByKey(kCraftedNonPlantKey) != nullptr || ByKey(kCraftedFarKey) != nullptr || ByKey(kCraftedRowKey) != nullptr ||
                ByKey(kCraftedPotKey) != nullptr;
        std::vector<void*> list;
        GP::List(list);
        plants = list.size();
        if (void* p = ByKey(base.plantKey)) {
            row = Narrow(GP::Row(p));
            GP::Values v;
            if (GP::Read(p, v)) water = v.water;
        }
    });
    const unsigned long long dClass = c.refusedClass - was.refusedClass, dReach = c.refusedReach - was.refusedReach,
                             dRow = c.refusedRow - was.refusedRow, dKey = c.refusedKey - was.refusedKey,
                             dParent = c.refusedParent - was.refusedParent, dClamped = c.clamped - was.clamped;
    UE_LOGI("plant-state: crafted edits refused for class %llu, reach %llu, row %llu, key %llu and pot %llu, values cut %llu; non-plant "
            "actors %zu->%zu, plants %zu->%zu, plant under a crafted key=%d, edited plant row '%hs' (was '%hs') water %.0f",
            dClass, dReach, dRow, dKey, dParent, dClamped, base.nonPlants, nonPlants, base.plants, plants, keyed ? 1 : 0, row.c_str(),
            base.plantRow.c_str(), static_cast<double>(water));
    const bool counted = Check(judged && dClass == 1 && dReach == 1 && dRow == 2 && dKey == 2 && dParent == 1 && dClamped == 1,
                               "the host refused the crafted class, place, rows, keys and pot, and cut the crafted water, each as many times as sent");
    const bool none = Check(nonPlants == base.nonPlants && plants == base.plants && !keyed,
                            "the host spawned nothing of any crafted planting");
    const bool held = Check(row == base.plantRow && water > 10000.f && water <= 20000.5f,
                            "the edited plant kept its row and took the water at the gate's bound");
    return counted && none && held;
}

}  // namespace harness::autotest::plant_drill

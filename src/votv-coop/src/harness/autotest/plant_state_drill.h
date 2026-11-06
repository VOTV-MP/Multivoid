// harness/autotest/plant_state_drill.h -- the plant-state drill's shared steps: what both of its files read the world with
// and the arms of autotest_plant_gate.cpp that the host's and the client's flows call. Private to the drill
// (autotest_plant_state.cpp, autotest_plant_gate.cpp).

#pragma once

#include "harness/autotest/peer_signal.h"
#include "coop/world/plant_edit_gate.h"
#include "coop/world/plant_sync.h"
#include "ue_wrap/actors/growing_plant.h"
#include "ue_wrap/core/game_thread.h"

#include <atomic>
#include <cmath>
#include <string>

namespace harness::autotest::plant_drill {

inline constexpr const wchar_t* kPineClass = L"growingPlant_pine_C";

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

// Logs `plant-state: PASS|FAIL <what>` and counts a failure.
bool Check(bool ok, const char* what);
int Fails();

// A key or a class name between the narrow text the peers' files carry and the engine's wide one.
std::string Narrow(const std::wstring& w);
std::wstring Widen(const std::string& s);

inline bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// The first live instance of a class, and how many there are; nothing of a Default__ object.
void* FirstOf(const wchar_t* cls);
size_t CountOf(const wchar_t* cls);

// The plant of a key. Game thread only.
void* ByKey(const std::string& key);

// Waits for a plant here to meet a test; a miss is the caller's Check, not an abort.
template <class Test>
bool WaitPlant(const std::string& key, Test&& test, int seconds = 15) {
    return peer_signal::WaitFor("a plant's state", seconds, [&] {
        bool ok = false;
        OnGameThread([&] {
            void* p = ByKey(key);
            ue_wrap::growing_plant::Values v;
            if (p && ue_wrap::growing_plant::Read(p, v)) ok = test(p, v);
        });
        return ok;
    }, false);
}

// HOST: a sprinkler beside the plant, left switched off.
bool SpawnSprinklerBeside(const std::string& plantKey);

// CLIENT: each arm returns whether it held.
bool SprinklerArm(const std::string& plantKey);
bool SpawnerArm();
bool TickArm();
bool PineHitArm(const std::string& key);
bool GlowArm(unsigned base, const std::string& sentAt);

// The edit gate's arm. CLIENT: sends plantings no player's act could make (a non-plant class at its own place, a plant class
// 50000 uu away, a row that is none of the host's, no key, the key "None", a pot out of reach) and two edits of the host's plant
// `plantKey` (an unknown row, water past every bound); `row` is a row the host's plants use and `potKey` the pot the host spawned
// ("-" when none). HOST: CraftBegin(plantKey) spawns the pot and takes the counts before the client crafts, CraftJudge after: the
// host's refusals and cuts rose by exactly what was sent, nothing of the plantings stands, and the edited plant kept its row.
struct CraftBase {
    coop::plant_edit_gate::Counts counts;
    size_t nonPlants = 0;
    size_t plants = 0;
    std::string plantKey, plantRow, potKey;
};
bool CraftArm(const std::string& row, const std::string& potKey, const std::string& plantKey);
CraftBase CraftBegin(const std::string& plantKey);
bool CraftJudge(const CraftBase& base);

// HOST: addDamage with damage no health survives; whether the call was made.
bool LethalHit(const std::string& key);

// HOST: a growth area at the place of a plant, and its tick called until its glow is sent ("<x> <y> <z>" or "none").
void* SpawnGrowthArea(const std::string& plantKey);
std::string TickGrowthArea(void* area);

}  // namespace harness::autotest::plant_drill

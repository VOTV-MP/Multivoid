// coop/props/food_tick_gate.cpp -- see coop/props/food_tick_gate.h.

#include "coop/props/food_tick_gate.h"

#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <iterator>

namespace coop::food_tick_gate {
namespace {

namespace SG = ue_wrap::script_gate;

// Every prop_food_C class that declares its own ReceiveTick, from the Blueprint dumps: the base, the cooking food
// (the egg under it declares none), the fish under it, the dingus and the coffee cup. The other subclasses declare
// none and run the base's body, which the first row covers.
UE_FACT_FN(kRowFoodTick, "food_tick_gate", L"prop_food_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowCookingFoodTick, "food_tick_gate", L"prop_cookingFood_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowFishTick, "food_tick_gate", L"prop_fish_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowDingusTick, "food_tick_gate", L"prop_dingus_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowCoffeeCupTick, "food_tick_gate", L"prop_food_coffeeCup_C", L"ReceiveTick", Declared);

struct RowDef {
    const ue_wrap::facts::Function* row;
    const char* tag;
};
constexpr RowDef kRows[] = {
    {&kRowFoodTick,       "food.ReceiveTick"},
    {&kRowCookingFoodTick, "cookingFood.ReceiveTick"},
    {&kRowFishTick,       "fish.ReceiveTick"},
    {&kRowDingusTick,     "dingus.ReceiveTick"},
    {&kRowCoffeeCupTick,  "coffeeCup.ReceiveTick"},
};
constexpr int kRowCount = static_cast<int>(std::size(kRows));
constexpr int kTagBase = 0x46540000;   // 'FT', then the row

// Per row, for the log: the first refusal and each power of two, so a refused tick proves itself without a line a
// frame. Game thread, as the gate's callbacks are.
std::uint64_t g_refused[kRowCount] = {};
std::uint64_t g_ran = 0;

// The copy shown in another slot's hand: hand_item spawns it and nothing else owns its numbers.
bool IsRemoteHandMirror(void* food) {
    for (std::uint8_t slot = 0; slot < coop::players::kMaxPeers; ++slot)
        if (coop::hand_item::MirrorActorForSlot(slot) == food) return true;
    return false;
}

// The rule. A client's own held food keeps its tick (FoodHand: the holder's food is its own); the other peers' hand
// copies never run theirs.
bool Refuses(void* food) {
    if (coop::spawn_authority::IsActiveClientSession()) return !coop::hand_item::IsLocalHandViewActor(food);
    return IsRemoteHandMirror(food);
}

SG::Verdict OnRowPre(const SG::Call& call) {
    if (!Refuses(call.object)) return SG::Verdict::Run;
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount) return SG::Verdict::Run;
    const std::uint64_t n = ++g_refused[row];
    if ((n & (n - 1)) == 0)
        UE_LOGI("food_tick_gate[%s]: refuse %p (call #%llu) -- the host runs the foods", kRows[row].tag, call.object,
                static_cast<unsigned long long>(n));
    return SG::Verdict::Cancel;
}

// The gate skips the post of a cancelled body, so this counts the food ticks that ran against the rule.
void OnPost(const SG::Call& call) {
    if (Refuses(call.object)) ++g_ran;
}

std::atomic<bool> g_watched{false};

}  // namespace

void Install() {
    if (g_watched.exchange(true, std::memory_order_acq_rel)) return;
    int watched = 0;
    for (int i = 0; i < kRowCount; ++i) {
        if (SG::WatchName(*kRows[i].row, kTagBase + i, &OnRowPre, &OnPost)) {
            ++watched;
        } else {
            UE_LOGE("food_tick_gate: the watch on %ls::%ls did not register -- a client runs it", kRows[i].row->Class(),
                    kRows[i].row->Name());
        }
    }
    UE_LOGI("food_tick_gate: %d/%d food tick watches registered by class and name; refused on an active client "
            "session and on another slot's hand copy", watched, kRowCount);
}

void OnDisconnect() {
    const Counts c = GetCounts();
    if (c.refused != 0 || c.ran != 0)
        UE_LOGI("food_tick_gate: this session refused %llu food ticks; %llu ran against the rule",
                static_cast<unsigned long long>(c.refused), static_cast<unsigned long long>(c.ran));
    for (std::uint64_t& n : g_refused) n = 0;
    g_ran = 0;
}

Counts GetCounts() {
    Counts c;
    for (const std::uint64_t n : g_refused) c.refused += n;
    c.ran = g_ran;
    return c;
}

}  // namespace coop::food_tick_gate

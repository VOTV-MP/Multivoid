// coop/world/plant_gate.cpp -- see coop/world/plant_gate.h.

#include "coop/world/plant_gate.h"

#include "coop/world/spawn_authority.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <iterator>

namespace coop::plant_gate {
namespace {

namespace SG = ue_wrap::script_gate;

// growingPlant.cpp: ReceiveTick is the one tick of every plant class (growth, rain, drying, death, addFruit). The herb's
// `grown` is the stolasHerb override that the base's empty `grown` leaves to it. dowater is the sprinkler's timer body
// (prop_sprinkler.cpp, the 0.1 s delegate bound at BeginPlay), not its ReceiveTick, which only spins the head.
UE_FACT_FN(kRowPlantTick, "plant_gate", L"growingPlant_C", L"ReceiveTick", Declared);
UE_FACT_FN(kRowHerbGrown, "plant_gate", L"growingPlant_stolasHerb_C", L"grown", Declared);
UE_FACT_FN(kRowSprinklerWater, "plant_gate", L"prop_sprinkler_C", L"dowater", Declared);
UE_FACT_FN(kRowGrowthAreaTick, "plant_gate", L"growthArea_C", L"ReceiveTick", Declared);

enum Row { kPlantTick, kHerbGrown, kSprinklerWater, kGrowthAreaTick };

struct RowDef {
    const ue_wrap::facts::Function* row;
    const char* tag;
};
constexpr RowDef kRows[] = {
    {&kRowPlantTick,       "plant.ReceiveTick"},
    {&kRowHerbGrown,       "stolasHerb.grown"},
    {&kRowSprinklerWater,  "sprinkler.dowater"},
    {&kRowGrowthAreaTick,  "growthArea.ReceiveTick"},
};
constexpr int kRowCount = static_cast<int>(std::size(kRows));
constexpr int kTagBase = 0x50470000;   // 'PG', then the row

// Per row, for the log and the drill's getter: the first refusal and each power of two, so a refused tick proves itself
// without a line a second. Game thread, as the gate's callbacks are.
std::uint64_t g_refused[kRowCount] = {};
std::uint64_t g_ran = 0;

SG::Verdict OnRowPre(const SG::Call& call) {
    if (!coop::spawn_authority::IsActiveClientSession()) return SG::Verdict::Run;
    const int row = call.tag - kTagBase;
    if (row < 0 || row >= kRowCount) return SG::Verdict::Run;
    const std::uint64_t n = ++g_refused[row];
    if ((n & (n - 1)) == 0)
        UE_LOGI("plant_gate[%s]: client-refuse %p (call #%llu) -- the host runs the plants", kRows[row].tag, call.object,
                static_cast<unsigned long long>(n));
    return SG::Verdict::Cancel;
}

// The gate skips the post of a cancelled body, so this counts the plant ticks that ran.
void OnPlantTickPost(const SG::Call&) {
    if (coop::spawn_authority::IsActiveClientSession()) ++g_ran;
}

std::atomic<bool> g_watched{false};

}  // namespace

void Install() {
    if (g_watched.exchange(true, std::memory_order_acq_rel)) return;
    int watched = 0;
    for (int i = 0; i < kRowCount; ++i) {
        if (SG::WatchName(*kRows[i].row, kTagBase + i, &OnRowPre, i == kPlantTick ? &OnPlantTickPost : nullptr)) {
            ++watched;
        } else {
            UE_LOGE("plant_gate: the watch on %ls::%ls did not register -- a client runs it", kRows[i].row->Class(),
                    kRows[i].row->Name());
        }
    }
    UE_LOGI("plant_gate: %d/%d plant watches registered by class and name; refused only on an active client session",
            watched, kRowCount);
}

void OnDisconnect() {
    std::uint64_t total = g_ran;
    for (int i = 0; i < kRowCount; ++i) total += g_refused[i];
    if (total != 0)
        UE_LOGI("plant_gate: this session refused %llu plant ticks, %llu herb grown, %llu sprinkler timers and %llu growth-area "
                "ticks; %llu plant ticks ran", static_cast<unsigned long long>(g_refused[kPlantTick]),
                static_cast<unsigned long long>(g_refused[kHerbGrown]), static_cast<unsigned long long>(g_refused[kSprinklerWater]),
                static_cast<unsigned long long>(g_refused[kGrowthAreaTick]), static_cast<unsigned long long>(g_ran));
    for (std::uint64_t& n : g_refused) n = 0;
    g_ran = 0;
}

Counts GetCounts() {
    return Counts{g_refused[kPlantTick], g_refused[kHerbGrown], g_refused[kSprinklerWater], g_refused[kGrowthAreaTick], g_ran};
}

}  // namespace coop::plant_gate

// coop/interactables/atv_damage_gate.cpp -- see coop/interactables/atv_damage_gate.h.

#include "coop/interactables/atv_damage_gate.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/atv_hit_guard.h"
#include "coop/interactables/atv_sync.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <chrono>

namespace coop::atv_damage_gate {
namespace {

namespace SG = ue_wrap::script_gate;

// ATV_C's own bodies. The class owns all three; car1_witch_C inherits them, so a Declared row matches it too.
UE_FACT_FN(kRowAddDamage, "atv_damage_gate", L"ATV_C", L"addDamage", Declared);
UE_FACT_FN(kRowExploded, "atv_damage_gate", L"ATV_C", L"exploded", Declared);
UE_FACT_FN(kRowExplode, "atv_damage_gate", L"ATV_C", L"explode", Declared);

constexpr int kTagAddDamage = 0x41544430;  // 'ATD0'
constexpr int kTagExploded = 0x41544431;   // 'ATD1'
constexpr int kTagExplode = 0x41544432;    // 'ATD2'

enum Row { kAddDamage, kExploded, kExplode };

Counters g_counters{};

// The calls let run, by the copy they ran on: a drill that blasts one ATV judges that ATV, not the other one a save
// may hold within the blast. A small table; a ninth ATV is counted in the totals only.
struct RanOn { void* actor = nullptr; unsigned long long n[kRows] = {}; };
RanOn g_ranOn[8];

void CountRan(Row row, void* actor) {
    for (RanOn& r : g_ranOn) {
        if (r.actor == actor || !r.actor) { r.actor = actor; ++r.n[row]; return; }
    }
}

SG::Verdict Judge(Row row, const SG::Call& c) {
    // The developer red arm of the ATV health drill: no copy is a non-owner here, so the host's copy runs the damage
    // the drill's `gate` arm needs it to cancel. Resolved once.
    static const bool s_redNonowner =
        coop::config::ResolveEnum(coop::config_registry::rows::atv_health_red) == "nonowner";
    if (!s_redNonowner && coop::atv_sync::NonOwnerCopy(c.object)) {
        ++g_counters.cancelled[row];
        return SG::Verdict::Cancel;
    }
    ++g_counters.ran[row];
    CountRan(row, c.object);
    // exploded / explode are rare, and which copy ran one is what a drill needs to know: name it.
    if (row != kAddDamage)
        UE_LOGI("atv_damage_gate: %s RAN on actor=%p (indexed=%d, owns=%d, guard active=%d)",
                row == kExploded ? "exploded" : "explode", c.object, coop::atv_sync::IsIndexed(c.object) ? 1 : 0,
                coop::atv_sync::OwnsTick(c.object) ? 1 : 0, coop::atv_hit_guard::Active() ? 1 : 0);
    return SG::Verdict::Run;
}

SG::Verdict OnAddDamagePre(const SG::Call& c) { return Judge(kAddDamage, c); }
SG::Verdict OnExplodedPre(const SG::Call& c) { return Judge(kExploded, c); }
SG::Verdict OnExplodePre(const SG::Call& c) { return Judge(kExplode, c); }

struct Hook {
    const ue_wrap::facts::Function* row;
    int                             tag;
    SG::PreFn                       pre;
    const wchar_t*                  name;
};
const Hook kTable[kRows] = {
    {&kRowAddDamage, kTagAddDamage, &OnAddDamagePre, L"addDamage"},
    {&kRowExploded, kTagExploded, &OnExplodedPre, L"exploded"},
    {&kRowExplode, kTagExplode, &OnExplodePre, L"explode"},
};

constexpr auto kRetry = std::chrono::seconds(1);
constexpr int  kMaxTries = 30;
using Clock = std::chrono::steady_clock;

bool              g_done[kRows] = {};
bool              g_dead[kRows] = {};
int               g_tries[kRows] = {};
Clock::time_point g_nextTry{};
bool              g_latched = false;

void SayDead(const Hook& h) {
    UE_LOGE("atv_damage_gate: the ATV_C::%ls watch is dead -- that route of an ATV's damage runs on every copy",
            h.name);
}

}  // namespace

// The shape of explosion_parking's Install (coop/world/explosion_parking.cpp): a refused registration is retried
// each second, 30 times; a registered watch the gate later settles not live is dead too. A Declared watch reads live
// before its class is judged, so a later class judgement that kills it is reported by the gate's own ERROR only.
void Install() {
    if (g_latched) return;
    if (Clock::now() >= g_nextTry) {
        g_nextTry = Clock::now() + kRetry;
        for (int i = 0; i < kRows; ++i) {
            if (g_done[i] || g_dead[i]) continue;
            if (SG::WatchName(*kTable[i].row, kTable[i].tag, kTable[i].pre, nullptr)) {
                g_done[i] = true;
            } else if (++g_tries[i] >= kMaxTries) {
                g_dead[i] = true;
                SayDead(kTable[i]);
            }
        }
    }
    bool pending = false, dead = false;
    for (int i = 0; i < kRows; ++i) {
        if (g_dead[i]) dead = true;
        else if (!g_done[i]) pending = true;
    }
    if (pending) return;
    if (dead) {   // each dead row said so when it died; the rest are left to the gate's own report
        g_latched = true;
        return;
    }
    SG::ResolvePendingNames();
    bool live = true;
    for (int i = 0; i < kRows; ++i)
        if (!SG::NameWatchLive(*kTable[i].row, kTable[i].tag)) live = false;
    if (live) {
        g_latched = true;
        UE_LOGI("atv_damage_gate: the ATV_C addDamage, exploded and explode watches are live");
        return;
    }
    for (int i = 0; i < kRows; ++i) {
        if (!SG::NameWatchSettled(*kTable[i].row, kTable[i].tag) || SG::NameWatchLive(*kTable[i].row, kTable[i].tag))
            continue;
        g_latched = true;
        SayDead(kTable[i]);
    }
}

Counters ReadCounters() { return g_counters; }

unsigned long long RanOnActor(void* actor, int row) {
    if (!actor || row < 0 || row >= kRows) return 0;
    for (const RanOn& r : g_ranOn)
        if (r.actor == actor) return r.n[row];
    return 0;
}

void Reset() {
    g_counters = Counters{};
    for (RanOn& r : g_ranOn) r = RanOn{};
}

}  // namespace coop::atv_damage_gate

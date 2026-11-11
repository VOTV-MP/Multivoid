// coop/dev/food_clock_probe.cpp -- see header.

#include "coop/dev/food_clock_probe.h"

#include "coop/config/config.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cstdint>

namespace coop::dev::food_clock_probe {
namespace {

namespace R = ue_wrap::reflection;

// Game-thread only. No mutex.
constexpr auto kVerdictPeriod = std::chrono::seconds(15);

void*    g_foodCls = nullptr;
uint64_t g_nextFoodClsTryMs = 0;

int  g_published = 0, g_applied = 0, g_refused = 0;
bool g_any = false, g_dirty = false;
std::chrono::steady_clock::time_point g_lastVerdict{};

bool IsFood(void* actor) {
    if (!actor) return false;
    if (!g_foodCls) {
        // FindClass walks the WHOLE object array, and a Blueprint class loads on demand -- in a
        // world with no food it never loads at all. So the lookup is throttled to 1 Hz rather than
        // paid once per record of a join drain.
        const uint64_t now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        if (now < g_nextFoodClsTryMs) return false;
        g_nextFoodClsTryMs = now + 1000;
        g_foodCls = R::FindClass(L"prop_food_C");
        if (!g_foodCls) return false;
    }
    void* cls = R::ClassOf(actor);
    return cls && (cls == g_foodCls || R::IsDescendantOfAny(cls, &g_foodCls, 1));
}

void Note(void* actor, int& counter) {
    if (!IsEnabled() || !IsFood(actor)) return;
    g_any = true;
    g_dirty = true;
    ++counter;
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::food_clock_probe);
    return s;
}

void NotePublish(void* actor) { Note(actor, g_published); }
void NoteApply(void* actor)   { Note(actor, g_applied); }
void NoteRefused(void* actor) { Note(actor, g_refused); }

// The run totals and the reading, unconditionally. Tick() puts it on a period; the session teardown
// calls it once more so the last one is the whole run.
void EmitVerdict() {
    if (!IsEnabled() || !g_any) return;
    g_lastVerdict = std::chrono::steady_clock::now();
    g_dirty = false;
    UE_LOGW("food_clock_probe: VERDICT published=%d applied=%d refused-by-the-class-gate=%d :: %hs",
            g_published, g_applied, g_refused,
            (g_published == 0 && g_applied == 0)
                ? "THE CLAIM IS HOLDING -- every food this peer offered to the record lane was refused by "
                  "the class gate, and none was published or applied (a run with no food in it shows a "
                  "refused count of zero too)"
                : "A FOOD CROSSED THE RECORD LANE -- the food lane's claim did not turn it away, so its "
                  "temperature and ripeness were written from a second address space");
}

void Tick() {
    if (!IsEnabled() || !g_any || !g_dirty) return;
    const auto now = std::chrono::steady_clock::now();
    if (g_lastVerdict.time_since_epoch().count() != 0 && now - g_lastVerdict < kVerdictPeriod) return;
    EmitVerdict();
}

void OnDisconnect() {
    EmitVerdict();
    // The class pointer goes with the rest: a Blueprint class can be unloaded and reloaded across a
    // level change, and a stale UClass would answer the lineage test for a class that no longer
    // exists -- the hazard prop_save_data::OnDisconnect resets for, one seam over.
    g_foodCls = nullptr;
    g_nextFoodClsTryMs = 0;
    g_published = g_applied = g_refused = 0;
    g_any = g_dirty = false;
    g_lastVerdict = {};
}

}  // namespace coop::dev::food_clock_probe

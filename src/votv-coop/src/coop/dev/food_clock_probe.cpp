// coop/dev/food_clock_probe.cpp -- see header.

#include "coop/dev/food_clock_probe.h"

#include "coop/config/config.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"

#include <chrono>
#include <cmath>
#include <cstdint>

namespace coop::dev::food_clock_probe {
namespace {

namespace R  = ue_wrap::reflection;
namespace SR = ue_wrap::save_record;

// Game-thread only (both seams dispatch a UFunction). No mutex.
constexpr int  kPerSeamLines  = 8;    // event lines per seam before the tally carries it alone
constexpr auto kVerdictPeriod = std::chrono::seconds(15);

// Aprop_food_C::getData lays its group out as [ the base's lifespan, temperature, ripeness,
// GetTimeSeconds ], so a food record is the only one whose floats group 0 runs this long.
constexpr size_t kTemperature = 1, kRipeness = 2, kStamp = 3;

void*    g_foodCls = nullptr;
uint64_t g_nextFoodClsTryMs = 0;

int   g_published = 0, g_applied = 0;
int   g_publishLines = 0, g_applyLines = 0;
// Which catch-up the record asks for, over EVERY record and not only the ones that printed a line.
// loadData branches on `ignoreRotting`, and the two halves are unrelated arithmetic on unrelated
// fields, so a run in which no record sets the flag has not exercised the temperature half at all.
int   g_branchTemp = 0, g_branchRipe = 0, g_branchUnknown = 0;
float g_deltaMin = 0, g_deltaMax = 0;
float g_moveTemp = 0, g_moveRipe = 0;   // the largest move the catch-up made, by field
bool  g_any = false, g_dirty = false;
std::chrono::steady_clock::time_point g_lastVerdict{};

// The first N applies are not where the interesting one is: the temperature branch is the rarer of
// the two, and a run's worst mover can land anywhere in the stream. So the largest temperature move
// keeps its own row and the verdict names it, however late it arrived.
struct Worst {
    std::wstring cls, key;
    float sentTemp = 0, gotTemp = 0, delta = 0;
    int   branch = -1;
    bool  seen = false;
} g_worst;

bool IsFood(void* actor) {
    if (!actor) return false;
    if (!g_foodCls) {
        // FindClass walks the WHOLE object array, and a Blueprint class loads on demand -- in a
        // world with no food it never loads at all. So the lookup is throttled to 1 Hz, the idiom
        // save_record's own EnsurePropClass uses, rather than paid once per record of a join drain.
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

// One element of a value group, or `miss` when the record is not shaped like a food's.
float FloatAt(const SR::SaveRecord& r, size_t i, float miss = 0) {
    if (r.floats.empty() || r.floats[0].size() <= i) return miss;
    return r.floats[0][i];
}

int UsesOf(const SR::SaveRecord& r) {
    if (r.ints.empty() || r.ints[0].empty()) return -1;
    return r.ints[0][0];
}

// Aprop_food_C::getData appends its own one-element bool group after the base's, and loadData
// branches on the LAST element of group 1: true takes the temperature catch-up, false the ripeness
// one.
int IgnoreRottingOf(const SR::SaveRecord& r) {
    if (r.bools.size() < 2 || r.bools[1].empty()) return -1;
    return r.bools[1].back() ? 1 : 0;
}

void NoteBranch(int branch) {
    if (branch < 0)      ++g_branchUnknown;
    else if (branch > 0) ++g_branchTemp;
    else                 ++g_branchRipe;
}

}  // namespace

bool IsEnabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::food_clock_probe);
    return s;
}

void NotePublish(void* actor, const std::wstring& key, const SR::SaveRecord& rec) {
    if (!IsEnabled() || !IsFood(actor)) return;
    g_any = true;
    g_dirty = true;
    ++g_published;
    NoteBranch(IgnoreRottingOf(rec));
    if (g_publishLines >= kPerSeamLines) return;
    ++g_publishLines;
    UE_LOGW("food_clock_probe: PUBLISH '%ls' key='%ls' uses=%d temperature=%.2f ripeness=%.2f "
            "stamp=%.2f s ignoreRotting=%d",
            R::ClassNameOf(actor).c_str(), key.c_str(), UsesOf(rec), FloatAt(rec, kTemperature),
            FloatAt(rec, kRipeness), FloatAt(rec, kStamp), IgnoreRottingOf(rec));
}

void NoteApply(void* actor, const std::wstring& key, const SR::SaveRecord& sent) {
    if (!IsEnabled() || !IsFood(actor)) return;
    // The actor's own getData, called again: the landed values and this peer's clock both come out
    // of the game's codec, with no field offset kept here.
    SR::SaveRecord landed;
    if (!SR::CaptureRecord(actor, landed)) {
        UE_LOGW("food_clock_probe: APPLY '%ls' key='%ls' -- the re-capture failed, so what the "
                "catch-up left cannot be read", R::ClassNameOf(actor).c_str(), key.c_str());
        return;
    }
    g_any = true;
    g_dirty = true;
    ++g_applied;
    NoteBranch(IgnoreRottingOf(sent));

    const float sentTemp = FloatAt(sent, kTemperature), sentRipe = FloatAt(sent, kRipeness);
    const float stamp    = FloatAt(sent, kStamp);
    const float myClock  = FloatAt(landed, kStamp);
    const float delta    = myClock - stamp;
    const float gotTemp  = FloatAt(landed, kTemperature), gotRipe = FloatAt(landed, kRipeness);
    const int   branch   = IgnoreRottingOf(sent);

    if (g_applied == 1 || delta < g_deltaMin) g_deltaMin = delta;
    if (g_applied == 1 || delta > g_deltaMax) g_deltaMax = delta;
    const float movedTemp = std::fabs(gotTemp - sentTemp);
    if (!g_worst.seen || movedTemp > g_moveTemp)
        g_worst = Worst{R::ClassNameOf(actor), key, sentTemp, gotTemp, delta, branch, true};
    g_moveTemp = std::fmax(g_moveTemp, movedTemp);
    g_moveRipe = std::fmax(g_moveRipe, std::fabs(gotRipe - sentRipe));

    if (g_applyLines >= kPerSeamLines) return;
    ++g_applyLines;
    UE_LOGW("food_clock_probe: APPLY '%ls' key='%ls' sent temperature=%.2f ripeness=%.2f "
            "stamp=%.2f s | my clock=%.2f s delta=%+.2f s -> landed temperature=%.2f (%+.2f K) "
            "ripeness=%.2f (%+.2f) branch=%hs",
            R::ClassNameOf(actor).c_str(), key.c_str(), sentTemp, sentRipe, stamp, myClock, delta,
            gotTemp, gotTemp - sentTemp, gotRipe, gotRipe - sentRipe,
            branch < 0 ? "?" : branch ? "temperature" : "ripeness");
}

void Tick() {
    if (!IsEnabled() || !g_any || !g_dirty) return;
    const auto now = std::chrono::steady_clock::now();
    if (g_lastVerdict.time_since_epoch().count() != 0 && now - g_lastVerdict < kVerdictPeriod) return;
    g_lastVerdict = now;
    g_dirty = false;
    UE_LOGW("food_clock_probe: VERDICT published=%d applied=%d delta min=%+.2f s max=%+.2f s "
            "| branch asked for: temperature=%d ripeness=%d unreadable=%d "
            "| widest post-apply gap temperature=%.2f K ripeness=%.2f",
            g_published, g_applied, g_deltaMin, g_deltaMax, g_branchTemp, g_branchRipe,
            g_branchUnknown, g_moveTemp, g_moveRipe);
    // The gap is what the record SENT against what the actor held once loadData returned, so it has
    // two possible authors: the catch-up moving the field, or a leaf whose loadData never restored
    // it and left this peer's own simulated value standing. Naming the class is what tells them
    // apart, so the widest one keeps its own line however late in the run it arrived.
    if (g_worst.seen)
        UE_LOGW("food_clock_probe: the widest temperature gap was '%ls' key='%ls' sent %.2f K, held "
                "%.2f K after the apply (%+.2f K) on delta=%+.2f s, record asking for the %hs "
                "catch-up", g_worst.cls.c_str(), g_worst.key.c_str(), g_worst.sentTemp,
                g_worst.gotTemp, g_worst.gotTemp - g_worst.sentTemp, g_worst.delta,
                g_worst.branch < 0 ? "unreadable" : g_worst.branch ? "temperature" : "ripeness");
    // What the numbers decide. The two clocks are UGameplayStatics::GetTimeSeconds on two worlds
    // that started at different moments, so their difference is not an elapsed time at all -- the
    // reading names which way it ran, and only claims the half of the catch-up that actually ran.
    const char* reading =
        (g_applied == 0 && g_published > 0)
            ? "PUBLISH SIDE ONLY -- this peer authored records and applied none, which is the "
              "host's half; the catch-up runs on the RECEIVER, so read the other peer's log"
        : (g_applied == 0)
            ? "NO DATA -- no food record was applied on this peer, so nothing here rules the "
              "catch-up in or out; check that a food class is still in the record lane"
        : (g_deltaMin > -1.0f && g_deltaMax < 1.0f && g_moveTemp < 0.5f && g_moveRipe < 1.0f)
            ? "CLOCKS AGREE -- the two peers read GetTimeSeconds within a second of each other and "
              "the catch-up moved nothing worth seeing; this run does not exercise the defect"
        : (g_deltaMin < 0)
            ? "THE SENDER'S CLOCK IS AHEAD -- the receiver subtracts a LARGER stamp from its own "
              "younger clock, so the catch-up runs on a negative elapsed time, and every record "
              "that asked for the ripeness half was CREDITED that time back as freshness"
            : "THE RECEIVER'S CLOCK IS AHEAD -- the catch-up ages the food by the difference of "
              "two unrelated world clocks instead of by the time it actually spent stored";
    UE_LOGW("food_clock_probe: reading :: %hs", reading);
    if (g_applied > 0 && g_branchTemp == 0)
        UE_LOGW("food_clock_probe: no record asked for the TEMPERATURE half -- every food here "
                "carried ignoreRotting=false, so that branch's arithmetic is unmeasured by this run");
}

}  // namespace coop::dev::food_clock_probe

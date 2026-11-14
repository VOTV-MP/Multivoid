// coop/gamefix/food_sleep_fix.cpp -- see coop/gamefix/food_sleep_fix.h.

#include "coop/gamefix/food_sleep_fix.h"

#include "coop/gamefix/body_guard.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/facts/function_row.h"

#include <algorithm>
#include <atomic>

namespace coop::gamefix::food_sleep_fix {
namespace {

namespace sg = ue_wrap::script_gate;
namespace R  = ue_wrap::reflection;
namespace V  = ue_wrap::vitals;

UE_FACT_FN(kRowAddFood, "food_sleep_fix", L"mainPlayer_C", L"addFood", Declared);
constexpr int kTag = 0x47584653;  // 'GXFS'

// mainplayer_c.addfood on 0.10.0: the body that adds the sleep gain to its own parameter.
BodyGuard g_guard{"food_sleep", 0x1ec64b62482f6cc9ull};

// The game clamps the sleep stat to this range where it writes it (addFood's own FClamp).
constexpr float kSleepMin = 0.f;
constexpr float kSleepMax = 100.f;

std::atomic<bool>     g_enabled{true};
std::atomic<uint64_t> g_applied{0};
bool g_watched = false;
R::ParamOffset g_offSleepArg{L"sleep"};
R::ParamOffset g_offFull{L"full"};

// The eat that is running: what the call was given and where the stats stood at its start.
struct Open {
    void* player = nullptr;
    float sleepArg = 0.f;
    float coffee = 0.f;
    float sleepBefore = 0.f;
};
Open g_open;

sg::Verdict OnAddFoodPre(const sg::Call& c) {
    g_open = Open{};
    if (!g_enabled.load(std::memory_order_relaxed) || !c.object || !c.locals || !g_guard.Holds(c.function))
        return sg::Verdict::Run;
    // The stat table is the machine's own player's: another pawn of the class has none here.
    void* local = coop::players::Registry::Get().Local();
    if (local && c.object != local) return sg::Verdict::Run;
    const int32_t off = g_offSleepArg.Of(c.function);
    if (off < 0) return sg::Verdict::Run;
    Open o;
    o.sleepArg = *reinterpret_cast<const float*>(c.locals + off);
    if (!V::Read(V::Field::Sleep, &o.sleepBefore) || !V::Read(V::Field::CoffeePower, &o.coffee))
        return sg::Verdict::Run;
    o.player = c.object;
    g_open = o;
    return sg::Verdict::Run;
}

void OnAddFoodPost(const sg::Call& c) {
    const Open o = g_open;
    g_open = Open{};
    if (!o.player || o.player != c.object || !c.locals) return;
    // `full` true is a refused eat ("Im not hungry", "I don't want to eat that"): no gain, as in 0.9.0n.
    const int32_t offFull = g_offFull.Of(c.function);
    if (offFull < 0) return;
    const uint8_t* full = sg::OutParamPtr(c, offFull);
    if (!full) full = c.locals + offFull;
    if (*full != 0) return;
    float now = 0.f;
    if (!V::Read(V::Field::Sleep, &now) || now != o.sleepBefore) return;  // the body wrote it itself
    const float gain = o.sleepArg * (1.f - o.coffee);  // Lerp(sleep, 0, coffee power)
    if (gain == 0.f) return;
    const float to = std::clamp(o.sleepBefore + gain, kSleepMin, kSleepMax);
    if (!V::Write(V::Field::Sleep, to)) return;
    const uint64_t n = g_applied.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 5 || n % 50 == 0)
        UE_LOGI("gamefix[food_sleep]: an eat's sleep gain %.2f (coffee power %.2f) is added: %.2f -> %.2f (n=%llu)",
                gain, o.coffee, o.sleepBefore, to, static_cast<unsigned long long>(n));
}

}  // namespace

void Install() {
    if (!g_watched) g_watched = sg::WatchName(kRowAddFood, kTag, &OnAddFoodPre, &OnAddFoodPost);
}

void SetEnabled(bool on) { g_enabled.store(on, std::memory_order_relaxed); }

uint64_t Applied() { return g_applied.load(std::memory_order_relaxed); }

}  // namespace coop::gamefix::food_sleep_fix

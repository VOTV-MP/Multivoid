// coop/world/explosion_recipe_selftest.cpp -- the un-gated selftest of the explosion recipe table, run
// once per process (shape: coop/player/hand_history_selftest.cpp). No engine and no wire: a source
// whose numbers are off, or a level that is not clamped, lets a request blast with parameters its
// Blueprint never uses.
//
// Red arm: with the dev row selftest_break_explosion_recipe the player's rows read the unclamped
// level, so the clamp checks must fail and the run must fail.

#include "coop/world/explosion_recipe.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "ue_wrap/core/log.h"

#include <cmath>
#include <mutex>

namespace coop::explosion_recipe {
namespace {

using ue_wrap::explosion::Params;
using ue_wrap::explosion_source::Kind;

struct Tally {
    int pass = 0;
    int total = 0;
    void operator()(bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("explosion_recipe selftest FAIL: %s", what);
    }
};

bool Near(float a, float b) { return std::fabs(a - b) < 0.001f; }

// The level a recipe is fed: clamped, except that the red arm passes it through raw.
struct Leveler {
    bool raw;
    float operator()(float level) const { return raw ? level : ClampLevel(level); }
};

void RunSrcClass(Tally& check) {
    bool roundTrip = true;
    for (uint8_t c = 1; c <= 7; ++c) {
        const Kind k = KindOfSrcClass(c);
        roundTrip = roundTrip && k != Kind::None && SrcClassOf(k) == c;
    }
    check(roundTrip, "srcClass: a value 1-7 does not round-trip");
    check(KindOfSrcClass(0) == Kind::None, "srcClass: 0 does not map to None");
    check(KindOfSrcClass(8) == Kind::None, "srcClass: 8 does not map to None");
    check(KindOfSrcClass(255) == Kind::None, "srcClass: 255 does not map to None");
    check(SrcClassOf(Kind::None) == 0, "srcClass: None does not map to 0");
}

void RunNames(Tally& check) {
    bool named = true;
    for (uint8_t c = 1; c <= 7; ++c) {
        named = named && ue_wrap::explosion_source::ClassName(KindOfSrcClass(c)) != nullptr;
    }
    check(named, "class name: a kind has no class name");
    check(ue_wrap::explosion_source::ClassName(Kind::None) == nullptr, "class name: None has one");
    check(LauncherClassName(1) != nullptr && LauncherClassName(2) != nullptr && LauncherClassName(3) != nullptr,
          "launcher: a value 1-3 has no class name");
    check(LauncherClassName(0) == nullptr, "launcher: 0 has a class name");
    check(LauncherClassName(4) == nullptr, "launcher: 4 has a class name");
}

void RunClamp(Tally& check, const Leveler& level) {
    check(Near(level(-5.0f), 0.0f), "clamp: -5 is not raised to 0");
    check(Near(level(0.0f), 0.0f), "clamp: 0 changed");
    check(Near(level(150.0f), 150.0f), "clamp: 150 changed");
    check(Near(level(250.0f), 200.0f), "clamp: 250 is not cut to 200");
}

bool Row(Kind k, float force, float damage, float radius) {
    Params p{};
    float scale[3];
    RecipeFor(k, 0.0f, p, scale);
    return Near(p.force, force) && Near(p.damage, damage) && Near(p.radius, radius) && Near(p.fireStrength, 0.0f) &&
           Near(p.fireFuel, 0.0f) && Near(scale[0], 1.0f) && Near(scale[1], 1.0f) && Near(scale[2], 1.0f);
}

void RunRows(Tally& check) {
    check(Row(Kind::Rocket, 5000, 80, 1000), "row: rocket");
    check(Row(Kind::Sticky, 4000, 20, 500), "row: sticky bomb");
    check(Row(Kind::Grenade, 10000, 50, 1000), "row: grenade");
    check(Row(Kind::Pipebomb, 2500, 100, 1500), "row: pipe bomb");
    check(Row(Kind::PoopGrenade, 2000, 25, 1000), "row: poop grenade");
    check(Row(Kind::SoapGrenade, 2000, 25, 1000), "row: soap grenade");
}

void RunDefaults(Tally& check) {
    Params p{};
    float scale[3];
    RecipeFor(Kind::Rocket, 0.0f, p, scale);
    check(!p.ignoreWalls && !p.skipSetting, "default: an untouched field is not zero");
    RecipeFor(Kind::None, 0.0f, p, scale);
    check(Near(p.force, 10000.0f) && Near(p.radius, 1000.0f) && Near(p.damage, 50.0f),
          "default: None is not the defaults");
}

// The player's row at a raw level fed through `level`.
bool PlayerAt(const Leveler& level, float raw, float force, float scaleWant, float damage) {
    Params p{};
    float scale[3];
    RecipeFor(Kind::Player, level(raw), p, scale);
    return Near(p.force, force) && Near(p.damage, damage) && Near(p.radius, 1000.0f) &&
           Near(p.fireStrength, 1.0f) && Near(p.fireFuel, 10.0f) && Near(scale[0], scaleWant) &&
           Near(scale[1], scaleWant) && Near(scale[2], scaleWant);
}

void RunPlayer(Tally& check, const Leveler& level) {
    check(PlayerAt(level, 0.0f, 2500.0f, 0.2f, 10.0f), "player: level 0");
    check(PlayerAt(level, 100.0f, 5000.0f, 1.0f, 25.0f), "player: level 100");
    check(PlayerAt(level, 200.0f, 7500.0f, 1.8f, 40.0f), "player: level 200");
    check(PlayerAt(level, 250.0f, 7500.0f, 1.8f, 40.0f), "player: level 250 is not held at the 200 row");
    check(PlayerAt(level, -5.0f, 2500.0f, 0.2f, 10.0f), "player: level -5 is not held at the 0 row");
}

bool RunSelftestBody() {
    Tally check;
    const Leveler level{coop::config::ResolveFlag(coop::config_registry::rows::selftest_break_explosion_recipe)};
    RunSrcClass(check);
    RunNames(check);
    RunClamp(check, level);
    RunRows(check);
    RunDefaults(check);
    RunPlayer(check, level);
    if (check.pass == check.total) {
        UE_LOGI("explosion_recipe selftest: ALL PASS (%d checks)", check.total);
        return true;
    }
    UE_LOGE("explosion_recipe selftest: %d/%d checks passed", check.pass, check.total);
    return false;
}

}  // namespace

// Once per process: the session-runtime call is once per session start, and a later call returns
// the first run's verdict without output.
bool RunSelftest() {
    static std::once_flag ran;
    static bool verdict = false;
    std::call_once(ran, [] { verdict = RunSelftestBody(); });
    return verdict;
}

}  // namespace coop::explosion_recipe

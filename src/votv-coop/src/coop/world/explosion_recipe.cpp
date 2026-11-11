// coop/world/explosion_recipe.cpp -- see coop/world/explosion_recipe.h.

#include "coop/world/explosion_recipe.h"

namespace coop::explosion_recipe {
namespace {

using ue_wrap::explosion_source::Kind;

constexpr float kMaxLevel = 200.0f;

float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// explosion_C's own defaults; every field not named here is zero or false.
ue_wrap::explosion::Params Defaults() {
    ue_wrap::explosion::Params p{};
    p.force      = 10000.0f;
    p.radius     = 1000.0f;
    p.damage     = 50.0f;
    return p;
}

void Over(ue_wrap::explosion::Params& p, float force, float damage, float radius) {
    p.force  = force;
    p.damage = damage;
    p.radius = radius;
}

}  // namespace

const wchar_t* LauncherClassName(uint8_t launcher) {
    switch (static_cast<Launcher>(launcher)) {
        case Launcher::Rpg:            return L"prop_funGun_rpg_C";
        case Launcher::StickyLauncher: return L"prop_funGun_stickyLauncher_C";
        case Launcher::WitchCar:       return L"car1_witch_C";
    }
    return nullptr;
}

Kind KindOfSrcClass(uint8_t srcClass) {
    switch (srcClass) {
        case 1: return Kind::Rocket;
        case 2: return Kind::Sticky;
        case 3: return Kind::Grenade;
        case 4: return Kind::Pipebomb;
        case 5: return Kind::Player;
        case 6: return Kind::PoopGrenade;
        case 7: return Kind::SoapGrenade;
        default: return Kind::None;
    }
}

uint8_t SrcClassOf(Kind k) {
    switch (k) {
        case Kind::Rocket:      return 1;
        case Kind::Sticky:      return 2;
        case Kind::Grenade:     return 3;
        case Kind::Pipebomb:    return 4;
        case Kind::Player:      return 5;
        case Kind::PoopGrenade: return 6;
        case Kind::SoapGrenade: return 7;
        case Kind::None:        break;
    }
    return 0;
}

float ClampLevel(float pillLevel) {
    if (pillLevel < 0.0f) return 0.0f;
    if (pillLevel > kMaxLevel) return kMaxLevel;
    return pillLevel;
}

void RecipeFor(Kind k, float clampedLevel, ue_wrap::explosion::Params& out, float scale[3]) {
    out = Defaults();
    scale[0] = scale[1] = scale[2] = 1.0f;
    switch (k) {
        case Kind::Rocket:      Over(out, 5000.0f, 80.0f, 1000.0f); break;
        case Kind::Sticky:      Over(out, 4000.0f, 20.0f, 500.0f); break;
        case Kind::Grenade:     Over(out, 10000.0f, 50.0f, 1000.0f); break;
        case Kind::Pipebomb:    Over(out, 2500.0f, 100.0f, 1500.0f); break;
        case Kind::PoopGrenade: Over(out, 2000.0f, 25.0f, 1000.0f); break;
        case Kind::SoapGrenade: Over(out, 2000.0f, 25.0f, 1000.0f); break;
        case Kind::Player: {
            const float t = clampedLevel / 100.0f;
            Over(out, Lerp(2500.0f, 5000.0f, t), Lerp(10.0f, 25.0f, t), 1000.0f);
            out.fireStrength = 1.0f;
            out.fireFuel     = 10.0f;
            scale[0] = scale[1] = scale[2] = Lerp(0.2f, 1.0f, t);
            break;
        }
        case Kind::None: break;
    }
}

}  // namespace coop::explosion_recipe

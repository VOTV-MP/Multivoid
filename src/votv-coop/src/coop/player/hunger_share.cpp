// coop/player/hunger_share.cpp -- see coop/player/hunger_share.h.

#include "coop/player/hunger_share.h"

#include "coop/config/config.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/roster_ledger.h"

#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/log.h"

#include <cmath>
#include <cstdint>

namespace coop::hunger_share {
namespace {

namespace V = ue_wrap::vitals;

constexpr uint64_t kStepMs = 1000;
constexpr float kBase = 1.0f;       // mainPlayer_C::foodDraining as the class leaves it
constexpr float kSame = 0.0005f;

uint64_t g_nextMs = 0;
float    g_written = kBase;  // what this module last put on the local player
int      g_saidPlayers = -1;

bool Near(float a, float b) { return std::fabs(a - b) < kSame; }

// Put `want` on the local player when the value there is the game's or this module's. False while an effect drives it
// or the player is not up.
bool Put(float want) {
    float cur = 0.f;
    if (!V::Read(V::Field::FoodDrain, &cur)) return false;
    if (!Near(cur, kBase) && !Near(cur, g_written)) return false;
    if (!Near(cur, want) && !V::Write(V::Field::FoodDrain, want)) return false;
    g_written = want;
    return true;
}

}  // namespace

float RateFor(int players) {
    return players <= 1 ? 1.0f : std::pow(static_cast<float>(players), -0.44f);
}

void Tick(coop::net::Session* session) {
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextMs) return;
    g_nextMs = now + kStepMs;
    const bool on = session && session->connected() &&
                    coop::config::ResolveFlag(::coop::config_registry::rows::net_hunger_shared);
    const int players = on ? coop::roster_ledger::OccupiedCount() : 1;
    const float rate = RateFor(players);
    if (!Put(kBase * rate)) return;
    if (players != g_saidPlayers) {
        g_saidPlayers = players;
        if (on)
            UE_LOGI("hunger_share: %d players -- this player's hunger runs at %.0f%% of the game's rate", players,
                    static_cast<double>(rate) * 100.0);
        else if (g_saidPlayers != -1)
            UE_LOGI("hunger_share: off -- this player's hunger runs at the game's rate");
    }
}

void OnDisconnect() {
    Put(kBase);
    g_saidPlayers = -1;
    g_nextMs = 0;
}

}  // namespace coop::hunger_share

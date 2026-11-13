// coop/player/nightmare_roll.cpp -- see coop/player/nightmare_roll.h.

#include "coop/player/nightmare_roll.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/sleep_rest.h"
#include "coop/player/sleep_sync.h"

#include "ue_wrap/actors/sleep.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <atomic>
#include <chrono>
#include <random>

namespace coop::nightmare_roll {
namespace {

namespace SLP = ue_wrap::sleep;
using Clock = std::chrono::steady_clock;

// mainGamemode.sleep's roll interval (the latent Delay, in world seconds), over the factor the game's sleep runs the
// world at: the real seconds between a host's rolls.
constexpr float kGameRollDelayS = 500.f;
constexpr float kRollEveryS = kGameRollDelayS / coop::sleep_rest::kNativeSleepDilation;

// [dev] The red arms of the nightmare drill, latched once.
bool RollOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::nightmare_roll_off);
    return off;
}
bool HideOff() {
    static const bool off = coop::config::ResolveFlag(coop::config_registry::rows::nightmare_hide_off);
    return off;
}

std::atomic<coop::net::Session*> g_session{nullptr};

// ---- the roll (a connected client) ----------------------------------------------------------------
bool g_counting = false;     // a rest is being counted
bool g_spent = false;        // this rest already had its nightmare: the next rest rolls again
Clock::time_point g_last{};
float g_restS = 0.f;         // real seconds of this rest since its last roll
int g_rolls = 0;             // this rest's rolls, for the log

std::mt19937& Rng() {
    static std::mt19937 rng{std::random_device{}()};
    return rng;
}

void ResetRoll() {
    g_counting = false;
    g_spent = false;
    g_restS = 0.f;
    g_rolls = 0;
}

// ---- the copies -------------------------------------------------------------------------------------
bool g_want[coop::net::kMaxPeers] = {};    // the slot's player is in a nightmare, as sleep_sync last announced
bool g_hidden[coop::net::kMaxPeers] = {};  // the slot's current copy is hidden by this

// Hide or show the slot's copy to match what its player's state says. A slot with no copy yet is left as it is:
// OnPuppetSpawned applies it when the copy comes.
void ApplyHide(int slot) {
    const bool want = g_want[slot] && !HideOff();
    if (want == g_hidden[slot]) return;
    coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    if (!p || !p->valid() || !p->actor()) return;
    ue_wrap::engine::SetActorHiddenInGame(p->actor(), want);
    g_hidden[slot] = want;
    UE_LOGI("nightmare_roll: slot %d's copy %s -- %s", slot, want ? "hidden" : "shown",
            want ? "that player is in a nightmare" : "that player is out of it");
}

void OnPeerRest(int slot, coop::net::SleepPeerState state) {
    if (slot < 0 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    g_want[slot] = state == coop::net::SleepPeerState::Dreaming;
    ApplyHide(slot);
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    coop::sleep_sync::SubscribePeerRest(&OnPeerRest);
}

void OnPuppetSpawned(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    g_hidden[slot] = false;  // a new body starts shown
    ApplyHide(slot);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected() || s->role() == coop::net::Role::Host) return;
    if (!coop::sleep_rest::Resting()) {
        ResetRoll();
        return;
    }
    if (g_spent || RollOff()) return;
    const Clock::time_point now = Clock::now();
    if (!g_counting) {
        g_counting = true;
        g_last = now;
        return;
    }
    g_restS += std::chrono::duration<float>(now - g_last).count();
    g_last = now;
    if (g_restS < kRollEveryS) return;
    g_restS -= kRollEveryS;
    float chance = 0.f;
    if (!SLP::ReadNightmareChance(chance)) {
        UE_LOGW("nightmare_roll: the nightmare chance does not read -- no roll this time");
        return;
    }
    ++g_rolls;
    const bool win = chance > 0.f && std::uniform_real_distribution<float>(0.f, 1.f)(Rng()) < chance;
    UE_LOGI("nightmare_roll: roll %d of the rest at chance %.2f -- %s", g_rolls, static_cast<double>(chance),
            win ? "a nightmare" : "none");
    if (!win) return;
    // One nightmare to a rest: the wake it begins ends the rest, and this stays spent until sleep_rest says so.
    g_spent = true;
    const bool began = SLP::BeginNightmare();
    UE_LOGI("nightmare_roll: this player's nightmare, roll %d of the rest at chance %.2f -- %s", g_rolls,
            static_cast<double>(chance), began ? "wakeup and createDream ran" : "the game's calls did not run");
}

void OnDisconnect() {
    ResetRoll();
    for (int slot = 0; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        g_want[slot] = false;
        ApplyHide(slot);
        g_hidden[slot] = false;
    }
}

}  // namespace coop::nightmare_roll

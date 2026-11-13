// coop/player/nightmare_roll.cpp -- see coop/player/nightmare_roll.h.

#include "coop/player/nightmare_roll.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/puppet_hide.h"
#include "coop/player/sleep_rest.h"
#include "coop/player/sleep_sync.h"

#include "ue_wrap/actors/sleep.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <chrono>
#include <random>

namespace coop::nightmare_roll {
namespace {

namespace SLP = ue_wrap::sleep;
using Clock = std::chrono::steady_clock;

// The one piece of the game's roll that is ours: WHEN. The game rolls once per sleep, a latent Delay(500) after
// fellAsleep (mainGamemode.cpp:506, :486), and its Delay counts world time, which a host's sleep runs at
// kNativeSleepDilation. A client's world is held at 1 (coop/player/sleep_rest), so the same Delay would fire after 500
// real seconds. The roll is made once, 500 / kNativeSleepDilation real seconds into the rest, which is when the
// host's fires; its chance, its wakeup() and createDream(nullptr) are the game's own.
constexpr float kGameRollDelayS = 500.f;
constexpr float kRollAtS = kGameRollDelayS / coop::sleep_rest::kNativeSleepDilation;

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
bool g_spent = false;        // this rest has had its roll: the next rest rolls again
Clock::time_point g_last{};
float g_restS = 0.f;         // real seconds of this rest so far
uint32_t g_points = 0;       // roll points reached by this process, for the drill

std::mt19937& Rng() {
    static std::mt19937 rng{std::random_device{}()};
    return rng;
}

void ResetRoll() {
    g_counting = false;
    g_spent = false;
    g_restS = 0.f;
}

// ---- the copies -------------------------------------------------------------------------------------
bool g_want[coop::net::kMaxPeers] = {};  // the slot's player is in a nightmare, as sleep_sync last announced

// The slot's copy is hidden for the dream while its player's state says so: the reason is set or cleared at
// coop/player/puppet_hide, the one owner of the flag.
void ApplyHide(int slot) {
    coop::puppet_hide::Set(static_cast<uint8_t>(slot), coop::puppet_hide::Reason::Dream, g_want[slot] && !HideOff());
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

uint32_t RollPoints() { return g_points; }

void OnPuppetSpawned(int slot) {
    if (slot < 0 || slot >= static_cast<int>(coop::net::kMaxPeers)) return;
    ApplyHide(slot);  // the dream a player is in when its copy is born: a forgotten slot's reason is asked again
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || !s->connected() || s->role() == coop::net::Role::Host) return;
    if (!coop::sleep_rest::Resting()) {
        ResetRoll();
        return;
    }
    if (g_spent) return;
    const Clock::time_point now = Clock::now();
    if (!g_counting) {
        g_counting = true;
        g_last = now;
        return;
    }
    g_restS += std::chrono::duration<float>(now - g_last).count();
    g_last = now;
    if (g_restS < kRollAtS) return;
    g_spent = true;  // one roll to a rest, as the game's
    ++g_points;
    if (RollOff()) {
        UE_LOGI("nightmare_roll: the rest's roll point -- no roll made [dev red nightmare_roll_off]");
        return;
    }
    float chance = 0.f;
    if (!SLP::ReadNightmareChance(chance)) {
        UE_LOGW("nightmare_roll: the nightmare chance does not read -- no roll this time");
        return;
    }
    const bool win = chance > 0.f && std::uniform_real_distribution<float>(0.f, 1.f)(Rng()) < chance;
    UE_LOGI("nightmare_roll: the rest's roll at chance %.2f -- %s", static_cast<double>(chance),
            win ? "a nightmare" : "none");
    if (!win) return;
    const bool began = SLP::BeginNightmare();
    UE_LOGI("nightmare_roll: this player's nightmare at chance %.2f -- %s", static_cast<double>(chance),
            began ? "wakeup and createDream ran" : "the game's calls did not run");
}

void OnDisconnect() {
    ResetRoll();
    for (int slot = 0; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) g_want[slot] = false;
}

}  // namespace coop::nightmare_roll

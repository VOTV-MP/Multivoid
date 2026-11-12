// coop/player/nightmare_roll.cpp -- see coop/player/nightmare_roll.h.

#include "coop/player/nightmare_roll.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
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

// The game's roll interval (mainGamemode.sleep: Delay 500) over the factor its sleep runs the world at (20), the
// real seconds between a host's rolls.
constexpr float kRollEveryS = 500.f / 20.f;

std::atomic<coop::net::Session*> g_session{nullptr};
bool g_resting = false;     // game thread: a rest is being counted
Clock::time_point g_last{};
float g_restS = 0.f;        // real seconds of this rest since its last roll
int g_rolls = 0;            // this rest's rolls, for the log

std::mt19937& Rng() {
    static std::mt19937 rng{std::random_device{}()};
    return rng;
}

// The copies this hid, by slot: the actor and whether it is hidden now.
void* g_copy[coop::net::kMaxPeers] = {};
bool g_hidden[coop::net::kMaxPeers] = {};

void HideDreamers() {
    static Clock::time_point s_next{};
    const Clock::time_point now = Clock::now();
    if (now < s_next) return;
    s_next = now + std::chrono::seconds(1);
    auto& reg = coop::players::Registry::Get();
    for (int slot = 0; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        coop::RemotePlayer* p = reg.Puppet(static_cast<uint8_t>(slot));
        void* actor = p ? p->actor() : nullptr;
        const bool want = actor && coop::sleep_sync::PeerState(slot) == coop::net::SleepPeerState::Dreaming;
        if (actor == g_copy[slot] && want == g_hidden[slot]) continue;
        if (actor) ue_wrap::engine::SetActorHiddenInGame(actor, want);
        if (want != g_hidden[slot])
            UE_LOGI("nightmare_roll: slot %d's copy %s -- %s", slot, want ? "hidden" : "shown",
                    want ? "that player is in a nightmare" : "that player is out of it");
        g_copy[slot] = actor;
        g_hidden[slot] = want;
    }
}

void ShowAll() {
    for (int slot = 0; slot < static_cast<int>(coop::net::kMaxPeers); ++slot) {
        coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
        if (g_hidden[slot] && p && p->actor() == g_copy[slot])
            ue_wrap::engine::SetActorHiddenInGame(g_copy[slot], false);
        g_copy[slot] = nullptr;
        g_hidden[slot] = false;
    }
}

void EndRest() {
    g_resting = false;
    g_restS = 0.f;
    g_rolls = 0;
}

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    HideDreamers();
    if (!s->connected() || s->role() == coop::net::Role::Host) return;
    if (!SLP::EnsureResolved()) return;
    if (!SLP::IsSleeping() || SLP::IsDreaming()) {
        if (g_resting) EndRest();
        return;
    }
    const Clock::time_point now = Clock::now();
    if (!g_resting) {
        g_resting = true;
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
    const bool began = SLP::BeginNightmare();
    UE_LOGI("nightmare_roll: this player's nightmare, roll %d of the rest at chance %.2f -- %s", g_rolls,
            static_cast<double>(chance), began ? "wakeup and createDream ran" : "the game's calls did not run");
    EndRest();
}

void OnDisconnect() {
    EndRest();
    ShowAll();
}

}  // namespace coop::nightmare_roll

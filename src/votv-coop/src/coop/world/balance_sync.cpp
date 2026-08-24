// coop/balance_sync.cpp -- see coop/balance_sync.h.

#include "coop/world/balance_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "ue_wrap/world/economy.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"

#include <atomic>
#include <cstdint>

namespace coop::balance_sync {
namespace {

namespace GT = ue_wrap::game_thread;
namespace E  = ue_wrap::economy;

std::atomic<coop::net::Session*> g_session{nullptr};

// Host broadcast dedup -- game-thread only (Tick host branch). Sentinel via a separate
// flag so any int32 Points value (including 0 / negative) is a legitimate balance.
bool    g_haveSent = false;
int32_t g_lastSent = 0;

// Client mirror: the latest host balance to apply, RETRIED each tick until the saveSlot
// resolves -- a connect-edge BalanceSync can arrive before the client's gamemode/saveSlot
// is loaded (the WritePoints would otherwise silently fail + the mirror never lands).
// Atomic because ApplyFromHost runs off the event_feed dispatch while Tick applies on the
// game thread. Latest-wins (a newer BalanceSync overwrites a not-yet-applied pending).
std::atomic<int32_t> g_pendingTotal{0};
std::atomic<bool>    g_havePending{false};

bool IsHost() {
    auto* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (s->role() == coop::net::Role::Host) {
        // HOST: poll the canonical balance; broadcast on change (dedup).
        int32_t points = 0;
        if (!E::ReadPoints(&points)) return;  // not resolved yet (booting / at menu)
        if (g_haveSent && points == g_lastSent) return;  // unchanged -> no broadcast
        g_haveSent = true;
        g_lastSent = points;
        coop::net::BalancePayload p{points};
        s->SendReliable(coop::net::ReliableKind::BalanceSync, &p, sizeof(p));
        UE_LOGI("balance_sync: host Points -> %d, broadcast BalanceSync to all", points);
    } else {
        // CLIENT: retry the latest pending mirror until WritePoints succeeds (the
        // saveSlot may not have resolved when the connect-edge BalanceSync arrived).
        if (!g_havePending.load(std::memory_order_acquire)) return;
        const int32_t total = g_pendingTotal.load(std::memory_order_acquire);
        if (E::WritePoints(total)) {
            // WritePoints is side-effect-free, so the on-screen HUD credit number stays frozen
            // (text_points is push-updated only by the BP addPoints->SetText). Re-run that
            // repaint with a zero add so the mirrored value actually shows. (2026-06-08 fix.)
            E::RefreshPointsHud();
            g_havePending.store(false, std::memory_order_release);
            UE_LOGI("balance_sync: mirrored host balance -> Points=%d (HUD repainted)", total);
        }
        // else: saveSlot still resolving -- silently retry next tick.
    }
}

void OnClientConnect(int slot) {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != coop::net::Role::Host) return;
    int32_t points = 0;
    if (!E::ReadPoints(&points)) {
        UE_LOGW("balance_sync: connect-edge slot %d -- Points not resolved yet (no replay)", slot);
        return;
    }
    coop::net::BalancePayload p{points};
    s->SendReliableToSlot(slot, coop::net::ReliableKind::BalanceSync, &p, sizeof(p));
    UE_LOGI("balance_sync: connect-edge -> sent Points=%d to slot %d", points, slot);
}

void ApplyFromHost(int32_t total) {
    if (IsHost()) return;  // the host is authoritative -- never mirror our own broadcast
    // Stash the latest; Tick (game thread) writes it + RETRIES until the saveSlot
    // resolves (a connect-edge BalanceSync can arrive before the gamemode is ready).
    g_pendingTotal.store(total, std::memory_order_release);
    g_havePending.store(true, std::memory_order_release);
}

void CreditLocal(int32_t amount) {
    auto* s = g_session.load(std::memory_order_acquire);
    const bool host      = s && s->role() == coop::net::Role::Host;
    const bool connected = s && s->connected();
    if (host || !connected) {
        // Host OR solo (no live session): apply locally. If hosting, the next TickHost
        // poll sees the new Points and broadcasts it to all clients.
        GT::Post([amount] { E::AddPoints(amount); });
        UE_LOGI("balance_sync: local credit %+d (%s)", amount,
                host ? "host -> will broadcast" : "solo");
        return;
    }
    // SECURITY A5 (docs/security/TRACKER.md): a connected CLIENT has no way to credit
    // the shared balance, and that is deliberate. This used to send a BalanceDelta the
    // host applied via AddPoints with no value bound; the lane was retired whole in v135
    // rather than clamped (RULE 2) because balance is host-authoritative everywhere and
    // there is no legitimate client->host economy write. Refusing here is defence in
    // depth -- coop::dev_gate already refuses the only caller on a client.
    UE_LOGW("balance_sync: local credit %+d REFUSED -- balance is host-authoritative "
            "(the client->host delta lane was retired in v135, security A5)", amount);
}

void OnDisconnect() {
    g_haveSent = false;
    g_lastSent = 0;
    g_havePending.store(false, std::memory_order_release);
}

}  // namespace coop::balance_sync

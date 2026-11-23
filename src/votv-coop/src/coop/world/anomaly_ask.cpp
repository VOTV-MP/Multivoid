// coop/world/anomaly_ask.cpp -- see coop/world/anomaly_ask.h.

#include "coop/world/anomaly_ask.h"

#include "coop/creatures/nisse_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/world/hexpillar_sync.h"

#include "ue_wrap/core/log.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>

namespace coop::anomaly_ask {
namespace {

namespace N = coop::net;

constexpr uint64_t kGapMs = 1000;   // one ask a second per sender

std::atomic<N::Session*> g_session{nullptr};
uint64_t g_lastAt[N::kMaxPeers] = {};

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

bool Send(uint8_t op, float x, float y, float z) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != N::Role::Client) return false;
    N::AnomalyAskPayload p{};
    p.op = op;
    p.x = x;
    p.y = y;
    p.z = z;
    return s->SendReliableToSlot(0, N::ReliableKind::AnomalyAsk, &p, sizeof(p));
}

void OnAsk(N::Session&, uint8_t slot, const N::AnomalyAskPayload& p) {
    if (slot < 1 || slot >= N::kMaxPeers) return;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
        UE_LOGW("anomaly_ask: slot %u's ask (op %u) refused: a place that is not a number", slot, p.op);
        return;
    }
    const uint64_t now = ::GetTickCount64();
    if (now - g_lastAt[slot] < kGapMs) {
        UE_LOGW("anomaly_ask: slot %u's ask (op %u) refused: within a second of its last", slot, p.op);
        return;
    }
    g_lastAt[slot] = now;
    switch (p.op) {
        case N::kAnomalyAskDigPillar: coop::hexpillar_sync::OnDigAsk(slot, p.x, p.y, p.z); break;
        case N::kAnomalyAskNisseWoken: coop::nisse_sync::OnWokenAsk(slot); break;
        default: UE_LOGW("anomaly_ask: slot %u's ask refused: op %u names no act", slot, p.op); break;
    }
}

void OnDisconnect() {
    for (uint64_t& t : g_lastAt) t = 0;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::anomaly_ask

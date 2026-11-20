// coop/dev/letgo_release_delay.cpp -- see coop/dev/letgo_release_delay.h.

#include "coop/dev/letgo_release_delay.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_element_tracker.h"   // SessionIsHost
#include "coop/props/remote_prop.h"            // OnRelease

#include "ue_wrap/core/log.h"

#include <cstdint>
#include <deque>

namespace coop::dev::letgo_release_delay {
namespace {

struct Held {
    uint64_t dueMs = 0;
    int      slot = 0;
    coop::net::PropReleasePayload payload{};
};
std::deque<Held> g_held;   // oldest first: one delay for every release, so the due times never run backwards

}  // namespace

long DelayMs() {
    static const long ms = coop::config::ResolveInt(::coop::config_registry::rows::letgo_release_delay_ms);
    return ms;
}

void Release(int senderSlot, const coop::net::PropReleasePayload& payload, void* localPlayer) {
    if (DelayMs() <= 0 || coop::prop_element_tracker::SessionIsHost()) {
        coop::remote_prop::OnRelease(senderSlot, payload, localPlayer);
        return;
    }
    g_held.push_back(Held{coop::net::NowMs() + static_cast<uint64_t>(DelayMs()), senderSlot, payload});
    UE_LOGI("[LETGO-DELAY] slot %d's release of hold %u held %ld ms", senderSlot, static_cast<unsigned>(payload.holdGen),
            DelayMs());
}

void Tick() {
    if (g_held.empty()) return;
    const uint64_t now = coop::net::NowMs();
    while (!g_held.empty() && g_held.front().dueMs <= now) {
        const Held h = g_held.front();
        g_held.pop_front();
        UE_LOGI("[LETGO-DELAY] slot %d's release of hold %u applied", h.slot, static_cast<unsigned>(h.payload.holdGen));
        coop::remote_prop::OnRelease(h.slot, h.payload, coop::players::Registry::Get().Local(), /*replayed=*/true);
    }
}

void OnDisconnect() { g_held.clear(); }

}  // namespace coop::dev::letgo_release_delay

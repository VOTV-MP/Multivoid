// coop/dev/letgo_release_delay.cpp -- see coop/dev/letgo_release_delay.h.

#include "coop/dev/letgo_release_delay.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_element_tracker.h"   // SessionIsHost
#include "coop/props/remote_prop.h"            // OnRelease

#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/world_identity.h"     // AddWorldChangeListener: a held release belongs to one world

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
uint32_t g_overflowed = 0;
constexpr size_t kMaxHeld = 64;   // far above releases-per-second x 0.4 s; a full queue sheds its oldest

void OnWorldChange() noexcept {
    if (!g_held.empty())
        UE_LOGI("[LETGO-DELAY] world change: %zu held release(s) dropped", g_held.size());
    g_held.clear();
}

}  // namespace

long DelayMs() {
    static const long ms = coop::config::ResolveInt(::coop::config_registry::rows::letgo_release_delay_ms);
    return ms;
}

void Release(int senderSlot, const coop::net::PropReleasePayload& payload, void* localPlayer) {
    UE_ASSERT_GAME_THREAD("letgo_release_delay::Release");
    if (DelayMs() <= 0 || coop::prop_element_tracker::SessionIsHost()) {
        coop::remote_prop::OnRelease(senderSlot, payload, localPlayer);
        return;
    }
    static bool s_listening = false;
    if (!s_listening) {
        s_listening = true;
        if (!ue_wrap::world_identity::AddWorldChangeListener(&OnWorldChange))
            UE_LOGE("[LETGO-DELAY] the world-change list is full -- held releases are dropped at the session's end only");
    }
    if (g_held.size() >= kMaxHeld) {
        const Held& oldest = g_held.front();
        const uint32_t n = ++g_overflowed;
        if (n <= 3 || (n & (n - 1)) == 0)
            UE_LOGW("[LETGO-DELAY] %zu releases held: slot %d's of hold %u dropped for the newest (#%u)", kMaxHeld,
                    oldest.slot, static_cast<unsigned>(oldest.payload.holdGen), n);
        g_held.pop_front();
    }
    g_held.push_back(Held{coop::net::NowMs() + static_cast<uint64_t>(DelayMs()), senderSlot, payload});
    UE_LOGI("[LETGO-DELAY] slot %d's release of hold %u held %ld ms", senderSlot, static_cast<unsigned>(payload.holdGen),
            DelayMs());
}

void Tick() {
    UE_ASSERT_GAME_THREAD("letgo_release_delay::Tick");
    if (g_held.empty()) return;
    const uint64_t now = coop::net::NowMs();
    while (!g_held.empty() && g_held.front().dueMs <= now) {
        const Held h = g_held.front();
        g_held.pop_front();
        UE_LOGI("[LETGO-DELAY] slot %d's release of hold %u applied", h.slot, static_cast<unsigned>(h.payload.holdGen));
        // The first application of this release, as a live one: the arrival never reached OnRelease.
        coop::remote_prop::OnRelease(h.slot, h.payload, coop::players::Registry::Get().Local());
    }
}

void OnDisconnect() {
    UE_ASSERT_GAME_THREAD("letgo_release_delay::OnDisconnect");
    g_held.clear();
    g_overflowed = 0;
}

}  // namespace coop::dev::letgo_release_delay

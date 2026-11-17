// coop/session/local_grants.cpp -- see coop/session/local_grants.h.

#include "coop/session/local_grants.h"

#include "coop/net/session.h"
#include "coop/net/session_serial.h"

#include <atomic>

namespace coop::session::local_grants {
namespace {

namespace G = coop::permissions::grants;

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<uint64_t> g_packed{0};

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

bool Has(G::Projected n) {
    const coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return true;  // solo and the menu: today's answer
    // The word first, the serial second; a word of another serial reads as the role's default.
    const uint64_t packed = g_packed.load(std::memory_order_acquire);
    const uint32_t current = coop::net::session_serial::Current();
    return G::ReadBit(packed, current, s->role() == coop::net::Role::Host, n);
}

bool AnyDevLocal() {
    for (uint8_t i = 0; i < static_cast<uint8_t>(G::Projected::Count); ++i) {
        if (i == static_cast<uint8_t>(G::Projected::Cheats)) continue;   // a player's right, not a dev pane's tool
        if (Has(static_cast<G::Projected>(i))) return true;
    }
    return false;
}

uint32_t AppliedSerial() { return static_cast<uint32_t>(g_packed.load(std::memory_order_acquire) >> 32); }

void SetBits(uint32_t serial, uint32_t bits) {
    g_packed.store(G::Pack(serial, bits), std::memory_order_release);
}

}  // namespace coop::session::local_grants

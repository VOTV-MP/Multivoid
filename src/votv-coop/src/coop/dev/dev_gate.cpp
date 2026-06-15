// coop/dev/dev_gate.cpp -- see coop/dev/dev_gate.h.

#include "coop/dev/dev_gate.h"

#include "coop/net/session.h"

#include <atomic>

namespace coop::dev_gate {
namespace {
std::atomic<coop::net::Session*> g_session{nullptr};
}  // namespace

void SetSession(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
}

bool Allowed() {
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return true;  // solo / no session: dev tools are fine
    return s->role() == coop::net::Role::Host;
}

}  // namespace coop::dev_gate

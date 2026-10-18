// coop/dev/announce_hold.cpp -- see coop/dev/announce_hold.h.

#include "coop/dev/announce_hold.h"

#include <atomic>

namespace coop::dev::announce_hold {
namespace {

std::atomic<bool> g_held{false};

}  // namespace

void Set(bool on) { g_held.store(on, std::memory_order_release); }

bool Held() { return g_held.load(std::memory_order_acquire); }

}  // namespace coop::dev::announce_hold

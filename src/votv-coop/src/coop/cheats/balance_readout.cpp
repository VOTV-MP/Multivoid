// coop/cheats/balance_readout.cpp -- see coop/cheats/balance_readout.h.

#include "coop/cheats/balance_readout.h"

#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/world/economy.h"

#include <windows.h>

#include <atomic>

namespace coop::cheats::balance_readout {
namespace {

std::atomic<int32_t> g_value{0};
std::atomic<bool> g_known{false};
std::atomic<bool> g_posted{false};
std::atomic<uint64_t> g_lastMs{0};

}  // namespace

void RequestRefresh() {
    const uint64_t now = ::GetTickCount64();
    if (now - g_lastMs.load(std::memory_order_relaxed) < 250) return;
    bool expected = false;
    if (!g_posted.compare_exchange_strong(expected, true)) return;
    g_lastMs.store(now, std::memory_order_relaxed);
    ue_wrap::game_thread::Post([] {
        int32_t v = 0;
        if (ue_wrap::economy::ReadPoints(&v)) {
            g_value.store(v, std::memory_order_relaxed);
            g_known.store(true, std::memory_order_release);
        }
        g_posted.store(false, std::memory_order_release);
    });
}

bool Latest(int32_t* out) {
    if (!g_known.load(std::memory_order_acquire)) return false;
    *out = g_value.load(std::memory_order_relaxed);
    return true;
}

}  // namespace coop::cheats::balance_readout

// ue_wrap/core/walk_census.cpp -- see walk_census.h.
#include "ue_wrap/core/walk_census.h"

#include <atomic>
#include <cstdint>

namespace ue_wrap::walk_census {
namespace {

std::atomic<unsigned long long> g_walks{0};
std::atomic<unsigned long long> g_unattributed{0};

// Open-addressed by site: a slot's site is claimed once and never changes.
struct Site {
    std::atomic<void*> ip{nullptr};
    std::atomic<unsigned long long> count{0};
};
constexpr int kSites = kSiteSlots;
Site g_sites[kSites];

}  // namespace

void NoteArrayWalk(void* callSite) {
    g_walks.fetch_add(1, std::memory_order_relaxed);
    const size_t h = (reinterpret_cast<uintptr_t>(callSite) >> 4) % kSites;
    for (int k = 0; k < kSites; ++k) {
        Site& s = g_sites[(h + k) % kSites];
        void* cur = s.ip.load(std::memory_order_acquire);
        if (!cur && s.ip.compare_exchange_strong(cur, callSite, std::memory_order_acq_rel)) cur = callSite;
        if (cur == callSite) {
            s.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    g_unattributed.fetch_add(1, std::memory_order_relaxed);
}

unsigned long long ArrayWalkCountTotal() { return g_walks.load(std::memory_order_relaxed); }

unsigned long long ArrayWalkUnattributedTotal() { return g_unattributed.load(std::memory_order_relaxed); }

bool ArrayWalkSiteAt(int i, void** outSite, unsigned long long* outCount) {
    if (i < 0 || i >= kSites) return false;
    *outSite = g_sites[i].ip.load(std::memory_order_acquire);
    *outCount = g_sites[i].count.load(std::memory_order_relaxed);
    return true;
}

}  // namespace ue_wrap::walk_census

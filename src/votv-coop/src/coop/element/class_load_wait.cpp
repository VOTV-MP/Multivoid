// coop/element/class_load_wait.cpp -- see coop/element/class_load_wait.h.
#include "coop/element/class_load_wait.h"

#include "coop/element/bind_wait.h"
#include "ue_wrap/core/class_resolve.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/world_identity.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace coop::element::class_load_wait {
namespace {

struct Waiter {
    const char* site;
    uint32_t key;
    std::function<void()> retry;
};

struct Group {
    std::wstring className;  // lower-cased
    std::vector<Waiter> waiters;
};

std::vector<Group> g_groups;  // in the order each class first waited
uint32_t g_worldGen = 0;

std::wstring Lower(const std::wstring& s) {
    std::wstring o = s;
    for (wchar_t& c : o)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return o;
}

// A world change drops every wait (its marks went with bind_wait's Clear). Run before anything is added, so a spawn that
// parks in the frame the world flips belongs to the new world and is kept.
void SyncWorld() {
    (void)ue_wrap::world_identity::CurrentWorld();
    const uint32_t gen = ue_wrap::world_identity::Generation();
    if (gen == g_worldGen) return;
    g_worldGen = gen;
    size_t n = 0;
    for (const Group& g : g_groups) n += g.waiters.size();
    if (n > 0)
        UE_LOGW("class_load_wait: the world changed with %zu spawn(s) waiting on a class load -- dropped", n);
    g_groups.clear();
}

void Add(const std::wstring& className, Waiter w) {
    const std::wstring name = Lower(className);
    for (Group& g : g_groups) {
        if (g.className == name) {
            g.waiters.push_back(std::move(w));
            return;
        }
    }
    g_groups.push_back(Group{name, {}});
    g_groups.back().waiters.push_back(std::move(w));
}

}  // namespace

void Park(const char* site, const std::wstring& className, uint32_t key, std::function<void()> retry) {
    SyncWorld();
    Add(className, Waiter{site, key, std::move(retry)});
}

void ParkSpawn(const char* site, const std::wstring& className, uint32_t eid, std::function<void()> retry,
               std::function<bool()> bound) {
    SyncWorld();
    // The mark is this wait's only when no other owner held the eid already (a kerfur adoption, a pile's pending bind):
    // a mark another lane holds is left to it.
    const bool ours = !bind_wait::IsDeferred(eid);
    bind_wait::MarkDeferred(eid, "its class is loading");
    Add(className, Waiter{site, eid, [site, eid, ours, retry = std::move(retry), bound = std::move(bound)] {
        // The mark is dropped (its holds kept) while the spawn runs again, so that a lane that marks the eid in that run
        // is told apart from this wait's own mark afterwards.
        if (ours) bind_wait::Unmark(eid);
        retry();
        if (IsParked(site, eid)) return;  // parked anew: it marked the eid again
        if (bound()) bind_wait::OnBound(eid);  // a mirror stands: release what waited
        else if (ours && !bind_wait::IsDeferred(eid)) bind_wait::GiveUp(eid, "its spawn did not bind once its class loaded");
    }});
}

size_t CancelIf(const char* site, const std::function<bool(uint32_t)>& match) {
    size_t n = 0;
    for (Group& g : g_groups) {
        for (auto it = g.waiters.begin(); it != g.waiters.end();) {
            if (std::strcmp(it->site, site) == 0 && match(it->key)) {
                it = g.waiters.erase(it);
                ++n;
            } else {
                ++it;
            }
        }
    }
    g_groups.erase(std::remove_if(g_groups.begin(), g_groups.end(), [](const Group& g) { return g.waiters.empty(); }),
                   g_groups.end());
    return n;
}

bool Cancel(const char* site, uint32_t key) {
    return CancelIf(site, [key](uint32_t k) { return k == key; }) > 0;
}

bool IsParked(const char* site, uint32_t key) {
    for (const Group& g : g_groups)
        for (const Waiter& w : g.waiters)
            if (w.key == key && std::strcmp(w.site, site) == 0) return true;
    return false;
}

void Tick() {
    if (g_groups.empty()) return;  // an idle frame does nothing
    SyncWorld();
    if (g_groups.empty()) return;
    std::vector<Group> batch;
    batch.swap(g_groups);
    size_t i = 0;
    for (; i < batch.size(); ++i) {
        // One attempt per class per tick, and only while this tick's load budget has room: a group that would answer
        // Pending again waits for the next tick untouched, so a spent budget costs nothing.
        if (ue_wrap::class_resolve::LoadBudgetSpent()) break;
        Group& g = batch[i];
        if (g.waiters.empty()) continue;
        // The first waiter asks the resolver again, which is the load; it either completes (the class is then found by
        // every other waiter of the class) or parks anew.
        Waiter first = std::move(g.waiters.front());
        g.waiters.erase(g.waiters.begin());
        first.retry();
        bool waitsAgain = false;
        for (const Group& now : g_groups)
            if (now.className == g.className) { waitsAgain = true; break; }
        if (waitsAgain) {
            for (Waiter& w : g.waiters) Add(g.className, std::move(w));
        } else {
            for (Waiter& w : g.waiters) w.retry();
        }
    }
    // The groups the budget did not reach keep their place, behind any that parked anew this tick.
    for (; i < batch.size(); ++i)
        for (Waiter& w : batch[i].waiters) Add(batch[i].className, std::move(w));
}

}  // namespace coop::element::class_load_wait

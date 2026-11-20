// coop/props/prop_drive_join.cpp -- see coop/props/prop_drive_join.h.

#include "coop/props/prop_drive_join.h"

#include "coop/player/roster.h"
#include "coop/props/constraint_ties.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_element_tracker.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace coop::prop_drive_join {
namespace {

namespace PD = coop::prop_drive_host;
namespace PR = ue_wrap::prop;

constexpr size_t kTieCap = 32;     // the ties read of one prop at a time
constexpr size_t kWhyLen = 40;     // a cause's name, copied: a caller's string does not outlive its call

// A prop whose tied partners are owed rows.
struct Leader {
    ue_wrap::CachedObjRef ref;
    char why[kWhyLen];
};
std::vector<Leader> g_leaders;

// A member the join found past the cap, waiting for a slot or for its body to sleep.
struct Owed {
    ue_wrap::CachedObjRef ref;
    uint32_t leaderEid;
    char why[kWhyLen];
};
std::vector<Owed> g_owed;

uint64_t g_joined = 0;      // members that got a row at the walk
uint64_t g_owedCount = 0;   // members put on the owed list
uint64_t g_owedOpened = 0;  // owed members that got a row when a slot freed
uint64_t g_owedOver = 0;    // owed members that got a row past the cap, asleep

std::vector<void*> g_walk;  // the walk's visited set and queue, kept so a walk allocates nothing in the steady state

void CopyWhy(char (&to)[kWhyLen], const char* why) {
    std::snprintf(to, sizeof(to), "%s", why ? why : "");
}

// A counter is said for its first three and at each doubling after, as the drive's cap refusals are.
bool Say(uint64_t n) { return n <= 3 || (n & (n - 1)) == 0; }

uint32_t EidOf(void* actor) {
    const coop::element::ElementId eid = coop::prop_element_tracker::GetPropElementIdForActor(actor);
    return (eid == coop::element::kInvalidId) ? 0u : static_cast<uint32_t>(eid);
}

bool IsOwed(void* actor) {
    for (const Owed& o : g_owed)
        if (o.ref.Raw() == actor) return true;
    return false;
}

// One prop the walk reached. A member already driven (any author) is kept as it is; one with no host element id has no copy
// to move; one in a hand is the hand lane's. The rest get a row, or wait on the owed list past the cap.
void Member(void* actor, uint32_t leaderEid, const char* why) {
    if (PD::IsDriven(actor) || IsOwed(actor) || PD::InAHand(actor) || EidOf(actor) == 0u) return;
    if (PD::JoinSlotFree()) {
        if (PD::CoastJoined(actor, leaderEid, /*overCap=*/false, why) && Say(++g_joined))
            UE_LOGI("prop_drive_join: joined %llu (%zu ties indexed)", static_cast<unsigned long long>(g_joined),
                    coop::constraint_ties::Count());
        return;
    }
    Owed o;
    o.ref.Set(actor);
    o.leaderEid = leaderEid;
    CopyWhy(o.why, why);
    g_owed.push_back(o);
    if (Say(++g_owedCount))
        UE_LOGI("prop_drive_join: owed %llu -- eid=%u waits past the cap of the leader eid=%u",
                static_cast<unsigned long long>(g_owedCount), EidOf(actor), leaderEid);
}

// Breadth first over the ties of `leader`: every prop reached is walked through, a member or not, and each is visited once.
// Only a prop is walked through: a wall or a device two cords meet at would otherwise join props that share nothing.
void Walk(const Leader& l) {
    void* leader = l.ref.Get();
    if (!leader) return;
    const uint32_t leaderEid = EidOf(leader);
    g_walk.clear();
    g_walk.push_back(leader);
    for (size_t i = 0; i < g_walk.size(); ++i) {
        void* tied[kTieCap];
        const size_t n = coop::constraint_ties::TiesOf(g_walk[i], tied, kTieCap);
        for (size_t k = 0; k < n; ++k) {
            void* t = tied[k];
            if (std::find(g_walk.begin(), g_walk.end(), t) != g_walk.end() || !PR::IsDescendantOfProp(t)) continue;
            g_walk.push_back(t);
            Member(t, leaderEid, l.why);
        }
    }
}

// The owed members: dropped when they died, got a row or were taken by a hand; the oldest opened while a slot is free; and
// any whose body reads asleep opened past the cap, which the drive's rest rule ends on its next probe.
void ServeOwed() {
    for (size_t i = 0; i < g_owed.size();) {
        void* a = g_owed[i].ref.Get();
        if (!a || PD::IsDriven(a) || PD::InAHand(a)) g_owed.erase(g_owed.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
    while (!g_owed.empty() && PD::JoinSlotFree()) {
        const Owed o = g_owed.front();
        g_owed.erase(g_owed.begin());
        void* a = o.ref.Get();
        if (a && PD::CoastJoined(a, o.leaderEid, /*overCap=*/false, o.why) && Say(++g_owedOpened))
            UE_LOGI("prop_drive_join: owed-opened %llu -- eid=%u got its row when a slot freed",
                    static_cast<unsigned long long>(g_owedOpened), EidOf(a));
    }
    for (size_t i = 0; i < g_owed.size();) {
        void* a = g_owed[i].ref.Get();
        if (!a || !PD::BodyAsleepForEnd(a)) {
            ++i;
            continue;
        }
        const Owed o = g_owed[i];
        g_owed.erase(g_owed.begin() + static_cast<std::ptrdiff_t>(i));
        if (PD::CoastJoined(a, o.leaderEid, /*overCap=*/true, o.why) && Say(++g_owedOver))
            UE_LOGI("prop_drive_join: owed-overcap %llu -- eid=%u asleep past the cap, its row ends at its rest pose",
                    static_cast<unsigned long long>(g_owedOver), EidOf(a));
    }
}

}  // namespace

void Owe(void* leader, const char* why) {
    if (!leader || !coop::roster::LocalIsHost()) return;
    UE_ASSERT_GAME_THREAD("prop_drive_join::Owe");
    for (const Leader& l : g_leaders)
        if (l.ref.Raw() == leader) return;
    Leader l;
    l.ref.Set(leader);
    CopyWhy(l.why, why);
    g_leaders.push_back(l);
}

void Tick() {
    if (g_leaders.empty() && g_owed.empty()) return;
    UE_ASSERT_GAME_THREAD("prop_drive_join::Tick");
    for (const Leader& l : g_leaders) Walk(l);
    g_leaders.clear();
    ServeOwed();
}

void Clear() {
    g_leaders.clear();
    g_owed.clear();
    g_joined = g_owedCount = g_owedOpened = g_owedOver = 0;
}

}  // namespace coop::prop_drive_join

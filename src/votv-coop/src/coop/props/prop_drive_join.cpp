// coop/props/prop_drive_join.cpp -- see coop/props/prop_drive_join.h.

#include "coop/props/prop_drive_join.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/player/roster.h"
#include "coop/props/active_drive.h"          // NowMs
#include "coop/props/constraint_ties.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_drive_rules.h"
#include "coop/props/prop_element_tracker.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine_attach.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace coop::prop_drive_join {
namespace {

namespace PD = coop::prop_drive_host;
namespace E  = ue_wrap::engine;

constexpr size_t kTieCap = 32;     // the ties read of one prop at a time
constexpr size_t kWhyLen = 40;     // a cause's name, copied: a caller's string does not outlive its call
constexpr size_t kWalkCap = 64;    // the actors one owed walk visits; past it the walk stops
constexpr size_t kOverCapPerTick = 4;   // the over-cap rows ServeOwed opens in one tick

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
    uint64_t tick;   // the Tick that owed it: the over-cap path waits for a later one
    uint64_t nextProbeMs;   // its sleep is read no sooner: the drive's own re-probe spacing, not every frame
    char why[kWhyLen];
};
std::vector<Owed> g_owed;

uint64_t g_tick = 0;        // Tick calls this session
uint64_t g_joined = 0;      // members that got a row at the walk
uint64_t g_owedCount = 0;   // members put on the owed list
uint64_t g_owedOpened = 0;  // owed members that got a row when a slot freed
uint64_t g_owedOver = 0;    // owed members that got a row past the cap, asleep
uint64_t g_walkStopped = 0; // walks that stopped at kWalkCap

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
    o.tick = g_tick;
    o.nextProbeMs = coop::active_drive::NowMs() + coop::prop_drive_rules::kRestProbeMs;
    CopyWhy(o.why, why);
    // Developer red arm of the constraint-join drill (constraint_join_off=owed): the member is counted and dropped, never
    // owed. Resolved once, as prop_drive_host's other drill red arms are.
    static const bool s_owedOff = coop::config::ResolveEnum(coop::config_registry::rows::constraint_join_off) == "owed";
    if (!s_owedOff) g_owed.push_back(o);
    if (Say(++g_owedCount))
        UE_LOGI("prop_drive_join: owed %llu -- eid=%u waits past the cap of the leader eid=%u",
                static_cast<unsigned long long>(g_owedCount), EidOf(actor), leaderEid);
}

// Breadth first over the ties of `leader`, each actor visited once. The walk passes THROUGH any actor whose root body
// simulates physics (a prop, a cord or wire segment, a hook) and STOPS at one whose root does not (a frozen prop, a wall, a
// device, the world): a constraint to a fixed body drags nothing, and what hangs on that body moves with no one's carry.
// Only a member that is a keyed prop with an element id is given a row (Member).
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
            if (std::find(g_walk.begin(), g_walk.end(), t) != g_walk.end() || !E::IsActorRootSimulatingPhysics(t)) continue;
            if (g_walk.size() >= kWalkCap) {
                if (Say(++g_walkStopped))
                    UE_LOGI("prop_drive_join: a walk from eid=%u stopped at %zu", leaderEid, kWalkCap);
                return;
            }
            g_walk.push_back(t);
            Member(t, leaderEid, l.why);
        }
    }
}

// The owed members: dropped when they died, got a row or were taken by a hand; the oldest opened while a slot is free; and
// any whose body reads asleep opened past the cap, which the drive's rest rule ends once it rests.
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
    // At most kOverCapPerTick of these a tick, and none for a member owed in this very tick: a body that reads asleep at the
    // take has not been reached by the island's wake yet, so it is not yet at rest. A member's sleep is read once per
    // kRestProbeMs, as the drive's rows are: the read is an engine call.
    const uint64_t now = coop::active_drive::NowMs();
    size_t opened = 0;
    for (size_t i = 0; i < g_owed.size() && opened < kOverCapPerTick;) {
        Owed& w = g_owed[i];
        void* a = w.ref.Get();
        if (!a || w.tick == g_tick || now < w.nextProbeMs) {
            ++i;
            continue;
        }
        w.nextProbeMs = now + coop::prop_drive_rules::kRestProbeMs;
        if (!PD::BodyAsleepForEnd(a)) {
            ++i;
            continue;
        }
        const Owed o = g_owed[i];
        g_owed.erase(g_owed.begin() + static_cast<std::ptrdiff_t>(i));
        if (!PD::CoastJoined(a, o.leaderEid, /*overCap=*/true, o.why)) continue;
        ++opened;
        if (Say(++g_owedOver))
            UE_LOGI("prop_drive_join: owed-overcap %llu -- eid=%u asleep past the cap, its row ends at its rest pose",
                    static_cast<unsigned long long>(g_owedOver), EidOf(a));
    }
}

// A tie made while one of its ends is driven or in a hand: that end is owed a join, so the other end gets a row too.
void OweIfDriven(void* owner) {
    if (PD::IsDriven(owner) || PD::InAHand(owner)) Owe(owner, "tie made");
}

}  // namespace

void Owe(void* leader, const char* why) {
    if (!leader || !coop::roster::LocalIsHost()) return;
    // Developer red arm of the constraint-join drill (constraint_join_off=join): nothing is ever owed.
    static const bool s_joinOff = coop::config::ResolveEnum(coop::config_registry::rows::constraint_join_off) == "join";
    if (s_joinOff) return;
    UE_ASSERT_GAME_THREAD("prop_drive_join::Owe");
    for (const Leader& l : g_leaders)
        if (l.ref.Raw() == leader) return;
    Leader l;
    l.ref.Set(leader);
    CopyWhy(l.why, why);
    g_leaders.push_back(l);
}

// The leader's End is the island's announced rest, so its owed members are read there, not on their own phase. The opens
// are not spaced like ServeOwed's: a member left owed past its leader's End is what this exists to prevent. Their number is
// bounded by the walk that owed them (kWalkCap), once per take.
void OnRowEnded(uint32_t eid) {
    if (g_owed.empty() || eid == 0u || !coop::roster::LocalIsHost()) return;
    UE_ASSERT_GAME_THREAD("prop_drive_join::OnRowEnded");
    for (size_t i = 0; i < g_owed.size();) {
        if (g_owed[i].leaderEid != eid || g_owed[i].tick == g_tick) {
            ++i;   // not this leader's, or owed in this very tick (a body that never woke reads asleep: ServeOwed decides it)
            continue;
        }
        void* a = g_owed[i].ref.Get();
        if (!a || PD::IsDriven(a) || PD::InAHand(a) || !PD::BodyAsleepForEnd(a)) {
            ++i;   // gone, driven or held (ServeOwed drops it), or awake (ServeOwed serves it)
            continue;
        }
        const Owed o = g_owed[i];
        g_owed.erase(g_owed.begin() + static_cast<std::ptrdiff_t>(i));
        if (!PD::CoastJoined(a, o.leaderEid, /*overCap=*/true, o.why)) continue;
        if (Say(++g_owedOver))
            UE_LOGI("prop_drive_join: owed-overcap %llu -- eid=%u asleep at its leader eid=%u's End, its row ends at its rest pose",
                    static_cast<unsigned long long>(g_owedOver), EidOf(a), eid);
    }
}

void Tick() {
    ++g_tick;
    coop::constraint_ties::DrainTied(&OweIfDriven);
    if (g_leaders.empty() && g_owed.empty()) return;
    UE_ASSERT_GAME_THREAD("prop_drive_join::Tick");
    for (const Leader& l : g_leaders) Walk(l);
    g_leaders.clear();
    ServeOwed();
}

void Clear() {
    g_leaders.clear();
    g_owed.clear();
    g_tick = g_joined = g_owedCount = g_owedOpened = g_owedOver = g_walkStopped = 0;
}

}  // namespace coop::prop_drive_join

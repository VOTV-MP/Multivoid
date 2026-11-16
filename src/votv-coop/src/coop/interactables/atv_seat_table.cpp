// coop/interactables/atv_seat_table.cpp -- see coop/interactables/atv_seat_table.h. The one writer of an ATV entry's
// seat, syncer and rider, and its subscriber list.

#include "coop/interactables/atv_seat_table.h"

#include "coop/dev/atv_riders_arm.h"
#include "coop/interactables/atv_drill_events.h"
#include "coop/interactables/atv_rider_watch.h"
#include "coop/interactables/atv_sync_internal.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"

namespace coop::atv_seat_table {

namespace R = ue_wrap::reflection;
using coop::atv_sync::AtvEntry;

namespace {

constexpr int kMaxSubscribers = 4;
Subscriber g_subscribers[kMaxSubscribers] = {};
int g_subscriberCount = 0;

bool IsHost() {
    const coop::net::Session* s = coop::atv_sync::LaneSession();
    return s && s->role() == coop::net::Role::Host;
}

// Subscribers run in registration order. A nested change made by one of them is delivered in full from inside this
// loop, so the count is read at each step. The record of the change (the riders drill's) follows the loop, with how many
// subscribers ran. The riders drill's red `seat_announce_off` writes the change and calls none.
void Announce(const Change& c) {
    static const bool s_redAnnounce = coop::dev::atv_riders_arm::Is("seat_announce_off");
    int ran = 0;
    if (!s_redAnnounce)
        for (int i = 0; i < g_subscriberCount; ++i, ++ran) g_subscribers[i](c);
    namespace DE = coop::atv_drill_events;
    if (DE::On())
        DE::Note(DE::kSeatChange, (c.occupantBefore << 8) | c.occupantAfter, static_cast<int32_t>(c.riderAfter), ran,
                 static_cast<float>(c.riderBefore), 0.f, 0.f, DE::KeyTag(*c.key));
}

}  // namespace

bool OnChange(Subscriber fn) {
    if (!fn) return false;
    for (int i = 0; i < g_subscriberCount; ++i)
        if (g_subscribers[i] == fn) return true;
    if (g_subscriberCount >= kMaxSubscribers) {
        static bool sSaid = false;
        if (!sSaid) { sSaid = true;
            UE_LOGW("atv_seat_table: the subscriber list is full (%d) -- a subscriber was refused (said once)",
                    kMaxSubscribers); }
        return false;
    }
    g_subscribers[g_subscriberCount++] = fn;
    return true;
}

// MTA's server changes its occupant table in the in/out handlers (SetOccupant / SetOccupiedVehicle,
// reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:3152-3594), at a death (:2102-2103, :2162-2163) and at an
// element's removal (CVehicle.cpp:143-170, CPed.cpp:102-114), and sends each as a CVehicleInOutPacket. Ours keeps the ATV
// lane's one table and announces its changes in-process, here, whichever path wrote them.
void SetSeat(AtvEntry& e, uint8_t occupant, uint8_t syncer, const char* why, bool forgetHorn) {
    const uint8_t occupantBefore = e.seatOccupant;
    const uint8_t syncerBefore = e.seatSyncer;
    e.seatOccupant = occupant;
    e.seatSyncer = syncer;
    // The horn's last level replayed was the previous syncer's.
    if (forgetHorn || syncerBefore != syncer) coop::atv_sync::ForgetHorn(e);
    if (occupantBefore == occupant && syncerBefore == syncer) return;
    if (IsHost()) e.stampDue = true;
    Announce(Change{&e.key, e.actor, e.idx, occupantBefore, occupant, syncerBefore, syncer, e.seatRider, e.seatRider,
                    e.seatRiderActor, why});
}

void SetRider(AtvEntry& e, uint32_t eid, void* actor, const char* why) {
    const uint32_t riderBefore = e.seatRider;
    void* const riderActorBefore = e.seatRiderActor;
    const bool host = IsHost();
    e.seatRider = eid;
    if (host) e.seatRiderActor = actor;
    if (riderBefore == eid) return;
    if (host) e.stampDue = true;
    Announce(Change{&e.key, e.actor, e.idx, e.seatOccupant, e.seatOccupant, e.seatSyncer, e.seatSyncer, riderBefore, eid,
                    riderActorBefore, why});
}

void Exit(AtvEntry& e, const char* why) {
    coop::atv_rider_watch::ForgetSeedWait(e.key);   // the seed's wait for this entry ends with it, red or not
    static const bool s_redExit = coop::dev::atv_riders_arm::Is("exit_seat_off");   // the riders drill's red: no exit is announced
    if (s_redExit) return;
    SetSeat(e, 0xFF, 0, why, false);
    SetRider(e, 0, nullptr, why);
}

void* ResolveAtv(const std::wstring& key, int32_t& atvIdx) {
    const AtvEntry* e = coop::atv_sync::FindEntry(key);
    if (!e || !R::IsLiveByIndex(e->actor, e->idx)) return nullptr;
    atvIdx = e->idx;
    return e->actor;
}

bool ResolveSeat(const std::wstring& key, uint8_t seatIndex, void*& atv, int32_t& atvIdx, ue_wrap::FVector& loc,
                 ue_wrap::FRotator& rot) {
    if (seatIndex != 0) return false;
    void* actor = ResolveAtv(key, atvIdx);
    if (!actor || !ue_wrap::atv::GetSeatTransform(actor, loc, rot)) return false;
    atv = actor;
    return true;
}

}  // namespace coop::atv_seat_table

// coop/interactables/atv_riders.cpp -- see coop/interactables/atv_riders.h.

#include "coop/interactables/atv_riders.h"

#include "coop/creatures/npc_ride.h"
#include "coop/creatures/npc_sync.h"
#include "coop/interactables/atv_rider_watch.h"
#include "coop/interactables/atv_seat_table.h"
#include "coop/interactables/atv_sync.h"
#include "coop/interactables/atv_sync_internal.h"
#include "coop/element/mirror_defer.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_seat.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::atv_riders {
namespace {

namespace R = ue_wrap::reflection;

std::atomic<coop::net::Session*> g_session{nullptr};

// The host's tallies, said at the session end.
uint32_t g_calls = 0, g_releases = 0;

bool IsClientSlot(uint8_t slot) { return slot != coop::players::kPeerIdUnknown && slot != 0; }

// The host's kerfur steps on a seat edge, for a CLIENT's seat only: the host's own seat keeps the game's own sit and
// dismount, which call the kerfurs already. The release of the slot that left runs before the invite of the slot that came.
// A live ATV takes the ordinary release. An ATV that is gone has nothing to release through: the kerfurs it invited and did
// not seat are turned back by their own failCar. They are found by their `occupyCar`, compared with the gone ATV's pointer
// and never dereferenced; the seated rider (the entry's riderActorBefore) also carries it, and failCar would half-reset it.
void HostKerfurSteps(const coop::atv_seat_table::Change& c) {
    if (c.occupantBefore == c.occupantAfter) return;
    const bool live = R::IsLiveByIndex(c.actor, c.actorIdx);
    if (IsClientSlot(c.occupantBefore)) {
        if (live) {
            if (ue_wrap::atv::ReleaseKerfurs(c.actor)) {
                ++g_releases;
                UE_LOGI("atv_riders: the client's seat on %ls ends -- released", c.key->c_str());
            }
        } else {
            uint32_t turned = 0;
            for (void* k : ue_wrap::kerfur::AllKerfurs()) {
                if (k == c.riderActorBefore || !ue_wrap::kerfur::OccupiesCar(k, c.actor)) continue;
                if (ue_wrap::kerfur::FailCar(k)) ++turned;
            }
            UE_LOGI("atv_riders: %ls is gone -- %u invited kerfur(s) turned back", c.key->c_str(), turned);
        }
    }
    if (live && IsClientSlot(c.occupantAfter)) {
        const std::vector<void*> kerfurs = ue_wrap::kerfur::AllKerfurs();
        if (ue_wrap::atv::InviteKerfurs(c.actor, kerfurs)) {
            ++g_calls;
            UE_LOGI("atv_riders: a client sits on %ls -- %zu kerfur(s) invited", c.key->c_str(), kerfurs.size());
        }
    }
}

// The body subscriber, on every peer: a slot other than this peer's own that takes the seat is seated, and one that leaves it
// is let go. The host's own seat appears on a client as slot 0, so a client seats the host's body by the same rule.
void OnSeatChange(const coop::atv_seat_table::Change& c) {
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    const auto another = [self](uint8_t slot) { return slot != coop::players::kPeerIdUnknown && slot != self; };
    if (another(c.occupantAfter)) coop::puppet_seat::Set(c.occupantAfter, *c.key, 0);
    if (another(c.occupantBefore) && c.occupantBefore != c.occupantAfter)
        coop::puppet_seat::Clear(c.occupantBefore, *c.key);
    const coop::net::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    // The host's kerfurs answer a client's seat on the seat's own edge.
    if (s->role() == coop::net::Role::Host) { HostKerfurSteps(c); return; }
    // The rider subscriber, on a client: the host's kerfur rides the ATV its stamp names, on this copy too.
    if (s->role() != coop::net::Role::Client || c.riderBefore == c.riderAfter) return;
    if (c.riderBefore != 0) coop::npc_ride::Clear(c.riderBefore, *c.key);
    if (c.riderAfter != 0) coop::npc_ride::Set(c.riderAfter, *c.key);
}

// The host retired an NPC element, on whichever thread its body ran: the table is the game thread's, so the work is posted,
// and the task finds the ATV that names that eid and ends its ride. The eid's free is deferred to the element flush, so it is
// not re-issued before the task runs.
void OnNpcRetired(uint32_t eid) {
    ue_wrap::game_thread::Post([eid] {
        coop::net::Session* s = g_session.load(std::memory_order_acquire);
        if (!s || s->role() != coop::net::Role::Host) return;
        if (coop::atv_sync::AtvEntry* e = coop::atv_sync::FindEntryByRider(eid))
            coop::atv_seat_table::SetRider(*e, 0, nullptr, "retired");
    });
}

}  // namespace

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    coop::npc_sync::OnHostNpcRetired(&OnNpcRetired);
    coop::atv_rider_watch::Install(session);
    coop::atv_seat_table::OnChange(&OnSeatChange);
    coop::puppet_seat::SetResolver(&coop::atv_seat_table::ResolveSeat);
    coop::npc_ride::SetResolver(&coop::atv_seat_table::ResolveAtv);
    coop::mirror_defer::OnReveal(&coop::npc_ride::OnRevealed);
}

void OnDisconnect() {
    coop::puppet_seat::OnDisconnect();
    if (g_calls || g_releases)
        UE_LOGI("atv_riders: session end -- kerfur calls=%u releases=%u", g_calls, g_releases);
    g_calls = g_releases = 0;
    coop::atv_rider_watch::OnDisconnect();
}

}  // namespace coop::atv_riders

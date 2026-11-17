// coop/interactables/atv_riders.cpp -- see coop/interactables/atv_riders.h.

#include "coop/interactables/atv_riders.h"

#include "coop/creatures/npc_ride.h"
#include "coop/creatures/npc_sync.h"
#include "coop/dev/atv_riders_arm.h"
#include "coop/dev/atv_riders_join_hold.h"
#include "coop/interactables/atv_drill_events.h"
#include "coop/interactables/atv_rider_watch.h"
#include "coop/interactables/atv_seat_table.h"
#include "coop/interactables/atv_sync.h"
#include "coop/interactables/atv_sync_internal.h"
#include "coop/element/mirror_defer.h"
#include "coop/element/mirror_managers.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_seat.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine_attach.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::atv_riders {
namespace {

namespace R = ue_wrap::reflection;
namespace DE = coop::atv_drill_events;
namespace arm = coop::dev::atv_riders_arm;

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
                if (DE::On()) DE::Note(DE::kInvite, DE::KeyTag(*c.key), 0, 0);
            }
        } else {
            uint32_t turned = 0;
            for (void* k : ue_wrap::kerfur::AllKerfurs()) {
                if (k == c.riderActorBefore || !ue_wrap::kerfur::OccupiesCar(k, c.actor)) continue;
                if (ue_wrap::kerfur::FailCar(k)) ++turned;
            }
            UE_LOGI("atv_riders: %ls is gone -- %u invited kerfur(s) turned back", c.key->c_str(), turned);
            if (DE::On()) DE::Note(DE::kInvite, DE::KeyTag(*c.key), 2, static_cast<int32_t>(turned));
        }
    }
    // The riders drill's red `invite_off`: the seat edge invites no kerfur.
    static const bool s_redInvite = arm::Is("invite_off");
    if (live && IsClientSlot(c.occupantAfter) && !s_redInvite) {
        const std::vector<void*> kerfurs = ue_wrap::kerfur::AllKerfurs();
        if (ue_wrap::atv::InviteKerfurs(c.actor, kerfurs)) {
            ++g_calls;
            UE_LOGI("atv_riders: a client sits on %ls -- %zu kerfur(s) invited", c.key->c_str(), kerfurs.size());
            if (DE::On()) DE::Note(DE::kInvite, DE::KeyTag(*c.key), 1, static_cast<int32_t>(kerfurs.size()));
        }
    }
}

// The riders drill's record of what still held for the rider's eid once the ride owner's Clear returned: a ride row, the
// owner's hold on the mirror's pose, and an attach parent of the mirror. Read here, at the stamp apply that cleared the rider.
void NoteRideCleared(uint32_t eid, const std::wstring& key) {
    int32_t held = 0;
    if (coop::npc_ride::HasRow(eid)) held |= 1;
    if (coop::npc_ride::HoldsPose(eid)) held |= 2;
    coop::element::Npc* const npc = coop::element::NpcMirrors().Get(eid);
    void* const actor = npc ? npc->GetActor() : nullptr;
    void* parent = nullptr;
    if (actor && R::IsLiveByIndex(actor, npc->GetInternalIdx()) && ue_wrap::engine::GetAttachParentActor(actor, parent) && parent)
        held |= 4;
    DE::Note(DE::kRideCleared, static_cast<int32_t>(eid), DE::KeyTag(key), held);
}

// The body subscriber, on every peer: a slot other than this peer's own that takes the seat is seated, and one that leaves it
// is let go. The host's own seat appears on a client as slot 0, so a client seats the host's body by the same rule.
void OnSeatChange(const coop::atv_seat_table::Change& c) {
    // The riders drill's reds: `release_seat_off` (a seat's end does not release the body), `mirror_ride_off` (the stamp's
    // rider is not applied) and `release_ride_off` (the rider's end does not unseat the mirror).
    static const bool s_redReleaseSeat = arm::Is("release_seat_off");
    static const bool s_redMirrorRide = arm::Is("mirror_ride_off");
    static const bool s_redReleaseRide = arm::Is("release_ride_off");
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    const auto another = [self](uint8_t slot) { return slot != coop::players::kPeerIdUnknown && slot != self; };
    if (another(c.occupantAfter)) coop::puppet_seat::Set(c.occupantAfter, *c.key, 0);
    if (another(c.occupantBefore) && c.occupantBefore != c.occupantAfter && !s_redReleaseSeat)
        coop::puppet_seat::Clear(c.occupantBefore, *c.key);
    const coop::net::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s) return;
    // The host's kerfurs answer a client's seat on the seat's own edge.
    if (s->role() == coop::net::Role::Host) { HostKerfurSteps(c); return; }
    // The rider subscriber, on a client: the host's kerfur rides the ATV its stamp names, on this copy too.
    if (s->role() != coop::net::Role::Client || c.riderBefore == c.riderAfter) return;
    if (c.riderBefore != 0 && !s_redReleaseRide) {
        coop::npc_ride::Clear(c.riderBefore, *c.key);
        if (DE::On()) NoteRideCleared(c.riderBefore, *c.key);
    }
    if (c.riderAfter != 0 && !s_redMirrorRide) coop::npc_ride::Set(c.riderAfter, *c.key);
}

// The owners' outcomes, as the drill's records (the owners know nothing of them: they only call an observer).
void OnBodyOutcome(uint8_t slot, int outcome, const std::wstring& key, float offsetCm, bool onAtv, bool collision) {
    DE::Note(DE::kBodySeat, slot, outcome, DE::KeyTag(key), offsetCm, onAtv ? 1.f : 0.f, collision ? 1.f : 0.f);
}

void OnRideOutcome(uint32_t eid, int outcome, const std::wstring& key, float sitarrowCm, bool wasAttached) {
    DE::Note(DE::kRide, static_cast<int32_t>(eid), outcome, DE::KeyTag(key), sitarrowCm, wasAttached ? 1.f : 0.f);
    // The drill's step 0 names the kerfur its lift hold waits for from the first row that rides, waits or is hidden.
    if (outcome == coop::npc_ride::kRides || outcome == coop::npc_ride::kHeldUnbound || outcome == coop::npc_ride::kHeldHidden)
        coop::dev::atv_riders_join_hold::NoteRideEid(eid, outcome != coop::npc_ride::kHeldUnbound);
}

// The host retired an NPC element, on whichever thread its body ran: the table is the game thread's, so the work is posted on
// every retire (a retire is a cold path, and a gate read on this thread could miss a rider named after it), and the task finds
// the ATV that names that eid and ends its ride. The eid's free is deferred to the element flush, so it is not re-issued before
// the task runs.
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
    // The riders drill's red `reveal_ride_off`: the reveal listener is not registered.
    static const bool s_redReveal = arm::Is("reveal_ride_off");
    if (!s_redReveal) coop::mirror_defer::OnReveal(&coop::npc_ride::OnRevealed);
    coop::dev::atv_riders_join_hold::InstallLiftHold();   // the riders drill's step 0: the driver's lift waits for the kerfur's bind
    if (DE::On()) {
        coop::puppet_seat::SetObserver(&OnBodyOutcome);
        coop::npc_ride::SetObserver(&OnRideOutcome);
    }
}

void OnDisconnect() {
    coop::puppet_seat::OnDisconnect();
    if (g_calls || g_releases)
        UE_LOGI("atv_riders: session end -- kerfur calls=%u releases=%u", g_calls, g_releases);
    g_calls = g_releases = 0;
    g_session.store(nullptr, std::memory_order_release);   // the next Install names the next session
    coop::atv_rider_watch::OnDisconnect();
}

}  // namespace coop::atv_riders

// coop/props/prop_hold.cpp -- see coop/props/prop_hold.h.

#include "coop/props/prop_hold.h"

#include "coop/items/hook_sync.h"
#include "coop/player/local_streams.h"
#include "coop/player/players_registry.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/engine/engine.h"

#include <array>

namespace coop::prop_hold {
namespace {

namespace E = ue_wrap::engine;

constexpr int kSlots = coop::players::kMaxPeers;

struct Reservation {
    bool held = false;
    uint16_t id = 0;
    uint32_t eid = 0;
    ue_wrap::CachedObjRef actor;
};

// A plain slot-indexed array, released by the commit, the abort or the slot's departure and by nothing that resets
// on its own: the release has to be load-bearing. The shape is MTA's vehicle entry (reserve CGame.cpp:3152, confirm
// :3196-3197), held in a per-slot table beside the other sources of "held" instead of a two-way link on the element.
// A reservation outlives a destroyed target until the commit or abort releases it; MTA unlinks at destroy
// (CVehicle.cpp:166-172).
std::array<Reservation, kSlots> g_reservations;

bool InTable(int slot) { return slot >= 1 && slot < kSlots; }

// The bit of the requester's own slot in a hook-holder mask; nothing for a requester outside the roster.
uint32_t OwnBit(int requester) { return requester >= 0 && requester < kSlots ? 1u << requester : 0u; }

}  // namespace

bool HostHolds(void* actor) {
    UE_ASSERT_GAME_THREAD("prop_hold::HostHolds");
    void* local = coop::players::Registry::Get().Local();
    E::MainPlayerGrabState g{};
    return local && E::ReadMainPlayerGrabState(local, g) && (g.grabbingActor == actor || g.holdingActor == actor);
}

Holder WhoHolds(void* actor, int requester) {
    UE_ASSERT_GAME_THREAD("prop_hold::WhoHolds");
    if (!actor) return Holder::None;
    // The requester's own drag never counts, and a drag is not refused by a reservation: the later claimant ends the
    // first's hold, as MTA's jack (CGame.cpp:3128-3150) and Source's force-drop (basecombatcharacter.cpp:1188).
    if (coop::remote_prop::IsActorDrivenByOtherThan(actor, requester)) return Holder::Drive;
    // The claim pass's reading, at most one 20 Hz pass (50 ms) old; MTA sets occupancy in the judging call
    // (CGame.cpp:3152).
    if (coop::hook_sync::PlayerHookHolders(actor) & ~OwnBit(requester)) return Holder::Hook;
    if (requester != 0 && (HostHolds(actor) || actor == coop::local_streams::LastHeldActor()))
        return Holder::HostHand;
    if (ReservedByOther(actor, requester)) return Holder::Reservation;
    return Holder::None;
}

bool HeldByOther(void* actor, int requester) {
    UE_ASSERT_GAME_THREAD("prop_hold::HeldByOther");
    return WhoHolds(actor, requester) != Holder::None;
}

const char* HolderWhy(Holder h) {
    UE_ASSERT_GAME_THREAD("prop_hold::HolderWhy");
    switch (h) {
    case Holder::Drive:       return "held by a peer's drive";
    case Holder::Hook:        return "pulled by a hook";
    case Holder::HostHand:    return "held by the host's own grab";
    case Holder::Reservation: return "reserved by another peer's take";
    case Holder::None:        break;
    }
    return "";
}

bool ReservedByOther(void* actor, int requester) {
    UE_ASSERT_GAME_THREAD("prop_hold::ReservedByOther");
    if (!actor) return false;
    for (int s = 1; s < kSlots; ++s)
        if (s != requester && g_reservations[s].held && g_reservations[s].actor.Is(actor)) return true;
    return false;
}

bool Reserve(int slot, uint16_t id, uint32_t eid, void* actor) {
    UE_ASSERT_GAME_THREAD("prop_hold::Reserve");
    if (!InTable(slot) || g_reservations[slot].held) return false;
    Reservation& r = g_reservations[slot];
    r.held = true;
    r.id = id;
    r.eid = eid;
    r.actor.Set(actor);
    return true;
}

bool HoldsReservation(int slot) {
    UE_ASSERT_GAME_THREAD("prop_hold::HoldsReservation");
    return InTable(slot) && g_reservations[slot].held;
}

bool Match(int slot, uint16_t id, uint32_t eid, void** outActor) {
    UE_ASSERT_GAME_THREAD("prop_hold::Match");
    if (!InTable(slot)) return false;
    const Reservation& r = g_reservations[slot];
    if (!r.held || r.id != id || r.eid != eid) return false;
    if (outActor) *outActor = r.actor.Get();
    return true;
}

void Release(int slot) {
    UE_ASSERT_GAME_THREAD("prop_hold::Release");
    if (!InTable(slot)) return;
    Reservation& r = g_reservations[slot];
    r.held = false;
    r.id = 0;
    r.eid = 0;
    r.actor.Reset();
}

int ReservationCount(int slot) {
    UE_ASSERT_GAME_THREAD("prop_hold::ReservationCount");
    return HoldsReservation(slot) ? 1 : 0;
}

uint32_t ReservedEid(int slot) {
    UE_ASSERT_GAME_THREAD("prop_hold::ReservedEid");
    return HoldsReservation(slot) ? g_reservations[slot].eid : 0u;
}

}  // namespace coop::prop_hold

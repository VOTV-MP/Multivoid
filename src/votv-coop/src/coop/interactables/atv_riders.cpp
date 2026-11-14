// coop/interactables/atv_riders.cpp -- see coop/interactables/atv_riders.h.

#include "coop/interactables/atv_riders.h"

#include "coop/creatures/npc_sync.h"
#include "coop/interactables/atv_rider_watch.h"
#include "coop/interactables/atv_seat_table.h"
#include "coop/interactables/atv_sync.h"
#include "coop/interactables/atv_sync_internal.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_seat.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"

#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

namespace coop::atv_riders {
namespace {

namespace R = ue_wrap::reflection;

std::atomic<coop::net::Session*> g_session{nullptr};

// The host's: each ATV whose kerfurs it called for a client's seat, with that ATV's object slot.
std::unordered_map<void*, int32_t> g_called;
uint32_t g_calls = 0, g_releases = 0;

// The body subscriber, on every peer: a slot other than this peer's own that takes the seat is seated, and one that leaves it
// is let go. The host's own seat appears on a client as slot 0, so a client seats the host's body by the same rule.
void OnSeatChange(const coop::atv_seat_table::Change& c) {
    const uint8_t self = coop::players::Registry::Get().LocalPeerId();
    const auto another = [self](uint8_t slot) { return slot != coop::players::kPeerIdUnknown && slot != self; };
    if (another(c.occupantAfter)) coop::puppet_seat::Set(c.occupantAfter, *c.key, 0);
    if (another(c.occupantBefore) && c.occupantBefore != c.occupantAfter)
        coop::puppet_seat::Clear(c.occupantBefore, *c.key);
}

void CallKerfurs(const uint8_t self) {
    struct Seen { uint8_t self; std::vector<void*> seated, free; } seen{self, {}, {}};
    coop::atv_sync::ForEachIndexed([](void* ctx, const std::wstring& key, void* actor) {
        auto* s = static_cast<Seen*>(ctx);
        coop::atv_sync::TableView tv;
        if (!coop::atv_sync::ReadTable(key, tv)) return;
        const bool client = tv.occupantSlot != 0xFF && tv.occupantSlot != s->self;
        (client ? s->seated : s->free).push_back(actor);
    }, &seen);
    for (void* atv : seen.seated) {
        if (g_called.count(atv)) continue;
        const std::vector<void*> kerfurs = ue_wrap::kerfur::AllKerfurs();
        if (!ue_wrap::atv::InviteKerfurs(atv, kerfurs)) continue;
        g_called[atv] = R::InternalIndexOf(atv);
        ++g_calls;
        UE_LOGI("atv_riders: a client sits on ATV %p -- %zu kerfur(s) called, as the ATV's own sit calls them", atv,
                kerfurs.size());
    }
    for (void* atv : seen.free) {
        auto it = g_called.find(atv);
        if (it == g_called.end()) continue;
        g_called.erase(it);
        if (!ue_wrap::atv::ReleaseKerfurs(atv)) continue;
        ++g_releases;
        UE_LOGI("atv_riders: the client's seat on ATV %p ends -- its kerfur is off, as the ATV's own dismount sends it",
                atv);
    }
    // An ATV that left the index: nothing to send off.
    for (auto it = g_called.begin(); it != g_called.end();)
        it = R::IsLiveByIndex(it->first, it->second) ? std::next(it) : g_called.erase(it);
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
}

void Tick() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    if (s->role() == coop::net::Role::Host) CallKerfurs(coop::players::Registry::Get().LocalPeerId());
}

void OnDisconnect() {
    coop::puppet_seat::OnDisconnect();
    if (g_calls || g_releases)
        UE_LOGI("atv_riders: session end -- kerfur calls=%u releases=%u", g_calls, g_releases);
    g_called.clear();
    g_calls = g_releases = 0;
    coop::atv_rider_watch::OnDisconnect();
}

}  // namespace coop::atv_riders

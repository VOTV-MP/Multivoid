// coop/interactables/atv_riders.cpp -- see coop/interactables/atv_riders.h.

#include "coop/interactables/atv_riders.h"

#include "coop/interactables/atv_sync.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/actors/kerfur.h"
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
uint32_t g_calls = 0, g_releases = 0, g_bumps = 0;

void SeatBodies() {
    auto& reg = coop::players::Registry::Get();
    const uint8_t self = reg.LocalPeerId();
    for (uint8_t slot = 0; slot < coop::players::kMaxPeers; ++slot) {
        if (slot == self) continue;
        coop::RemotePlayer* p = reg.Puppet(slot);
        if (!p || !p->valid()) continue;
        void* driven = coop::atv_sync::ActorSeatedBy(slot);
        void* ridden = driven ? nullptr : coop::atv_sync::ActorRiddenBy(slot);
        p->SetSeat(driven ? driven : ridden, ridden != nullptr);
    }
}

void CallKerfurs(const uint8_t self) {
    struct Seen { uint8_t self; std::vector<void*> seated, free, ridden; } seen{self, {}, {}, {}};
    coop::atv_sync::ForEachIndexed([](void* ctx, const std::wstring& key, void* actor) {
        auto* s = static_cast<Seen*>(ctx);
        coop::atv_sync::TableView tv;
        if (!coop::atv_sync::ReadTable(key, tv)) return;
        if (tv.passengerSlot != 0xFF) { s->ridden.push_back(actor); return; }
        const bool client = tv.occupantSlot != 0xFF && tv.occupantSlot != s->self;
        (client ? s->seated : s->free).push_back(actor);
    }, &seen);
    // A player in the passenger spot has it over a kerfur: the rider gets off, as the ATV's own dismount sends it, and
    // the rest turn back. The host's own sit calls the kerfurs itself, so one may walk up later and is sent off then.
    for (void* atv : seen.ridden) {
        g_called.erase(atv);
        if (!ue_wrap::atv::RidingKerfur(atv) || !ue_wrap::atv::ReleaseKerfurs(atv)) continue;
        ++g_bumps;
        UE_LOGI("atv_riders: a player rides behind the driver of ATV %p -- its kerfur is off", atv);
    }
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

}  // namespace

void Install(coop::net::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    coop::net::Session* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    SeatBodies();
    if (s->role() == coop::net::Role::Host) CallKerfurs(coop::players::Registry::Get().LocalPeerId());
}

void OnDisconnect() {
    auto& reg = coop::players::Registry::Get();
    for (uint8_t slot = 0; slot < coop::players::kMaxPeers; ++slot)
        if (coop::RemotePlayer* p = reg.Puppet(slot)) p->SetSeat(nullptr);
    if (g_calls || g_releases || g_bumps)
        UE_LOGI("atv_riders: session end -- kerfur calls=%u releases=%u bumps=%u", g_calls, g_releases, g_bumps);
    g_called.clear();
    g_calls = g_releases = g_bumps = 0;
}

}  // namespace coop::atv_riders

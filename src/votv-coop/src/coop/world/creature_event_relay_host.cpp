// coop/world/creature_event_relay_host.cpp -- the host's half of coop/world/creature_event_relay.h: each enrolled
// creature's net count of setEvent(true) calls, sent whole to every client on every change, at the creature's
// enrolment after its own `true`, and at a joiner's world-ready; a creature that ends still holding a count sends 0.

#include "coop/world/creature_event_relay.h"
#include "creature_event_relay_internal.h"  // co-located private header (src tree, not include/)

#include "coop/element/death_seam.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/world/active_events.h"

#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace coop::creature_event_relay::detail {
namespace {

namespace R  = ue_wrap::reflection;
namespace AE = ue_wrap::active_events;
namespace EL = coop::element;

// A creature's count and the world it was counted in; a count of another world's gamemode is dropped where it is
// met.
struct HostEntry {
    int      count;
    uint32_t gen;
};
std::unordered_map<EL::ElementId, HostEntry> g_host;

// The entry for `eid` in this world, or null; an entry of another world is erased on the way.
HostEntry* Find(EL::ElementId eid) {
    const auto it = g_host.find(eid);
    if (it == g_host.end()) return nullptr;
    if (it->second.gen != ue_wrap::world_identity::Generation()) {
        g_host.erase(it);
        return nullptr;
    }
    return &it->second;
}

// Record `count` for `eid` (clamped to the cap, a creature already at it held there); false when the table is
// full for a new creature.
bool Record(EL::ElementId eid, int count) {
    HostEntry* e = Find(eid);
    if (!e) {
        if (g_host.size() >= kMaxEntries) {
            UE_LOGW("creature_event_relay: host eid=%u count=%d refused -- %zu creatures already hold a count", eid,
                    count, kMaxEntries);
            return false;
        }
        e = &g_host[eid];
        e->gen = ue_wrap::world_identity::Generation();
    }
    e->count = count;
    return true;
}

coop::net::CreatureEventActivePayload PayloadFor(EL::ElementId eid, int count) {
    coop::net::CreatureEventActivePayload p{};
    p.elementId = static_cast<uint32_t>(eid);
    p.count = static_cast<uint8_t>(count);
    return p;
}

// To every client that is world-ready; a client still joining gets the count in its snapshot. Every call is sent, even
// one that repeats the last value: diverges from MTA's unchanged-value skip
// (reference/mtasa-blue/Server/mods/deathmatch/logic/CStaticFunctionDefinitions.cpp:1019), because a repeated value
// re-sends what a client may hold stale (a snapshot's target, a missed edge). The whole value is written at :1031 and
// broadcast at :1034, as here.
void Broadcast(coop::net::Session* s, EL::ElementId eid, int count) {
    const coop::net::CreatureEventActivePayload p = PayloadFor(eid, count);
    if (s->SendReliable(coop::net::ReliableKind::CreatureEventActive, &p, sizeof(p)))
        UE_LOGI("creature_event_relay: host eid=%u count=%d sent", eid, count);
    else
        UE_LOGI("creature_event_relay: host eid=%u count=%d not sent (no world-ready client, or the send failed)", eid,
                count);
}

// A creature's copies in the registry, held at the cap.
int Capped(int copies) {
    if (copies > kMaxCount) {
        UE_LOGW("creature_event_relay: %d registrations held at %d", copies, kMaxCount);
        return kMaxCount;
    }
    return copies;
}

}  // namespace

// The registry reads both halves make.
EL::ElementId NpcEidOf(void* actor, bool mirror) {
    if (!actor) return EL::kInvalidId;
    EL::Registry& reg = EL::Registry::Get();
    const EL::ElementId eid = reg.EidForActor(actor);
    const EL::Element* e = eid == EL::kInvalidId ? nullptr : reg.Get(eid);
    return (e && e->GetType() == EL::ElementType::Npc && e->IsMirror() == mirror) ? eid : EL::kInvalidId;
}

bool CopiesInRegistry(void* actor, int& copies) {
    AE::Senders arr{};
    if (!AE::EnsureResolved() || !AE::ReadSenders(arr) || arr.num < 0 || arr.num > 4096) return false;
    copies = 0;
    for (int32_t i = 0; i < arr.num; ++i)
        if (arr.data && arr.data[i] == actor) ++copies;
    return true;
}

void HostPost(void* ctx, bool active) {
    // No liveness test on the context: the destroy's own false runs inside the creature's ReceiveDestroyed, where the
    // creature may already read as dying, and its 0 must still be counted and sent. The context is the frame the
    // Blueprint passed, valid for the call; the registry lookup reads only the object array.
    coop::net::Session* s = ActiveSession(coop::net::Role::Host);
    if (!s || !ctx) return;
    const EL::ElementId eid = NpcEidOf(ctx, /*mirror=*/false);
    if (eid == EL::kInvalidId || IsPerViewer(ctx)) return;
    const HostEntry* held = Find(eid);
    int count = held ? held->count : 0;
    if (active) {
        if (count >= kMaxCount) UE_LOGW("creature_event_relay: host eid=%u held at %d", eid, kMaxCount);
        else ++count;
    } else if (count > 0) {
        --count;
    }
    if (count == 0) g_host.erase(eid);
    else if (!Record(eid, count)) return;
    Broadcast(s, eid, count);
}

void HostOnNpcEnrolled(EL::ElementId eid, void* actor) {
    coop::net::Session* s = ActiveSession(coop::net::Role::Host);
    if (!s || !actor || IsPerViewer(actor) || Find(eid)) return;
    int copies = 0;
    if (!CopiesInRegistry(actor, copies) || copies <= 0) return;
    const int count = Capped(copies);
    if (Record(eid, count)) Broadcast(s, eid, count);
}

void HostOnNpcEnd(const EL::death_seam::Death& death) {
    const auto it = g_host.find(death.eid);
    if (it == g_host.end()) return;
    const int held = it->second.count;
    g_host.erase(it);
    if (held <= 0) return;
    UE_LOGW("creature_event_relay: host eid=%u ended at count %d -- entry dropped", death.eid, held);
    // After the creature's EntityDestroy on the same Default lane. MTA drops an element's custom data with the element
    // (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientEntity.cpp:86-90, the destructor frees it); our count
    // lives in the client's relay table keyed by eid, not on the element, so the end sends the zero that clears it: a
    // client entry that never bound has no mirror to end it.
    if (coop::net::Session* s = ActiveSession(coop::net::Role::Host)) Broadcast(s, death.eid, 0);
}

void HostSendJoinSnapshot(coop::net::Session* session, int slot) {
    // The creatures the game's own registry names and the map does not hold (a creature that began before the relay
    // watched, or whose enrolment missed it) are recorded as they are sent, so the map and every client agree.
    AE::Senders arr{};
    if (AE::EnsureResolved() && AE::ReadSenders(arr) && arr.num >= 0 && arr.num <= 4096 && arr.data) {
        std::vector<std::pair<void*, int>> distinct;
        for (int32_t i = 0; i < arr.num; ++i) {
            void* obj = arr.data[i];
            if (!obj) continue;
            bool known = false;
            for (auto& d : distinct) {
                if (d.first != obj) continue;
                ++d.second;
                known = true;
                break;
            }
            if (!known) distinct.emplace_back(obj, 1);
        }
        for (const auto& d : distinct) {
            if (!R::IsLive(d.first)) continue;
            const EL::ElementId eid = NpcEidOf(d.first, /*mirror=*/false);
            if (eid == EL::kInvalidId || IsPerViewer(d.first) || Find(eid)) continue;
            Record(eid, Capped(d.second));
        }
    }
    // A separate message after the NPC queue: diverges from MTA carrying element data inside the add packet
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/packets/CEntityAddPacket.cpp:177-190), because the NPC spawn
    // kind carries no event state.
    int sent = 0;
    EL::Registry& reg = EL::Registry::Get();
    for (auto it = g_host.begin(); it != g_host.end();) {
        const auto kv = *it;  // a copy, so the erase and the advance leave it valid
        const EL::Element* el = reg.Get(kv.first);
        if (!el || el->GetType() != EL::ElementType::Npc) {  // its creature is gone from the registry
            // The seam's end may not have drained yet and then finds no entry, so the zero is sent here: a client
            // entry that never bound keeps its target otherwise.
            it = g_host.erase(it);
            Broadcast(session, kv.first, 0);
            continue;
        }
        ++it;
        if (kv.second.gen != ue_wrap::world_identity::Generation()) continue;
        const coop::net::CreatureEventActivePayload p = PayloadFor(kv.first, kv.second.count);
        if (session->SendReliableToSlot(slot, coop::net::ReliableKind::CreatureEventActive, &p, sizeof(p))) {
            UE_LOGI("creature_event_relay: join-edge slot=%d eid=%u count=%d", slot, kv.first, kv.second.count);
            ++sent;
        } else {
            UE_LOGW("creature_event_relay: join-edge slot=%d eid=%u count send FAILED", slot, kv.first);
        }
    }
    UE_LOGI("creature_event_relay: join-edge slot=%d sent %d creature event(s)", slot, sent);
}

void HostOnDisconnect() { g_host.clear(); }

}  // namespace coop::creature_event_relay::detail

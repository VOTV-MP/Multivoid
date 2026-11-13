// coop/world/explosion_request_atv.cpp -- the explosion request lane's fourth family: an ATV a client syncs that
// exploded (see coop/world/explosion_request.h). The ATV's own `explode` runs on the client's copy, which ejects the
// driver and spawns a blast there that the client's birth seam dissolves; the client asks the host for the blast
// (OnClientAtvBirth), and the host, after judging the sender is the ATV's syncer, spawns it at the ATV's own `tp`
// (OnAtvRequestReceived, JudgeAtvBlast). A host-owned ATV's death runs natively and the host's capture carries its
// blast, so it needs no request. Game thread.

#include "coop/world/explosion_request.h"

#include "explosion_request_internal.h"  // co-located private header (src tree, not include/)

#include "coop/interactables/atv_authority.h"
#include "coop/interactables/atv_hit_guard.h"
#include "coop/interactables/atv_sync.h"
#include "coop/interactables/atv_sync_internal.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/world/explosion_recipe.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/world/explosion_source.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace coop::explosion_request {

namespace {

namespace RC = coop::explosion_recipe;
namespace XS = ue_wrap::explosion_source;
namespace A  = ue_wrap::atv;
namespace R  = ue_wrap::reflection;
using coop::atv_sync::AtvEntry;
using ue_wrap::FVector;

// The entry under `key`, when the index is this world's and its actor lives; null otherwise.
AtvEntry* LiveEntry(const std::wstring& key) {
    AtvEntry* e = coop::atv_sync::FindEntryMutable(key);
    if (!e || !coop::atv_sync::IndexCurrent() || !R::IsLiveByIndex(e->actor, e->idx)) return nullptr;
    return e;
}

bool g_dropSaid = false;   // a send the session refused, said once per session

}  // namespace

void detail::ClearAtvClient() { g_dropSaid = false; }

bool OnClientAtvBirth(void* source) {
    if (!A::IsAtv(source)) return false;
    if (!coop::atv_sync::OwnsTick(source)) {
        // "Not owned" means another copy owns it only while the guard's set is published; with it withdrawn (a tick's
        // early return, a world change in flight) this copy may be the owner, and the blast is dissolved unasked.
        if (!coop::atv_hit_guard::Active()) ++detail::MutCounters().atvBirthUnowned;
        return false;
    }
    coop::net::Session* s = detail::SessionPtr();
    if (!s) return true;
    const std::wstring key = A::GetKeyString(source);
    const AtvEntry* e = coop::atv_sync::FindEntry(key);
    // The story event's trap fell in this frame: this copy's trap, seen set at the last Tick, now reads clear. A trap
    // that does not read is no trap fired.
    bool trapNow = true;
    const bool trapFired = e && e->copyTrapSeen && A::ReadTrap(source, trapNow) && !trapNow;
    coop::net::AtvExplodeRequestPayload p{};
    coop::net::WireKeyFromString(key, p.key);
    p.trapFired = trapFired ? 1 : 0;
    if (!s->SendReliable(coop::net::ReliableKind::AtvExplodeRequest, &p, sizeof(p))) {
        ++detail::MutCounters().clientDropped;
        if (!g_dropSaid) {
            g_dropSaid = true;
            UE_LOGW("[EXPLOSION] an ATV's request was dropped unsent: the session took no reliable message");
        }
        return true;
    }
    ++detail::MutCounters().requestsSent[detail::KindSlot(RC::RequestKind::Atv)];
    UE_LOGI("[EXPLOSION] request sent kind=atv key=%ls trapFired=%u", key.c_str(), static_cast<unsigned>(p.trapFired));
    return true;
}

void OnAtvRequestReceived(int senderSlot, const uint8_t* bytes, uint16_t len) {
    if (!detail::ValidSenderSlot(senderSlot)) return;  // the dispatch's guard, kept at this edge
    const auto slot = static_cast<uint8_t>(senderSlot);
    if (!detail::SenderWorldReady(slot)) return detail::Refuse(slot, Refusal::NotWorldReady);
    coop::net::AtvExplodeRequestPayload p{};
    if (!bytes || len != sizeof(p)) return detail::Refuse(slot, Refusal::Malformed);
    std::memcpy(&p, bytes, sizeof(p));
    const std::wstring key = coop::net::StringFromWireKey(p.key);
    if (key.empty() || p.trapFired > 1 || p.pad[0] || p.pad[1] || p.pad[2])
        return detail::Refuse(slot, Refusal::Malformed);

    AtvEntry* e = LiveEntry(key);
    if (!e) return detail::Refuse(slot, Refusal::UnknownAtv);
    // MTA's client sends a vehicle's explosion only for a responsible it may report: the local player, the vehicle
    // the local player sits in (:153) or an unoccupied vehicle it syncs, and its server does not test it
    // (reference/mtasa-blue/Client/mods/deathmatch/logic/CClientExplosionManager.cpp:152-157); here the host judges
    // the syncer at its receive boundary. The syncer a hand-back just replaced is admitted inside AdmitBlock's late
    // window: its copy owned the tick when the ATV died, so its report is honest.
    if (e->syncerSlot != slot && !coop::atv_authority::IsLateSyncer(*e, slot, coop::atv_sync::NowMs()))
        return detail::Refuse(slot, Refusal::NotSyncer);
    // MTA blows a vehicle once (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2870-2904); here the
    // game's own re-arm applies: the mark clears when the slot that blew it reports health at 0 or above (atv_sync).
    if (e->blown) return detail::Refuse(slot, Refusal::Blown);
    if (!detail::SlotOf(slot).atvBlast.Push(detail::AtvItem{key}, detail::kAtvBlastCap))
        return detail::Refuse(slot, Refusal::QueueFull);
    if (e->syncerSlot != slot) ++detail::MutCounters().lateAccepted;
    // Marked only now the request is queued; a trap that fired on the client has fired for everyone, so the host's
    // own copy must not fire it again.
    e->blown = true;
    e->blownBy = slot;
    ++detail::MutCounters().atvBlownMarked;
    if (p.trapFired) A::WriteTrap(e->actor, false);
}

namespace detail {

coop::net::Served JudgeAtvBlast(uint8_t slot, const AtvItem& it) {
    AtvEntry* e = LiveEntry(it.key);
    if (!e) {
        if (AtvEntry* gone = coop::atv_sync::FindEntryMutable(it.key)) {
            gone->blown = false;
            gone->blownBy = 0xFF;
        }
        Refuse(slot, Refusal::UnknownAtv);
        return coop::net::Served::Done;
    }
    // The blast's place is the host copy's own `tp`, read here after the slot's queue and bucket. MTA reads its
    // vehicle's position synchronously at the packet
    // (reference/mtasa-blue/Server/mods/deathmatch/logic/CGame.cpp:2866-2867); a wreck is at rest by now, and the
    // host copy trails the syncer by the corrector's lag.
    FVector tp{};
    if (!A::ReadTpLocation(e->actor, tp)) {  // no place to put the blast: never one at the origin
        e->blown = false;
        e->blownBy = 0xFF;
        Refuse(slot, Refusal::NoTp);
        return coop::net::Served::Done;
    }
    const float loc[3] = {tp.X, tp.Y, tp.Z};
    if (!HostSpawn(slot, XS::Kind::None, loc, 0.0f)) {
        e->blown = false;  // no blast was made
        e->blownBy = 0xFF;
        return coop::net::Served::Done;
    }
    ++MutCounters().requestsAccepted[KindSlot(RC::RequestKind::Atv)];
    UE_LOGI("[EXPLOSION] request accepted kind=atv key=%ls slot=%u at (%.0f,%.0f,%.0f)", it.key.c_str(),
            static_cast<unsigned>(slot), loc[0], loc[1], loc[2]);
    return coop::net::Served::Done;
}

}  // namespace detail

}  // namespace coop::explosion_request

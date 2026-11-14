// coop/creatures/npc_ride.cpp -- see coop/creatures/npc_ride.h.

#include "coop/creatures/npc_ride.h"

#include "coop/element/mirror_defer.h"
#include "coop/element/mirror_managers.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine_attach.h"

#include <unordered_map>
#include <utility>

namespace coop::npc_ride {
namespace {

namespace E = ue_wrap::engine;
namespace K = ue_wrap::kerfur;
namespace R = ue_wrap::reflection;

struct Row {
    std::wstring key;            // the ATV the kerfur rides
    void* atvAttached = nullptr; // the ATV the sit used, set only once the kerfur's attach parent read as it: compared, index-checked
    int32_t atvIdx = -1;
    void* kerfur = nullptr;      // the actor the latch was taken on: compared, never dereferenced
    bool refusedSaid = false;    // "not a kerfur" was said for this eid
    bool noAtvSaid = false;      // a sit that did not attach was said for this key
};

std::unordered_map<uint32_t, Row> g_rows;
Resolver g_resolve = nullptr;

// The live mirror actor of `eid`, or null.
void* LiveMirrorActor(uint32_t eid) {
    coop::element::Npc* const npc = coop::element::NpcMirrors().Get(eid);
    void* const actor = npc ? npc->GetActor() : nullptr;
    return (actor && R::IsLiveByIndex(actor, npc->GetInternalIdx())) ? actor : nullptr;
}

// A mirror is unseated by its own state and never by its getOffCar: the client cancels that call, and its teleport and
// move(false) belong to the host's kerfur, not to a parked copy.
// The latched ATV when it is still live, else null.
void* LiveLatch(const Row& row) {
    return (row.atvAttached && R::IsLiveByIndex(row.atvAttached, row.atvIdx)) ? row.atvAttached : nullptr;
}

// What the latch says: seated on a live ATV, seated on an ATV that is gone, or never seated. `actor` is the live mirror, or
// null when it is gone too (a retired rider), where the ATV is told to forget the dead kerfur.
void Unseat(uint32_t eid, Row& row, const wchar_t* keyText, void* actor) {
    void* const live = LiveLatch(row);
    if (actor && row.atvAttached) {
        K::UnseatWithoutCar(actor);
        if (live) ue_wrap::atv::ForgetSittingKerfur(live, actor);
        UE_LOGI("npc_ride: eid %u off %ls (unseat without car)", eid, keyText);
    } else {
        if (!actor && live && row.kerfur) ue_wrap::atv::ForgetSittingKerfur(live, row.kerfur);
        UE_LOGI("npc_ride: eid %u off %ls (already off)", eid, keyText);
    }
    row.atvAttached = nullptr;
    row.atvIdx = -1;
    row.kerfur = nullptr;
}

// MTA keeps the occupant on the vehicle and the ped, and warps the ped back in when the ped's model is created
// (CClientVehicle.cpp:2735, :2746); a client never decides a remote ped's vehicle entry, the server's in/out packet does.
// Here the host's stamp is that packet: a stored row is applied at the mirror's ready edges.
void Reconcile(uint32_t eid, Row& row) {
    void* const actor = LiveMirrorActor(eid);
    if (!actor) {
        UE_LOGI("npc_ride: eid %u held (unbound)", eid);
        return;
    }
    if (!K::IsKerfurActor(actor)) {
        if (!row.refusedSaid) {
            row.refusedSaid = true;
            UE_LOGW("npc_ride: eid %u refused: not a kerfur (%ls)", eid, R::ClassNameOf(actor).c_str());
        }
        return;
    }
    coop::mirror_defer::HideInfo hide{};
    if (coop::mirror_defer::HideInfoOf(eid, hide)) {
        UE_LOGI("npc_ride: eid %u held (hidden)", eid);
        return;
    }
    int32_t atvIdx = -1;
    void* const atv = g_resolve ? g_resolve(row.key, atvIdx) : nullptr;
    if (!atv) {
        UE_LOGI("npc_ride: eid %u held (no atv)", eid);
        return;
    }
    void* parent = nullptr;
    // The parent is read here, at the event, never per pose: `occupyCar` is no ride, the game's own sit sets it on every kerfur.
    const bool read = E::GetAttachParentActor(actor, parent);
    if (read && parent == atv) {
        row.atvAttached = atv;
        row.atvIdx = atvIdx;
        row.kerfur = actor;
        return;
    }
    if (read && parent && ue_wrap::atv::IsAtv(parent)) {
        // Attached to another ATV: that seat ends first.
        row.atvAttached = parent;
        row.atvIdx = R::InternalIndexOf(parent);
        row.kerfur = actor;
        Unseat(eid, row, L"the previous ATV", actor);
    }
    if (K::SitOnCar(actor, atv) && E::GetAttachParentActor(actor, parent) && parent == atv) {
        row.atvAttached = atv;
        row.atvIdx = atvIdx;
        row.kerfur = actor;
        UE_LOGI("npc_ride: eid %u rides %ls (sitOnCar)", eid, row.key.c_str());
        return;
    }
    row.atvAttached = nullptr;
    if (!row.noAtvSaid) {
        row.noAtvSaid = true;
        UE_LOGW("npc_ride: eid %u held (no atv): its sitOnCar on %ls did not attach it", eid, row.key.c_str());
    }
}

}  // namespace

void SetResolver(Resolver fn) { g_resolve = fn; }

void Set(uint32_t eid, const std::wstring& key) {
    UE_ASSERT_GAME_THREAD("npc_ride::Set");
    if (eid == 0u) return;
    Row& row = g_rows[eid];
    if (row.key != key) {
        row.key = key;
        row.atvAttached = nullptr;
        row.noAtvSaid = false;
    }
    Reconcile(eid, row);
}

void Clear(uint32_t eid, const std::wstring& key) {
    UE_ASSERT_GAME_THREAD("npc_ride::Clear");
    const auto it = g_rows.find(eid);
    if (it == g_rows.end() || it->second.key != key) return;
    Row row = std::move(it->second);
    g_rows.erase(it);
    Unseat(eid, row, key.c_str(), LiveMirrorActor(eid));
}

void OnMirrorBound(uint32_t eid) {
    UE_ASSERT_GAME_THREAD("npc_ride::OnMirrorBound");
    const auto it = g_rows.find(eid);
    if (it == g_rows.end()) return;
    it->second.atvAttached = nullptr;  // a new actor is not attached
    it->second.kerfur = nullptr;
    Reconcile(eid, it->second);
}

void OnRevealed(uint32_t eid) {
    UE_ASSERT_GAME_THREAD("npc_ride::OnRevealed");
    const auto it = g_rows.find(eid);
    if (it == g_rows.end()) return;
    Reconcile(eid, it->second);
}

bool HoldsPose(uint32_t eid) {
    if (g_rows.empty()) return false;
    const auto it = g_rows.find(eid);
    return it != g_rows.end() && LiveLatch(it->second) != nullptr;
}

void OnDisconnect() { g_rows.clear(); }

}  // namespace coop::npc_ride

// coop/interactables/atv_rider_watch.cpp -- see coop/interactables/atv_rider_watch.h.

#include "coop/interactables/atv_rider_watch.h"

#include "coop/element/mirror_managers.h"
#include "coop/element/registry.h"
#include "coop/dev/atv_riders_arm.h"
#include "coop/interactables/atv_drill_events.h"
#include "coop/interactables/atv_seat_table.h"
#include "coop/interactables/atv_sync_internal.h"
#include "coop/net/session.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace coop::atv_rider_watch {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace DE = coop::atv_drill_events;

UE_FACT_FN(kRowSitOnCar, "atv_rider_watch", L"kerfurOmega_C", L"sitOnCar", Declared);
UE_FACT_FN(kRowGetOffCar, "atv_rider_watch", L"kerfurOmega_C", L"getOffCar", Declared);
UE_FACT_FN(kRowTryToOccupyCar, "atv_rider_watch", L"kerfurOmega_C", L"tryToOccupyCar", Declared);

constexpr int kTagSitOnCar = 0x41525701;   // 'ARW' 1
constexpr int kTagGetOffCar = 0x41525702;
constexpr int kTagTryToOccupyCar = 0x41525703;

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_saidNoEid{false};
std::atomic<bool> g_saidGetOff{false};
std::atomic<bool> g_saidOccupy{false};
std::atomic<uint32_t> g_getOffRefused{0};
std::atomic<uint32_t> g_occupyRefused{0};

bool IsHost() {
    const coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
}

bool IsClient() {
    const coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Client;
}

// A kerfur this client holds as the host's mirror: its element id is an NPC mirror's.
bool IsNpcMirror(void* kerfur) {
    const uint32_t eid = coop::element::Registry::Get().EidForActor(kerfur);
    return eid != coop::element::kInvalidId && coop::element::NpcMirrors().Get(eid) != nullptr;
}

// A client's own game acting on a kerfur the host owns is refused, and said once per session naming the Blueprint
// function that made the call (the ATV's sit, dismount or crash path).
sg::Verdict RefuseOnMirror(const sg::Call& c, const char* what, int kind, std::atomic<uint32_t>& count,
                           std::atomic<bool>& said) {
    count.fetch_add(1, std::memory_order_relaxed);
    if (DE::On()) DE::Note(DE::kMirrorCancel, kind, static_cast<int32_t>(coop::element::Registry::Get().EidForActor(c.object)));
    if (!said.exchange(true)) {
        const std::wstring caller =
            c.callerFunction ? R::ToString(R::NameOf(c.callerFunction)) : std::wstring(L"ProcessEvent");
        UE_LOGI("atv_rider_watch: this client's own %s on the host's kerfur mirror %p refused (called from %ls) -- the "
                "mirror's seat is the host's", what, c.object, caller.c_str());
    }
    return sg::Verdict::Cancel;
}

// POST of sitOnCar: the kerfur's own sit, run by its Blueprint after its walk to the ATV. The body also returns through its
// failCar branch, so the kerfur counts as seated only when its attach parent is the ATV its occupyCar names.
void OnSitOnCarPost(const sg::Call& c) {
    if (!IsHost()) return;
    void* const car = ue_wrap::kerfur::GetOccupyCar(c.object);
    if (!car) return;
    void* parent = nullptr;
    if (!ue_wrap::engine::GetAttachParentActor(c.object, parent) || parent != car) return;
    coop::atv_sync::AtvEntry* const e = coop::atv_sync::FindEntryByActor(car);
    if (!e) return;
    const uint32_t eid = coop::element::Registry::Get().EidForActor(c.object);
    if (eid == coop::element::kInvalidId) {
        if (!g_saidNoEid.exchange(true))
            UE_LOGW("atv_rider_watch: a '%ls' sat on ATV %p and has no element id -- no rider is named (said once per session)",
                    R::ClassNameOf(c.object).c_str(), car);
        return;
    }
    if (DE::On()) DE::Note(DE::kRiderFact, DE::KeyTag(e->key), static_cast<int32_t>(eid), 1);
    coop::atv_seat_table::SetRider(*e, eid, c.object, "sitOnCar");
}

// PRE of getOffCar: the body nulls occupyCar before it returns, so the kerfur is identified by its element id.
sg::Verdict OnGetOffCarPre(const sg::Call& c) {
    // A client's own dismount and crash paths never unseat or kill a mirror: its seat ends by the host's stamp, which
    // coop/creatures/npc_ride answers. Cancelling skips the kill tail with the rest.
    // The riders drill's red `getoff_cancel_off`: the call runs on a mirror.
    static const bool s_redGetOff = coop::dev::atv_riders_arm::Is("getoff_cancel_off");
    if (IsClient()) {
        return IsNpcMirror(c.object) && !s_redGetOff ? RefuseOnMirror(c, "getOffCar", 0, g_getOffRefused, g_saidGetOff)
                                                     : sg::Verdict::Run;
    }
    if (!IsHost()) return sg::Verdict::Run;
    const uint32_t eid = coop::element::Registry::Get().EidForActor(c.object);
    if (eid == coop::element::kInvalidId) return sg::Verdict::Run;
    if (coop::atv_sync::AtvEntry* const e = coop::atv_sync::FindEntryByRider(eid)) {
        if (DE::On()) DE::Note(DE::kRiderFact, DE::KeyTag(e->key), 0, 0);
        coop::atv_seat_table::SetRider(*e, 0, nullptr, "getOffCar");
    }
    return sg::Verdict::Run;
}

// PRE of tryToOccupyCar: the client's own ATV sit invites every kerfur of its world, its mirrors included, which would
// start a local walk on a parked mirror. The host's kerfurs are the host's to invite.
sg::Verdict OnTryToOccupyCarPre(const sg::Call& c) {
    // The riders drill's red `mirror_invite_off`: the call runs on a mirror.
    static const bool s_redOccupy = coop::dev::atv_riders_arm::Is("mirror_invite_off");
    if (!IsClient() || !IsNpcMirror(c.object) || s_redOccupy) return sg::Verdict::Run;
    return RefuseOnMirror(c, "tryToOccupyCar", 1, g_occupyRefused, g_saidOccupy);
}

// One watch: registered with the gate, then asked each call until it settles live or dead.
struct Watch {
    const ue_wrap::facts::Function& row;
    const char*                     label;
    int                             tag;
    sg::PreFn                       pre;
    sg::PostFn                      post;
    bool                            registered;
    bool                            settled;
};

Watch g_sitOnCar{kRowSitOnCar, "kerfurOmega_C.sitOnCar POST", kTagSitOnCar, nullptr, &OnSitOnCarPost, false, false};
Watch g_getOffCar{kRowGetOffCar, "kerfurOmega_C.getOffCar PRE", kTagGetOffCar, &OnGetOffCarPre, nullptr, false, false};
Watch g_tryToOccupyCar{kRowTryToOccupyCar, "kerfurOmega_C.tryToOccupyCar PRE", kTagTryToOccupyCar, &OnTryToOccupyCarPre,
                       nullptr, false, false};

// The shape of kerfur_seat_probe's InstallWatch: a refused registration is asked again while the gate is not armed; a watch
// whose name resolved dead is said once, with the reason.
void InstallWatch(Watch& w) {
    if (w.settled) return;
    if (!w.registered) w.registered = sg::WatchName(w.row, w.tag, w.pre, w.post);
    if (!w.registered) {
        if (sg::IsInstalled()) {
            w.settled = true;
            UE_LOGE("atv_rider_watch: watch '%s' NOT live: the gate refused the registration", w.label);
        }
        return;
    }
    sg::ResolvePendingNames();
    if (sg::NameWatchLive(w.row, w.tag)) {
        w.settled = true;
        UE_LOGI("atv_rider_watch: watch '%s' live", w.label);
    } else if (sg::NameWatchSettled(w.row, w.tag)) {
        w.settled = true;
        UE_LOGE("atv_rider_watch: watch '%s' NOT live: its name resolved dead (the class declares no such function, a "
                "native one, or the table is full)", w.label);
    }
}

}  // namespace

void SeedRider(coop::atv_sync::AtvEntry& e) {
    if (!IsHost() || !e.actor) return;
    void* const kerfur = ue_wrap::atv::GetSittingKerfur(e.actor);
    if (!kerfur) return;
    void* parent = nullptr;
    if (!ue_wrap::engine::GetAttachParentActor(kerfur, parent) || parent != e.actor) return;
    const uint32_t eid = coop::element::Registry::Get().EidForActor(kerfur);
    if (eid == coop::element::kInvalidId) return;
    coop::atv_seat_table::SetRider(e, eid, kerfur, "seeded");
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    InstallWatch(g_sitOnCar);
    InstallWatch(g_getOffCar);
    InstallWatch(g_tryToOccupyCar);
}

void OnDisconnect() {
    const uint32_t getOff = g_getOffRefused.exchange(0), occupy = g_occupyRefused.exchange(0);
    if (getOff || occupy)
        UE_LOGI("atv_rider_watch: session end -- this client's own calls on the host's kerfur mirrors refused: getOffCar=%u "
                "tryToOccupyCar=%u", getOff, occupy);
    g_session.store(nullptr, std::memory_order_release);   // the next Install names the next session
    g_saidNoEid.store(false, std::memory_order_relaxed);
    g_saidGetOff.store(false, std::memory_order_relaxed);
    g_saidOccupy.store(false, std::memory_order_relaxed);
}

}  // namespace coop::atv_rider_watch

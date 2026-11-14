// coop/interactables/atv_rider_watch.cpp -- see coop/interactables/atv_rider_watch.h.

#include "coop/interactables/atv_rider_watch.h"

#include "coop/element/registry.h"
#include "coop/interactables/atv_seat_table.h"
#include "coop/interactables/atv_sync_internal.h"
#include "coop/net/session.h"

#include "ue_wrap/actors/kerfur.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>

namespace coop::atv_rider_watch {
namespace {

namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

UE_FACT_FN(kRowSitOnCar, "atv_rider_watch", L"kerfurOmega_C", L"sitOnCar", Declared);
UE_FACT_FN(kRowGetOffCar, "atv_rider_watch", L"kerfurOmega_C", L"getOffCar", Declared);

constexpr int kTagSitOnCar = 0x41525701;   // 'ARW' 1
constexpr int kTagGetOffCar = 0x41525702;

std::atomic<coop::net::Session*> g_session{nullptr};
std::atomic<bool> g_saidNoEid{false};

bool IsHost() {
    const coop::net::Session* s = g_session.load(std::memory_order_acquire);
    return s && s->role() == coop::net::Role::Host;
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
    coop::atv_seat_table::SetRider(*e, eid, c.object, "sitOnCar");
}

// PRE of getOffCar: the body nulls occupyCar before it returns, so the kerfur is identified by its element id.
sg::Verdict OnGetOffCarPre(const sg::Call& c) {
    if (!IsHost()) return sg::Verdict::Run;
    const uint32_t eid = coop::element::Registry::Get().EidForActor(c.object);
    if (eid == coop::element::kInvalidId) return sg::Verdict::Run;
    if (coop::atv_sync::AtvEntry* const e = coop::atv_sync::FindEntryByRider(eid))
        coop::atv_seat_table::SetRider(*e, 0, nullptr, "getOffCar");
    return sg::Verdict::Run;
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

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    InstallWatch(g_sitOnCar);
    InstallWatch(g_getOffCar);
}

void OnDisconnect() { g_saidNoEid.store(false, std::memory_order_relaxed); }

}  // namespace coop::atv_rider_watch

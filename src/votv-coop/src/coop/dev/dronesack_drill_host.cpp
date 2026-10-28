// coop/dev/dronesack_drill_host.cpp -- see coop/dev/dronesack_drill.h: the host's half. It puts the sack on the floor
// in front of itself and waits for the client to bind its mirror of it. Arm 1 then drops a crate onto the sack and
// for kWatchMs logs what destroyed the crate. Arm 2 spawns loose props round the sack, lets the client carry them
// into it, calls the delivery drone and waits for its sale to clear the drone's container.

#include "coffee_drill_detail.h"     // co-located private header: SpawnProp (a prop_C named by a row)
#include "dronesack_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/dev/drill_props.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/container_contents_sync.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/drone.h"
#include "ue_wrap/devices/drone_console.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace coop::dev::dronesack_drill::detail {
namespace {

namespace CD = coop::dev::coffee_drill::detail;
namespace DP = coop::dev::drill_props;
namespace DR = ue_wrap::drone;
namespace DK = ue_wrap::drone_console;
namespace E  = ue_wrap::engine;
namespace EL = coop::element;
namespace R  = ue_wrap::reflection;

constexpr uint64_t kPlaceEveryMs = 1000;   // a floor that was not found is looked for again this often
const wchar_t* const kSackPath = L"/Game/objects/prop_dronesack.prop_dronesack_C";
// Arm 2's loose props are what the coffee drill spawns for a cup: a prop_C whose `name` row is "cup".
const wchar_t* const kPropClass = L"prop_C";
const wchar_t* const kPropName = L"cup";

enum class Ph : uint8_t {
    Wait, Place, SackEid, AwaitBound,
    CrateEid, Watch,                                  // arm 1
    PlaceProps, PropEids, AwaitPuts, Settle, Call, Sale, Tail,   // arm 2 (Call, Sale and Tail are arm 3's too)
    Fill,                                             // arm 3
    AwaitTake,                                        // arm 4
    Done
};

struct Host {
    Ph                    ph = Ph::Wait;
    uint64_t              phaseMs = 0, nextPlaceMs = 0, dropMs = 0, callMs = 0, soldMs = 0;
    uint32_t              sackEid = 0, crateEid = 0;
    int                   recordsAtDrop = -1, recordsAtCall = -1;
    ue_wrap::FVector      sackAt{};
    ue_wrap::CachedObjRef sack, crate;
    std::vector<ue_wrap::CachedObjRef> props;
    std::vector<uint32_t>              propEids;
    // arm 3: the items dropped one at a time, cups first and the crate last
    int                   item = 0, absorbed = 0, recordsBeforeItem = 0, crateRecords = -1;
    int32_t               crateIndex = -1;
    uint64_t              itemMs = 0, takeHeardMs = 0;
    bool                  spawned = false, read = false, eidSeen = false;
    ue_wrap::CachedObjRef cur;
    std::vector<uint32_t> watchEids;
};
Host g_h;

void Abandon(const char* why) {
    UE_LOGW("[SACK-DRILL] ABANDONED on the host: %s", why);
    g_h.ph = Ph::Done;
}

void Go(Ph ph) {
    g_h.ph = ph;
    g_h.phaseMs = NowMs();
}

EL::ElementId EidOf(void* actor) { return EL::Registry::Get().EidForActor(actor); }

std::string Narrow(const std::wstring& w) {
    std::string out;
    for (const wchar_t c : w) out.push_back(c > 0 && c < 128 ? static_cast<char>(c) : '?');
    return out;
}

// One try at putting the sack on the floor kSackAheadCm in front of the host. True once it stands; a floor that
// was not found is tried again at the next due tick.
bool PlaceSack(void* me) {
    void* cls = R::FindClass(L"prop_dronesack_C");
    if (!cls) cls = ue_wrap::asset_load::LoadObjectByPath(kSackPath);
    if (!cls) { Abandon("the drone sack class does not resolve"); return false; }
    ue_wrap::FVector at{};
    if (!E::TryGetActorLocation(me, at)) return false;
    const ue_wrap::FVector fwd = E::GetActorForwardVector(me);
    const float len = std::sqrt(fwd.X * fwd.X + fwd.Y * fwd.Y);
    if (len < 0.1f) return false;
    const float x = at.X + fwd.X / len * kSackAheadCm, y = at.Y + fwd.Y / len * kSackAheadCm;
    void* sack = nullptr;
    float floorZ = 0.f;
    const DP::Drop d = DP::DropOnFloor(me, cls, nullptr, x, y, at.Z, kFloorDownCm, kMinColumnCm, kRiseCm, &sack, &floorZ);
    if (d == DP::Drop::NoFloor) return false;
    if (d == DP::Drop::SpawnFailed) { Abandon("the drone sack did not spawn"); return false; }
    g_h.sack.Set(sack);
    return true;
}

// The destroys the hooks recorded, and the client PropDestroys that arrived, each said once; times are +ms from the
// drop (arm 1) or from the props' spawn (arm 2).
void PrintNew() {
    for (Destroyed& d : Destroys())
        if (!d.printed) PrintDestroy("host", d, g_h.crateEid, g_h.dropMs, false);
    for (Received& r : Recv()) {
        if (r.printed) continue;
        r.printed = true;
        UE_LOGI("[SACK-DRILL] host received PropDestroy eid=%u from slot %u at +%llums (the %s still alive here: %d)",
                r.eid, static_cast<unsigned>(r.slot),
                static_cast<unsigned long long>(r.ms > g_h.dropMs ? r.ms - g_h.dropMs : 0),
                r.eid == g_h.crateEid ? "crate" : "prop", r.alive ? 1 : 0);
    }
}

// ---- arm 1: the crate ----

// kWatchMs after the drop: the crate's fate, as this peer's two seams told it.
void Finish() {
    const Received* rc = nullptr;
    for (const Received& r : Recv())
        if (r.eid == g_h.crateEid) { rc = &r; break; }
    const Destroyed* local = nullptr;
    for (const Destroyed& d : Destroys())
        if (d.eid == g_h.crateEid) { local = &d; break; }
    const bool alive = EL::LivePropActor(g_h.crateEid) != nullptr;
    const char* by = "none";
    if (local && !rc) by = "host-absorb";
    else if (rc && (!local || local->ms >= rc->ms)) by = "client-relay";
    else if (local && rc) by = "both";
    UE_LOGI("[SACK-DRILL] host sack records at drop=%d, at end=%d", g_h.recordsAtDrop, SackRecords(g_h.sack.Get()));
    Say(kDoneLine);
    UE_LOGI("[SACK-DRILL] host DONE crate=%s by=%s", alive ? "alive" : "gone", by);
    g_h.ph = Ph::Done;
}

void Watch() {
    PrintNew();
    if (NowMs() - g_h.dropMs >= kWatchMs) Finish();
}

// ---- arm 2: the loose props and the sale ----

// Up to kPropCount props on a ring round the sack, each where the floor is found. False when none stands.
bool PlaceProps(void* me) {
    for (int k = 0; k < kPropSpots && static_cast<int>(g_h.props.size()) < kPropCount; ++k) {
        const float a = static_cast<float>(k) * (6.2831853f / static_cast<float>(kPropSpots));
        const float x = g_h.sackAt.X + std::cos(a) * kPropRingCm, y = g_h.sackAt.Y + std::sin(a) * kPropRingCm;
        float floorZ = 0.f;
        if (!DP::FloorAt(me, x, y, g_h.sackAt.Z + 100.f, kFloorDownCm, kMinColumnCm, &floorZ)) continue;
        void* const p = CD::SpawnProp(kPropClass, {x, y, floorZ + kRiseCm}, kPropName);
        if (!p) continue;
        ue_wrap::CachedObjRef ref;
        ref.Set(p);
        g_h.props.push_back(ref);
    }
    UE_LOGI("[SACK-DRILL] host placed %zu of %d loose props round the sack", g_h.props.size(), kPropCount);
    return !g_h.props.empty();
}

int PropsOutside() {
    int n = 0;
    for (const uint32_t eid : g_h.propEids) n += EL::LivePropActor(eid) != nullptr ? 1 : 0;
    return n;
}

std::string OutsideList() {
    std::string out;
    for (const uint32_t eid : g_h.propEids)
        if (EL::LivePropActor(eid)) out += (out.empty() ? "" : ",") + std::to_string(eid);
    return out.empty() ? "none" : out;
}

void SaleDone() {
    if (Mode() >= 3) {
        UE_LOGI("[SACK-DRILL] host DONE arm %d items absorbed=%d of %d, drone container records at call=%d, now=%d",
                Mode(), g_h.absorbed, kPropCount + 1, g_h.recordsAtCall, SackRecords(g_h.sack.Get()));
        Say(kDoneLine);
        g_h.ph = Ph::Done;
        return;
    }
    UE_LOGI("[SACK-DRILL] host DONE arm 2 props in sack=%d of %zu, drone container records at call=%d, now=%d",
            static_cast<int>(g_h.propEids.size()) - PropsOutside(), g_h.propEids.size(), g_h.recordsAtCall,
            SackRecords(g_h.sack.Get()));
    Say(kDoneLine);
    g_h.ph = Ph::Done;
}

// The drone is called as the console's own keyboard calls it, once it is parked.
void CallDrone() {
    void* consoles[1] = {};
    void* const drone = DR::Find();
    if (DK::LiveConsoles(consoles, 1) <= 0 || !drone) { Abandon("this world holds no drone console or no drone"); return; }
    if (DR::IsActive(drone)) {
        if (NowMs() - g_h.phaseMs > kParkBoundMs) Abandon("the drone did not park");
        return;
    }
    g_h.recordsAtCall = SackRecords(g_h.sack.Get());
    UE_LOGI("[SACK-DRILL] host calls the drone: hasSack=%d, drone container records=%d",
            (DR::ReadFxBits(drone) & DR::kFxHasSack) ? 1 : 0, g_h.recordsAtCall);
    if (Mode() >= 3) UE_LOGI("[SACK-DRILL] host crate inventory index=%d before the sale (read when it was dropped)",
                             g_h.crateIndex);
    CensusTake("host", Moment::Before);
    Say(kCallLine);
    if (!DK::TriggerFly(consoles[0])) { Abandon("the drone's call did not dispatch"); return; }
    g_h.callMs = NowMs();
    Go(Ph::Sale);
}

void Sale() {
    PrintNew();
    const int records = SackRecords(g_h.sack.Get());
    if (g_h.recordsAtCall <= 0) {
        UE_LOGI("[SACK-DRILL] host: the drone container held nothing at the call, so there is no sale to see");
        SaleDone();
        return;
    }
    if (records == 0) {
        UE_LOGI("[SACK-DRILL] host sale done at +%llums",
                static_cast<unsigned long long>(NowMs() - g_h.callMs));
        CensusTake("host", Moment::After);
        if (Mode() >= 3) CensusIndexHolders("host", Moment::After, g_h.crateIndex);
        Say(kSoldLine);
        g_h.soldMs = NowMs();
        Go(Ph::Tail);
        return;
    }
    if (NowMs() - g_h.callMs > kSaleBoundMs) Abandon("the drone container was not cleared");
}

// The census is read again kCensusLaterMs after the sale, and the two reads are compared.
void Tail() {
    PrintNew();
    if (NowMs() - g_h.soldMs < kCensusLaterMs) return;
    CensusTake("host", Moment::AfterLater);
    if (Mode() >= 3) CensusIndexHolders("host", Moment::AfterLater, g_h.crateIndex);
    CensusDiff("host", g_h.sack.Get());
    if (Mode() == 4) {   // the client takes from the sold sack at +5 s, if its copy still holds records: wait for what it says
        void* const container = ContainerOf(g_h.sack.Get());
        UE_LOGI("[SACK-DRILL] host drone container in sync scope=%d", ContainerInSyncScope(container) ? 1 : 0);
        Go(Ph::AwaitTake);
        return;
    }
    SaleDone();
}

// Arm 4: what reached the host about the client's take, from the container lane's own judgement of a client slice.
void AwaitTake() {
    PrintNew();
    const uint64_t now = NowMs();
    std::string rest;
    if (!Rest(kTookPrefix, &rest)) {
        if (now - g_h.phaseMs > kTakeBoundMs) Abandon("the client never said whether it took from the sold sack");
        return;
    }
    const bool took = std::atoi(rest.c_str()) != 0;
    void* const container = ContainerOf(g_h.sack.Get());
    const EL::ElementId eid = container ? EidOf(container) : EL::kInvalidId;
    namespace CC = coop::props::container_contents_sync;
    const CC::ClientSliceOutcome out = eid == EL::kInvalidId ? CC::ClientSliceOutcome::None
                                                             : CC::LastClientSliceOutcome(static_cast<uint32_t>(eid));
    // A take that was made is given kTakeSettleMs for the slice to arrive and be judged; "none" is an answer.
    if (took && out == CC::ClientSliceOutcome::None) {
        if (g_h.takeHeardMs == 0) g_h.takeHeardMs = now;
        if (now - g_h.takeHeardMs < kTakeSettleMs) return;
    }
    const char* seen = out == CC::ClientSliceOutcome::Accepted ? "accepted"
                     : out == CC::ClientSliceOutcome::Refused  ? "refused" : "none";
    UE_LOGI("[SACK-DRILL] host: the client took=%d; the drone container is eid=%u", took ? 1 : 0,
            eid == EL::kInvalidId ? 0u : static_cast<unsigned>(eid));
    UE_LOGI("[SACK-DRILL] host DONE arm 4 drone container records=%d; client take seen=%s",
            SackRecords(g_h.sack.Get()), seen);
    Say(kDoneLine);
    g_h.ph = Ph::Done;
}

// ---- arm 3: the host drops each item itself ----

// Registers a dropped item's element id with the hooks, once it has one.
void NoteItemEid(void* p, bool crate) {
    const EL::ElementId eid = EidOf(p);
    if (eid == EL::kInvalidId) return;
    g_h.eidSeen = true;
    g_h.watchEids.push_back(static_cast<uint32_t>(eid));
    SetWatch(g_h.watchEids);
    if (!crate) return;
    g_h.crateEid = static_cast<uint32_t>(eid);
    SetCrate(g_h.crateEid, Narrow(ue_wrap::prop::GetInteractableKeyString(p)));
    UE_LOGI("[SACK-DRILL] host crate eid=%u", g_h.crateEid);
}

// One item at a time from kCrateAboveCm over the sack: kPropCount cups, then one fresh crate. Each is waited for until the
// drone container's record count rises or the item is destroyed, for at most kFillItemBoundMs.
void Fill() {
    PrintNew();
    const uint64_t now = NowMs();
    if (g_h.item > kPropCount) {   // every item has been tried
        if (g_h.crateIndex >= 0) Say(std::string(kIndexPrefix) + std::to_string(g_h.crateIndex));
        UE_LOGI("[SACK-DRILL] host filled: %d of %d items absorbed; drone container records=%d", g_h.absorbed,
                kPropCount + 1, SackRecords(g_h.sack.Get()));
        Go(Ph::Call);
        return;
    }
    const bool crate = g_h.item == kPropCount;
    if (!g_h.spawned) {
        ue_wrap::FVector at = g_h.sackAt;
        at.Z += kCrateAboveCm;
        void* cls = crate ? R::FindClass(L"prop_container_C") : nullptr;
        void* const p = crate ? (cls ? E::SpawnActor(cls, at) : nullptr) : CD::SpawnProp(kPropClass, at, kPropName);
        if (!p) { Abandon("an item did not spawn"); return; }
        g_h.cur.Set(p);
        g_h.recordsBeforeItem = SackRecords(g_h.sack.Get());
        g_h.itemMs = now;
        g_h.spawned = true;
        g_h.read = g_h.eidSeen = false;
        return;
    }
    void* const p = g_h.cur.Get();
    if (p && !g_h.read) {
        g_h.read = true;
        if (crate) {
            g_h.crateRecords = RecordsOf(p);
            g_h.crateIndex = ContainerIndex(p);
            UE_LOGI("[SACK-DRILL] host crate records=%d inventory index=%d dropped", g_h.crateRecords, g_h.crateIndex);
        }
    }
    if (p && !g_h.eidSeen) NoteItemEid(p, crate);
    const int records = SackRecords(g_h.sack.Get());
    const char* const what = crate ? "crate" : "cup";
    const unsigned long long at = now - g_h.itemMs;
    if (!p || records > g_h.recordsBeforeItem) {
        ++g_h.absorbed;
        UE_LOGI("[SACK-DRILL] host %s %d of %d absorbed at +%llums (drone container records %d -> %d, the actor %s)",
                what, g_h.item + 1, kPropCount + 1, at, g_h.recordsBeforeItem, records, p ? "lives" : "is destroyed");
    } else if (now - g_h.itemMs > kFillItemBoundMs) {
        UE_LOGI("[SACK-DRILL] host %s %d of %d MISSED: not absorbed within %llums (drone container records %d -> %d)",
                what, g_h.item + 1, kPropCount + 1, at, g_h.recordsBeforeItem, records);
    } else {
        return;
    }
    ++g_h.item;
    g_h.spawned = false;
    g_h.cur.Reset();
}

}  // namespace

void HostReset() {
    g_h = Host{};
    CensusReset();
}

void HostTick(coop::net::Session* s) {
    const uint64_t now = NowMs();
    void* const me = coop::players::Registry::Get().Local();
    switch (g_h.ph) {
    case Ph::Wait:
        if (!me || !s->AnyWorldReadyPeer()) return;
        Go(Ph::Place);
        return;
    case Ph::Place:
        if (!me || now < g_h.nextPlaceMs) return;
        g_h.nextPlaceMs = now + kPlaceEveryMs;
        if (PlaceSack(me)) { Go(Ph::SackEid); return; }
        if (g_h.ph == Ph::Place && now - g_h.phaseMs > kPlaceBoundMs) Abandon("no floor was found in front of the host");
        return;
    case Ph::SackEid: {
        void* const sack = g_h.sack.Get();
        if (!sack) { Abandon("the sack died before it took an element id"); return; }
        const EL::ElementId eid = EidOf(sack);
        if (eid == EL::kInvalidId) {
            if (now - g_h.phaseMs > kEidBoundMs) Abandon("the sack took no element id");
            return;
        }
        if (!E::TryGetActorLocation(sack, g_h.sackAt)) { Abandon("the sack's place is unread"); return; }
        if (!Arm(true)) { Abandon("the container or prop class does not resolve"); return; }
        g_h.sackEid = static_cast<uint32_t>(eid);
        UE_LOGI("[SACK-DRILL] host READY sack eid=%u at (%.0f,%.0f,%.0f)", g_h.sackEid, g_h.sackAt.X, g_h.sackAt.Y,
                g_h.sackAt.Z);
        Say(std::string(kSackPrefix) + std::to_string(g_h.sackEid));
        Go(Ph::AwaitBound);
        return;
    }
    case Ph::AwaitBound: {
        std::string rest;
        if (Rest(kBoundPrefix, &rest) && std::strtoul(rest.c_str(), nullptr, 10) == g_h.sackEid) {
            if (Mode() == 2) { Go(Ph::PlaceProps); return; }
            if (Mode() >= 3) { g_h.dropMs = now; Go(Ph::Fill); return; }
            void* const cls = R::FindClass(L"prop_container_C");
            ue_wrap::FVector at = g_h.sackAt;
            at.Z += kCrateAboveCm;
            void* const crate = cls ? E::SpawnActor(cls, at) : nullptr;
            if (!crate) { Abandon("the crate did not spawn"); return; }
            g_h.crate.Set(crate);
            g_h.dropMs = now;
            Go(Ph::CrateEid);
            return;
        }
        if (now - g_h.phaseMs > kBoundBoundMs) Abandon("the client never said its sack mirror is bound");
        return;
    }
    case Ph::CrateEid: {
        void* const crate = g_h.crate.Get();
        if (!crate) { Abandon("the crate was gone before its element id was read"); return; }
        const EL::ElementId eid = EidOf(crate);
        if (eid == EL::kInvalidId) {
            if (now - g_h.phaseMs > kEidBoundMs) Abandon("the crate took no element id");
            return;
        }
        g_h.crateEid = static_cast<uint32_t>(eid);
        SetCrate(g_h.crateEid, Narrow(ue_wrap::prop::GetInteractableKeyString(crate)));
        g_h.recordsAtDrop = SackRecords(g_h.sack.Get());
        UE_LOGI("[SACK-DRILL] host crate eid=%u records=%d dropped", g_h.crateEid, RecordsOf(crate));
        Say(std::string(kCratePrefix) + std::to_string(g_h.crateEid));
        Go(Ph::Watch);
        return;
    }
    case Ph::Watch:
        Watch();
        return;
    case Ph::PlaceProps:
        if (!me) return;
        if (!PlaceProps(me)) { Abandon("no loose prop could be placed round the sack"); return; }
        g_h.dropMs = now;
        Go(Ph::PropEids);
        return;
    case Ph::PropEids: {
        std::vector<uint32_t> eids;
        for (ue_wrap::CachedObjRef& ref : g_h.props) {
            void* const p = ref.Get();
            const EL::ElementId eid = p ? EidOf(p) : EL::kInvalidId;
            if (eid == EL::kInvalidId) break;
            eids.push_back(static_cast<uint32_t>(eid));
        }
        if (eids.size() < g_h.props.size()) {
            if (now - g_h.phaseMs > kEidBoundMs) Abandon("a loose prop took no element id");
            return;
        }
        g_h.propEids = eids;
        SetWatch(eids);
        std::string csv;
        for (const uint32_t e : eids) csv += (csv.empty() ? "" : ",") + std::to_string(e);
        g_h.recordsAtDrop = SackRecords(g_h.sack.Get());
        UE_LOGI("[SACK-DRILL] host READY %zu loose props (%ls '%ls') eids=%s; drone container records=%d", eids.size(),
                kPropClass, kPropName, csv.c_str(), g_h.recordsAtDrop);
        Say(std::string(kPropsPrefix) + csv);
        Go(Ph::AwaitPuts);
        return;
    }
    case Ph::AwaitPuts:
        PrintNew();
        if (Heard(kPutsDoneLine)) { Go(Ph::Settle); return; }
        if (now - g_h.phaseMs > kClientBoundMs) Abandon("the client never finished its puts");
        return;
    case Ph::Settle:
        PrintNew();
        if (PropsOutside() == 0 || now - g_h.phaseMs > kPutsSettleMs) {
            UE_LOGI("[SACK-DRILL] host puts: %d of %zu props are in the sack; still outside (eids): %s",
                    static_cast<int>(g_h.propEids.size()) - PropsOutside(), g_h.propEids.size(),
                    OutsideList().c_str());
            Go(Ph::Call);
        }
        return;
    case Ph::Call:
        PrintNew();
        CallDrone();
        return;
    case Ph::Sale:
        Sale();
        return;
    case Ph::AwaitTake:
        AwaitTake();
        return;
    case Ph::Fill:
        Fill();
        return;
    case Ph::Tail:
        Tail();
        return;
    case Ph::Done:
        return;
    }
}

}  // namespace coop::dev::dronesack_drill::detail

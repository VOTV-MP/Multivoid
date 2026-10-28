// coop/dev/container_route_drill_host.cpp -- the HOST's half of the route census drill; see
// coop/dev/container_route_drill.h.

#include "container_route_drill_detail.h"

#include "coop/dev/container_route_census.h"
#include "coop/dev/drill_chat.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/container_contents_sync.h"
#include "coop/props/prop_save_data.h"   // Covers
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"   // IsActorRootSimulatingPhysics
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::container_route_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace ci  = ue_wrap::container_inventory;
namespace CV  = ue_wrap::container_view;
namespace cen = coop::dev::container_route_census;
namespace cs  = coop::props::container_contents_sync;

constexpr uint64_t kFillBoundMs    = 5000;   // the spawn to the backpack's inventory binding its slot
constexpr uint64_t kPublishBoundMs = 10000;  // the fill to the first fan-out
constexpr uint64_t kViewBoundMs    = 5000;   // the open verb to the screen showing the backpack
constexpr uint64_t kJudgeBoundMs   = 15000;  // the client's R1 slice sent to the host judging it
constexpr const wchar_t* kSecondClass = L"prop_floppyDisc_R_C";   // the second class of read 7; no drill puts a disc into a container, so the capture check below is its guard
constexpr const wchar_t* kSecondPath  = L"/Game/objects/prop_floppyDisc_R.prop_floppyDisc_R_C";
constexpr uint64_t kRoutesBoundMs  = 300000; // the routes phase: above the client's worst chain of independent bounds (about 230 s)
constexpr uint64_t kBeforeBoundMs  = 60000;  // the unpublished write to the client's take reported
constexpr uint64_t kThrownBoundMs  = 30000;  // r4-start to the host holding a drive made by the throw

enum class Step : uint8_t { Arm, Fill, Publish, WaitR1, WaitJudge, WaitRoutes, ViewOpen, ViewRegen, WaitBefore, WaitDone, Done };
Step     g_step = Step::Arm;
uint64_t g_stepMs = 0;
uint64_t g_next = 0;
ue_wrap::CachedObjRef g_pack, g_r4Prop;
uint32_t g_eid = static_cast<uint32_t>(coop::element::kInvalidId);
int      g_slotsBefore = 0;
uint64_t g_gen0 = 0;
void*    g_slot0 = nullptr;   // the screen's first slot widget before the raw write
int      g_drives0 = 0;
bool     g_opened = false;   // the open verb was sent for the host's screen
int      g_r1Sent = 0;       // the slices the client had sent when it finished R1
std::string g_slotText0;   // the screen's first slot before the raw write

bool     g_r4Started = false, g_r4Over = false, g_r4Gone = false, g_r4Failed = false;   // g_r4Failed: R4 ended without a drive
uint64_t g_r4StartMs = 0;
uint64_t g_ov0 = 0;          // the census's R4 overlaps at r4-start
std::vector<uint32_t> g_driveSet, g_now;   // the live drive element ids at r4-hold; the ones live now
bool     g_r4Armed = false;  // the set is taken
uint32_t g_r4Eid = 0;        // the drive the throw made here (0 until found)
std::vector<coop::element::Registry::ActorIdPair> g_snap;
TraceRate g_trHost;

void Next(Step s) { g_step = s; g_stepMs = ::GetTickCount64(); }
bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }
bool Due() {
    const uint64_t now = ::GetTickCount64();
    if (now < g_next) return false;
    g_next = now + kCheckEveryMs;
    return true;
}
bool Said(const char* line) {
    return coop::dev::drill_chat::PollDue() && coop::dev::drill_chat::ChatHasLive(line);
}

void Fail(const char* why) {
    Abandon("host", why);
    g_step = Step::Done;
}

// Every route was tried: the run's last lines and the done marker.
void HostDone() {
    LogLargest("host");
    cen::ReportNow("host end");
    UE_LOGI("[CI-CENSUS] drill host DONE");
    Next(Step::Done);
}

ue_wrap::FVector Offset(const ue_wrap::FVector& at, const ue_wrap::FVector& fwd, float ahead, float side) {
    return {at.X + fwd.X * ahead + fwd.Y * side, at.Y + fwd.Y * ahead - fwd.X * side, at.Z + kDropCm};
}

// The backpack's own put-in, as the player's hand puts an item in. True when it took the item.
bool PutIn(void* pack, void* item) {
    void* inv = ci::InventoryOf(pack);
    void* fn = inv ? R::FindDispatchFunctionCached(R::ClassOf(inv), L"addObject") : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !f.Set<void*>(L"actor", item) || !f.Set<int32_t>(L"insertIndex1", -1) ||
        !ue_wrap::Call(inv, f))
        return false;
    return f.Get<bool>(L"return");
}

// The fixture's own fill, or the refill: two drives put into the backpack the way the player's hand puts one in,
// each destroyed after the put that took, none counted as a route. Returns how many the backpack took.
int AddDrives(void* pack) {
    void* itemCls = R::FindClass(kDriveClass);
    ue_wrap::FVector put{};
    if (!itemCls || !E::TryGetActorLocation(pack, put)) return -1;
    cen::Quiet quiet;
    int added = 0;
    for (int i = 0; i < kItems; ++i) {
        void* item = E::SpawnActor(itemCls, {put.X, put.Y, put.Z + 60.f + 30.f * i});
        if (item && PutIn(pack, item)) {
            ++added;
            E::DestroyActor(item);   // as every BP caller does after a put that took
        }
    }
    return added;
}

void StepArm(coop::net::Session* s) {
    switch (SeamsGate("host")) {   // the first step waits for every watch to be live
        case SeamGate::Wait: return;
        case SeamGate::Dead: g_step = Step::Done; return;
        case SeamGate::Go:   break;
    }
    if (!s->IsSlotWorldReady(1)) return;
    void* puppet = coop::puppet_drive::Puppet(1).GetActor();   // spawned once the peer's pose arrives
    ue_wrap::FVector at{};
    if (!puppet || !R::IsLive(puppet) || !E::TryGetActorLocation(puppet, at)) return;
    void* packCls = ue_wrap::asset_load::LoadObjectByPath(kPackPath);
    if (!packCls) { Fail("the backpack's class does not load"); return; }
    const ue_wrap::FVector fwd = E::GetActorForwardVector(puppet);
    void* pack = E::SpawnActor(packCls, Offset(at, fwd, kAheadCm, 0.f));
    if (!pack || !ci::IsContainer(pack)) { Fail("the backpack did not spawn as a container"); return; }
    g_pack.Set(pack);
    if (!cen::IsBeforeOnly()) {
        // One loose drive to the side, which the client holds and throws into the backpack (R4). The client's mirror
        // arrives where the host put it.
        void* driveCls = R::FindClass(kDriveClass);
        void* b = driveCls ? E::SpawnActor(driveCls, Offset(at, fwd, kSideAheadCm, -kSideCm)) : nullptr;
        if (!b) { Fail("the loose drive did not spawn"); return; }
    }
    Next(Step::Fill);
}

void StepFill() {
    void* pack = g_pack.Get();
    void* inv = pack ? ci::InventoryOf(pack) : nullptr;
    if (!pack) { Fail("the backpack died"); return; }
    if (!inv || !ci::ContentsSlot(inv)) {   // its inventory's own init binds the save slot's contents first
        if (Expired(kFillBoundMs)) Fail("the backpack's inventory never bound its contents slot");
        return;
    }
    const int added = AddDrives(pack);   // the fixture's own fill is no route
    if (added < 0) { Fail("the drive's class or the backpack's place is unread"); return; }
    if (added != kItems) { Fail("the backpack did not take the two drives"); return; }
    Next(Step::Publish);
}

void StepPublish() {
    void* pack = g_pack.Get();
    if (!pack) { Fail("the backpack died"); return; }
    g_eid = EidOf(pack);
    const bool bound = g_eid != static_cast<uint32_t>(coop::element::kInvalidId);
    if (!bound || !cs::SentBaselineKnown(g_eid)) {
        if (Expired(kPublishBoundMs)) Fail(bound ? "the backpack's contents were never published" : "the backpack has no element id");
        return;
    }
    UE_LOGI("[CI-CENSUS] drill host: a backpack holding %d drives before the client's puppet, eid=%u", kItems, g_eid);
    LogHashes("host", g_eid, "published");
    LogSizes("host", "fixture", pack);
    cen::ReportNow("host fixture");
    Next(cen::IsBeforeOnly() ? Step::WaitBefore : Step::WaitR1);
}

// The client's R1 take is done: once the host has judged the slices it sent, the backpack is handed two fresh records
// (the fixture the R2 put-back and the later reads stand on), and the client is told.
void StepWaitR1() {
    std::string rest;
    int sent = 0;
    if (coop::dev::drill_chat::PollDue() && coop::dev::drill_chat::LiveRestOf(kPrefixR1Done, &rest) &&
        IntAfter(rest, "sent", sent)) {
        g_r1Sent = sent;
        Next(Step::WaitJudge);
        return;
    }
    if (Expired(kRoutesBoundMs)) {
        RouteFail("R1 the client never reported its R1 done");
        Next(Step::WaitRoutes);
    }
}

void StepWaitJudge() {
    const uint64_t judged = cen::HostSlices(1, true) + cen::HostSlices(1, false);
    if (judged < static_cast<uint64_t>(g_r1Sent)) {
        if (!Expired(kJudgeBoundMs)) return;
        RouteFail("R1 the client's slice was never judged");   // the fresh records are R2's fixture: handed all the same
    }
    void* pack = g_pack.Get();
    if (!pack) { Fail("the backpack died"); return; }
    if (AddDrives(pack) != kItems) {
        RouteFail("R2 the backpack did not take the two fresh drives");
        coop::dev::drill_chat::Say(kLineRefillRefused);   // the client's R2 ends with it
        Next(Step::WaitRoutes);
        return;
    }
    UE_LOGI("[CI-CENSUS] drill host: two fresh records handed to the backpack (%d record(s) now), after %llu judged slice(s)",
            RecordCount(g_eid), static_cast<unsigned long long>(judged));
    coop::dev::drill_chat::Say(kLineRefilled);
    Next(Step::WaitRoutes);
}

// The live prop_drive_C element ids now, in `out`.
void LiveDriveEids(std::vector<uint32_t>& out) {
    out.clear();
    void* driveCls = R::FindClass(kDriveClass);
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, g_snap);
    for (const auto& p : g_snap) {
        void* a = coop::element::LivePropActor(p.id);   // null when the element's actor is gone
        if (a && driveCls && R::ClassOf(a) == driveCls) out.push_back(p.id);
    }
}

// The drive the host's drop path made for the client's throw: the first prop_drive_C element that is not in the set
// taken at r4-hold. True once found and named to the client.
bool FindThrownDrive() {
    LiveDriveEids(g_now);
    uint32_t best = static_cast<uint32_t>(coop::element::kInvalidId);
    for (uint32_t id : g_now)
        if (id < best && std::find(g_driveSet.begin(), g_driveSet.end(), id) == g_driveSet.end()) best = id;
    if (best == static_cast<uint32_t>(coop::element::kInvalidId)) return false;
    g_r4Prop.Set(coop::element::LivePropActor(best));
    g_r4Eid = best;
    char say[48];
    std::snprintf(say, sizeof(say), "%seid=%u", kPrefixR4Thrown, best);
    coop::dev::drill_chat::Say(say);
    UE_LOGI("[CI-CENSUS] drill host: the throw made a drive here, eid=%u (not among the %zu armed at r4-hold)", best,
            g_driveSet.size());
    return true;
}

// From the found drive to the client's done line, one read per check, with the clock the client's lines carry.
void TraceR4() {
    void* prop = g_r4Prop.Get();
    ue_wrap::FVector at{};
    const bool live = prop && E::TryGetActorLocation(prop, at);
    if (!live && !g_r4Gone) {
        g_r4Gone = true;
        UE_LOGI("[CI-CENSUS] r4 host eid=%u gone t=%llu", g_r4Eid, static_cast<unsigned long long>(cen::UnixMs()));
    }
    if (live && g_trHost.Take())
        UE_LOGI("[CI-CENSUS] r4 host eid=%u sim=%d t=%llu pos=(%.1f,%.1f,%.1f)", g_r4Eid, E::IsActorRootSimulatingPhysics(prop) ? 1 : 0,
                static_cast<unsigned long long>(cen::UnixMs()), at.X, at.Y, at.Z);
}

// The client drives R4 here; each of its start lines is answered, and the host's side of R4 is traced. R4 ends for the
// host with the client's done line, a drive found or not.
void StepWaitRoutes() {
    std::string rest;
    int held = 0;
    if (!g_r4Armed && coop::dev::drill_chat::LiveRestOf(kPrefixR4Hold, &rest)) {
        g_r4Armed = true;
        LiveDriveEids(g_driveSet);
        coop::dev::drill_chat::Say(kLineR4Armed);
        UE_LOGI("[CI-CENSUS] drill host: r4-hold, armed with %zu live drive element(s)", g_driveSet.size());
    }
    if (!g_r4Started && coop::dev::drill_chat::LiveRestOf(kPrefixR4Start, &rest) && IntAfter(rest, "eid", held)) {
        g_r4Started = true;
        g_r4StartMs = ::GetTickCount64();
        g_ov0 = cen::R4Overlaps();
        UE_LOGI("[CI-CENSUS] drill host: r4-start, the client holds eid=%d", held);
    }
    if (g_r4Started && g_r4Armed && !g_r4Eid && !g_r4Failed && !FindThrownDrive() &&
        ::GetTickCount64() - g_r4StartMs > kThrownBoundMs) {
        RouteFail("R4 the host made no drive for the throw");
        g_r4Failed = true;
    }
    if (g_r4Started && !g_r4Over) {
        if (coop::dev::drill_chat::ChatHasLive(kLineR4Done)) {
            g_r4Over = true;
            UE_LOGI("[CI-CENSUS] drill host: R4 overlap n=%llu", static_cast<unsigned long long>(cen::R4Overlaps() - g_ov0));
            if (!g_r4Eid) UE_LOGI("[CI-CENSUS] drill host: R4 ended by the client, no drive found");   // the find goes on to its bound
        } else if (g_r4Eid) {
            TraceR4();
        }
    }
    if ((!g_r4Started || g_r4Eid || g_r4Failed || g_r4Over) && coop::dev::drill_chat::ChatHasLive(kLineRoutesDone)) {
        Next(Step::ViewOpen);
    } else if (Expired(kRoutesBoundMs)) {
        RouteFail("the client never reported its routes done");
        Next(Step::ViewOpen);
    }
}

// The view route failed: its FAIL line, the screen closed, and on to the stale take.
void ViewFailed(const char* phase) {
    RouteFail(phase);
    CV::Close();
    Next(Step::WaitBefore);
}

// Read 7: the host's own screen onto the backpack, one raw write the way an accepted client slice is applied, and
// whether the open screen rebuilt (its gen_player body ran) or still shows the slots it had.
void StepViewOpen() {
    void* pack = g_pack.Get();
    if (!pack) { Fail("the backpack died"); return; }
    if (!g_opened) {
        g_opened = true;
        if (!CV::Open(pack)) ViewFailed("view the host's screen could not be opened onto the backpack");
        return;
    }
    if (CV::Viewed() == pack) {
        g_slotsBefore = SlotCount(Screen());
        g_gen0 = cen::GenCalls();
        g_slot0 = SlotAt(Screen(), 0);
        g_slotText0 = FirstSlotText();
        // The write adds a record of a second class (a small prop the backpack takes), so the screen's slots can move.
        void* cls = R::FindClass(kSecondClass);
        if (!cls) cls = ue_wrap::asset_load::LoadObjectByPath(kSecondPath);
        ue_wrap::FVector put{};
        void* item = cls && E::TryGetActorLocation(pack, put) ? E::SpawnActor(cls, {put.X, put.Y, put.Z + 90.f}) : nullptr;
        if (!item) { ViewFailed("view the second item did not spawn"); return; }
        // The record lane admits the class (Covers) and its capture neither refuses nor ejects the class: said, and a
        // refusal ends the read rather than appending a record the lane could not have made.
        ue_wrap::save_record::SaveRecord probe;
        const bool covers = coop::prop_save_data::Covers(item);
        const bool captured = covers && ue_wrap::save_record::CaptureRecord(item, probe);
        UE_LOGI("[CI-CENSUS] drill host: the second class %ls covers=%d capture=%d", R::ClassNameOf(item).c_str(), covers ? 1 : 0,
                captured ? 1 : 0);
        if (!captured) {
            E::DestroyActor(item);
            ViewFailed("view the second class's record would not capture");
            return;
        }
        bool written = false;
        {
            cen::Quiet quiet;
            written = cs::DevRawAppendActor(g_eid, item, /*publish=*/true);
        }
        E::DestroyActor(item);
        if (!written) { ViewFailed("view the raw write to the backpack did not apply"); return; }
        UE_LOGI("[CI-CENSUS] drill host: one raw write of a second class applied to eid=%u (%d record(s) now), published",
                g_eid, RecordCount(g_eid));
        Next(Step::ViewRegen);
        return;
    }
    if (Expired(kViewBoundMs)) ViewFailed("view the screen never showed the backpack");
}

void StepViewRegen() {
    const bool regen = ScreenRebuilt(g_gen0, g_slot0);
    if (!regen && !Expired(kRegenWindowMs)) return;
    UE_LOGI("[CI-CENSUS] host view regen=%d slots %d -> %d%s", regen ? 1 : 0, g_slotsBefore, SlotCount(Screen()),
            SlotChange(g_slotText0, FirstSlotText()).c_str());
    CV::Close();
    LogHashes("host", g_eid, "after the raw write");
    LogSizes("host", "after the raw write", g_pack.Get());
    cen::ReportNow("host read 7");
    Next(Step::WaitBefore);
}

void StepWaitBefore() {
    if (coop::dev::drill_chat::ChatHasLive(kLineBeforeFailed)) {
        UE_LOGI("[CI-CENSUS] drill host: the client ended the take route before it was ready");
        HostDone();
        return;
    }
    if (!Said(kLineBeforeReady)) {
        if (Expired(kRoutesBoundMs)) { RouteFail("before the client never got ready for the take"); HostDone(); }
        return;
    }
    if (!cs::DevRawAppend(g_eid, /*publish=*/false)) { RouteFail("before the unpublished raw write did not apply"); HostDone(); return; }
    g_drives0 = LiveDrives();
    UE_LOGI("[CI-CENSUS] drill host: a raw write NOT published to eid=%u (%d record(s) here), the client's base is stale",
            g_eid, RecordCount(g_eid));
    coop::dev::drill_chat::Say(kLineBeforeWritten);
    Next(Step::WaitDone);
}

void StepWaitDone() {
    std::string rest;
    int rb = 0, ic = 0;
    if (coop::dev::drill_chat::ChatHasLive(kLineBeforeFailed)) {
        UE_LOGI("[CI-CENSUS] drill host: the client ended the take route");
        HostDone();
        return;
    }
    if (!coop::dev::drill_chat::PollDue() || !coop::dev::drill_chat::LiveRestOf(kPrefixClientDone, &rest) ||
        !IntAfter(rest, "rb", rb) || !IntAfter(rest, "ic", ic)) {
        if (Expired(kBeforeBoundMs)) { RouteFail("before the client never reported the take"); HostDone(); }
        return;
    }
    const int ih = LiveDrives() > g_drives0 ? 1 : 0;
    char say[48];
    std::snprintf(say, sizeof(say), "%sih=%d", kPrefixHostDone, ih);
    coop::dev::drill_chat::Say(say);
    // The host's own container never lost the record, so its answer to "is it back" is yes.
    UE_LOGI("[CI-CENSUS] before record-back=1 item-client=%d item-host=%d (the host judged slot 1: accepted=%llu refused=%llu)",
            ic, ih, static_cast<unsigned long long>(cen::HostSlices(1, true)),
            static_cast<unsigned long long>(cen::HostSlices(1, false)));
    HostDone();
}

}  // namespace

void HostTick(coop::net::Session* s) {
    if (g_step == Step::Done) return;
    if (g_step == Step::ViewOpen || g_step == Step::ViewRegen) {
        // The screen steps run each tick: the open verb and the write are separate frames.
    } else if (!Due()) {
        return;
    }
    switch (g_step) {
    case Step::Arm:        StepArm(s); return;
    case Step::Fill:       StepFill(); return;
    case Step::Publish:    StepPublish(); return;
    case Step::WaitR1:     StepWaitR1(); return;
    case Step::WaitJudge:  StepWaitJudge(); return;
    case Step::WaitRoutes: StepWaitRoutes(); return;
    case Step::ViewOpen:   StepViewOpen(); return;
    case Step::ViewRegen:  StepViewRegen(); return;
    case Step::WaitBefore: StepWaitBefore(); return;
    case Step::WaitDone:   StepWaitDone(); return;
    case Step::Done:       return;
    }
}

void HostReset() {
    g_step = Step::Arm;
    g_stepMs = g_next = 0;
    g_pack.Reset();
    g_eid = static_cast<uint32_t>(coop::element::kInvalidId);
    g_slotsBefore = 0;
    g_gen0 = 0;
    g_slot0 = nullptr;
    g_slotText0.clear();
    g_r1Sent = 0;
    g_r4Prop.Reset();
    g_drives0 = 0;
    g_r4StartMs = g_ov0 = 0;
    g_r4Started = g_r4Over = g_r4Gone = g_r4Failed = false;
    g_driveSet.clear();
    g_r4Armed = false;
    g_r4Eid = 0;
    g_trHost.Reset();
    g_opened = false;
}

}  // namespace coop::dev::container_route_drill::detail

// coop/dev/container_route_drill_client.cpp -- the CLIENT's half of the route census drill; see
// coop/dev/container_route_drill.h.

#include "container_route_drill_client_state.h"
#include "container_route_drill_detail.h"
#include "drive_drill_verbs.h"  // Hold, Throw: the player's own pickup and throw (co-located private header)

#include "coop/dev/container_route_census.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_hand.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/session/join_progress.h"
#include "coop/props/container_contents_sync.h"  // DevRecordHashes
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::container_route_drill::detail {

namespace cen = coop::dev::container_route_census;

namespace {
uint64_t g_stepMs = 0;
Step     g_step = Step::Arm;   // Done ends the drill
}

ue_wrap::CachedObjRef g_pack, g_drive;   // g_drive: the loose drive R4 holds and throws

void Next(Step s) { g_step = s; g_stepMs = ::GetTickCount64(); }
bool Expired(uint64_t bound) { return ::GetTickCount64() - g_stepMs > bound; }
void RouteFailTo(const char* phase, Step next) {
    RouteFail(phase);
    Next(next);
}
void Finish(const char* label, Step next) {
    cen::ReportNow(label);
    Next(next);
}
void* HeldActor(void* player) {
    namespace E = ue_wrap::engine;
    E::MainPlayerGrabState hand{};
    return E::ReadMainPlayerGrabState(player, hand) ? hand.holdingActor : nullptr;
}

namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace CV  = ue_wrap::container_view;
namespace DV  = coop::dev::drive_drill_verbs;
namespace cs  = coop::props::container_contents_sync;

constexpr uint64_t kFindBoundMs   = 60000;   // world-ready to the fixtures' mirrors
constexpr uint64_t kSettleBoundMs = 30000;   // the mirrors' first sight to rest and the host's drive ended
constexpr uint64_t kViewBoundMs   = 5000;    // the open verb to the screen showing the backpack
constexpr uint64_t kSliceBoundMs  = 6000;    // a route's write to the slice it sends (the sweep is 250 ms)
constexpr uint64_t kBackBoundMs   = 8000;    // the refused slice to the host's truth coming back
constexpr int      kRestChecks    = 4;
constexpr int      kEmTakeCalls   = 4;       // the first em_take ever only initialises the screen

uint64_t g_next = 0;
uint32_t g_eid = static_cast<uint32_t>(coop::element::kInvalidId);
ue_wrap::FVector g_last[2] = {};
int      g_rest = 0;
uint64_t g_slice0 = 0;
int      g_inv0 = 0;
int      g_calls = 0;
int32_t  g_count0 = 0, g_countTaken = 0;
int      g_slots0 = 0;
void*    g_slot0 = nullptr;      // the screen's first slot widget at the view-regen baseline
uint64_t g_gen0 = 0;
std::string g_slotText0;         // the screen's first slot at the view-regen baseline
uint64_t g_changedMs = 0;
int      g_rb = 0, g_ic = 0;
bool     g_refilled = false;     // the host said it handed the backpack its fresh records
uint64_t g_r2Add0 = 0, g_r1Add0 = 0;   // the add verbs counted under R2 / R1 before the R2 calls
std::vector<uint64_t> g_hashes;  // the backpack's record hashes at the last baseline (R1, then R2)
uint64_t g_r1Took = 0;           // the record R1's press took, by hash
uint64_t g_pressMs = 0, g_pressToSliceMs = 0;   // the take's press, and its press-to-slice time

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
    Abandon("client", why);
    g_step = Step::Done;
}
// The write is sent as a slice within the lane's sweep, or the step says it was not.
bool SliceSent(const char* route) {
    if (cen::SlicesSent() > g_slice0) return true;
    if (!Expired(kSliceBoundMs)) return false;
    char phase[96];
    std::snprintf(phase, sizeof(phase), "%s sent no slice within %llu ms", route, static_cast<unsigned long long>(kSliceBoundMs));
    RouteFail(phase);
    return true;
}

// The slot press a player's click makes: the hover set on the screen, then the slot's own button.
bool PressSlot(void* ui, int index) {
    void* slot = SlotAt(ui, index);
    void* hover = ui ? R::FindDispatchFunctionCached(R::ClassOf(ui), L"setHoverContainerSlot") : nullptr;
    if (!slot) return false;
    ue_wrap::ParamFrame f(hover);
    if (hover && f.valid() && f.Set<void*>(L"containerSlot", slot)) ue_wrap::Call(ui, f);
    return CallNoArg(slot, L"pressButton");
}

// The screen's hovered container slot cleared the way the game's own unhover does (setHoverContainerSlot with no
// object), and read back: em_take presses the hovered slot first, so a stale hover makes the take key an R1.
bool ClearHover(void* ui) {
    void* fn = ui ? R::FindDispatchFunctionCached(R::ClassOf(ui), L"setHoverContainerSlot") : nullptr;
    ue_wrap::ParamFrame f(fn);
    const int32_t off = ui ? R::FindPropertyOffset(R::ClassOf(ui), L"hoverContainerSlot") : -1;
    if (!fn || !f.valid() || off < 0 || !f.Set<void*>(L"containerSlot", nullptr) || !ue_wrap::Call(ui, f)) return false;
    return *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(ui) + off) == nullptr;
}

// The first hash of `a` that `b` does not hold (a record `b` lacks), or 0 when `b` holds them all.
uint64_t FirstMissing(const std::vector<uint64_t>& a, std::vector<uint64_t> b) {
    for (uint64_t h : a) {
        bool found = false;
        for (size_t i = 0; i < b.size() && !found; ++i)
            if (b[i] == h) { b.erase(b.begin() + static_cast<std::ptrdiff_t>(i)); found = true; }
        if (!found) return h;
    }
    return 0;
}

// hold+run with the held prop: playerUsedOn with the player's input_run raised for the call.
bool UseWithRun(void* player, void* pack, void* hold) {
    int32_t off = -1;
    uint8_t mask = 0;
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(pack), L"playerUsedOn");
    ue_wrap::ParamFrame f(fn);
    if (!fn || !f.valid() || !R::FindBoolProperty(R::ClassOf(player), L"input_run", off, mask) || off < 0 ||
        !f.Set<void*>(L"player", player) || !f.Set<void*>(L"holdObject", hold))
        return false;
    uint8_t* flag = static_cast<uint8_t*>(player) + off;
    const uint8_t was = *flag;
    *flag = static_cast<uint8_t>(was | mask);
    const bool ok = ue_wrap::Call(pack, f);
    *flag = static_cast<uint8_t>((*flag & ~mask) | (was & mask));
    return ok;
}

struct Gather {
    ue_wrap::FVector from;
    void* pack = nullptr;
    float packCm = kNearCm;
    void* drives[2] = {};
    int nDrives = 0;
};

void GatherFixtures(void* player, Gather& g) {
    if (!E::TryGetActorLocation(player, g.from)) return;
    if (void* cls = ue_wrap::object_index::ClassByName(kPackClass))
        ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
            auto* x = static_cast<Gather*>(c);
            ue_wrap::FVector at{};
            if (!R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__") || !E::TryGetActorLocation(obj, at)) return;
            const float cm = std::hypot(at.X - x->from.X, at.Y - x->from.Y);
            const uint32_t eid = EidOf(obj);
            if (cm < x->packCm && eid != static_cast<uint32_t>(coop::element::kInvalidId) && RecordCount(eid) == kItems) {
                x->pack = obj;
                x->packCm = cm;
            }
        }, &g);
    if (void* cls = ue_wrap::object_index::ClassByName(kDriveClass))
        ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
            auto* x = static_cast<Gather*>(c);
            ue_wrap::FVector at{};
            if (x->nDrives >= 2 || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__") ||
                !E::TryGetActorLocation(obj, at) || EidOf(obj) == static_cast<uint32_t>(coop::element::kInvalidId))
                return;
            if (std::hypot(at.X - x->from.X, at.Y - x->from.Y) < kNearCm) x->drives[x->nDrives++] = obj;
        }, &g);
}

void StepFind(void* player) {
    Gather g;
    GatherFixtures(player, g);
    if (g.pack && (cen::IsBeforeOnly() || g.nDrives == 1)) {
        g_pack.Set(g.pack);
        g_eid = EidOf(g.pack);
        if (!cen::IsBeforeOnly()) g_drive.Set(g.drives[0]);
        g_rest = 0;
        UE_LOGI("[CI-CENSUS] drill client: the backpack's mirror holds the host's %d records, eid=%u", kItems, g_eid);
        LogHashes("client", g_eid, "published");
        LogSizes("client", "fixture", g.pack);
        Next(Step::Settle);
        return;
    }
    if (Expired(kFindBoundMs)) Fail("no backpack with the host's records, and one loose drive, stood near this client within 60 s");
}

void StepSettle() {
    void* actors[2] = {g_pack.Get(), g_drive.Get()};
    const int n = cen::IsBeforeOnly() ? 1 : 2;
    bool still = true, parked = false;
    for (int i = 0; i < n; ++i) {
        ue_wrap::FVector at{};
        if (!actors[i] || !E::TryGetActorLocation(actors[i], at)) { Fail("a fixture's mirror died"); return; }
        still = still && std::fabs(at.X - g_last[i].X) + std::fabs(at.Y - g_last[i].Y) + std::fabs(at.Z - g_last[i].Z) < 1.f;
        g_last[i] = at;
        parked = parked || coop::prop_drive_stream::IsParked(actors[i]);
    }
    g_rest = still ? g_rest + 1 : 0;
    if (g_rest >= kRestChecks && !parked) { Next(Step::Open); return; }
    if (Expired(kSettleBoundMs)) Fail(parked ? "a mirror stayed parked by the host's drive" : "the mirrors never came to rest");
}

// ---- the routes -----------------------------------------------------------------------------------------------

void StepOpen() {
    void* pack = g_pack.Get();
    if (!pack || !CV::Open(pack)) { Fail("the screen could not be opened onto the backpack"); return; }
    Next(Step::OpenWait);
}

void StepOpenWait() {
    if (CV::Viewed() == g_pack.Get() && SlotCount(Screen()) >= 1) {
        UE_LOGI("[CI-CENSUS] drill client: the screen shows the backpack, %d container slot(s)", SlotCount(Screen()));
        Next(cen::IsBeforeOnly() ? Step::BeforeReady : Step::R1);
        return;
    }
    if (Expired(kViewBoundMs)) Fail("the screen never showed the backpack");
}

void StepR1() {
    g_slice0 = cen::SlicesSent();
    g_inv0 = ue_wrap::inventory::LivePersonalStoreCount();
    uint64_t whole = 0;
    cs::DevRecordHashes(g_eid, g_hashes, whole);
    if (!PressSlot(Screen(), 0)) { RouteFailTo("R1 the slot press did not dispatch", Step::R1Done); return; }
    UE_LOGI("[CI-CENSUS] drill client: R1 slot pressed (route entries %llu)", static_cast<unsigned long long>(cen::RouteEntries(0)));
    Next(Step::R1Wait);
}

void StepR1Wait() {
    if (SliceSent("R1")) Finish("R1", Step::R1Done);
}

// R1 is over: the record its press took is named by what the backpack lost, and the host is told how many slices this
// peer has sent so it hands the backpack two fresh records only after it has judged them.
void StepR1Done() {
    std::vector<uint64_t> after;
    uint64_t whole = 0;
    cs::DevRecordHashes(g_eid, after, whole);
    g_r1Took = FirstMissing(g_hashes, after);
    g_count0 = RecordCount(g_eid);
    UE_LOGI("[CI-CENSUS] drill client: R1 took record hash=%016llx, the backpack holds %d record(s)",
            static_cast<unsigned long long>(g_r1Took), g_count0);
    char say[64];
    std::snprintf(say, sizeof(say), "%ssent=%llu", kPrefixR1Done, static_cast<unsigned long long>(cen::SlicesSent()));
    coop::dev::drill_chat::Say(say);
    Next(Step::R2Refill);
}

// The host hands the backpack two fresh records once it has judged R1's slice; the put-back goes on the base that
// carries them, never on a stale one.
void StepR2Refill() {
    if (!g_refilled) {
        if (!Said(kLineRefilled)) {
            if (Expired(kHostBoundMs)) RouteFailTo("R2 the host never handed the backpack its fresh records", Step::R3);
            return;
        }
        g_refilled = true;
        if (coop::dev::drill_chat::ChatHasLive(kLineRefillRefused)) {
            RouteFailTo("R2 the host's backpack refused the fresh records", Step::R3);
            return;
        }
    }
    if (RecordCount(g_eid) < g_count0 + kItems) {
        if (Expired(kHostBoundMs)) RouteFailTo("R2 the fresh records never reached this peer's backpack", Step::R3);
        return;
    }
    UE_LOGI("[CI-CENSUS] drill client: the backpack holds %d record(s) with the host's fresh ones", RecordCount(g_eid));
    Next(Step::R2);
}

void StepR2() {
    void* ui = Screen();
    const int inv = ue_wrap::inventory::LivePersonalStoreCount();
    const int32_t off = ui ? R::FindPropertyOffset(R::ClassOf(ui), L"selected") : -1;
    if (inv <= 0 || off < 0) { RouteFailTo("R2 the player's inventory is empty or the screen has no selection", Step::R3); return; }
    if (!ClearHover(ui)) { RouteFailTo("R2 the screen's hovered container slot would not clear", Step::R3); return; }
    const int32_t sel = inv - 1;   // the item R1 took is the last one
    std::memcpy(static_cast<uint8_t*>(ui) + off, &sel, sizeof(sel));
    CallNoArg(ui, L"updSelect");
    g_inv0 = inv;
    g_calls = 0;
    g_slice0 = cen::SlicesSent();
    g_r2Add0 = cen::RouteVerbs(1, false);
    g_r1Add0 = cen::RouteVerbs(0, false);
    uint64_t whole = 0;
    cs::DevRecordHashes(g_eid, g_hashes, whole);
    UE_LOGI("[CI-CENSUS] drill client: R2 the hovered slot cleared, item %d selected of %d, the backpack holds %zu record(s)",
            sel, inv, g_hashes.size());
    Next(Step::R2Call);
}

void StepR2Call() {
    if (ue_wrap::inventory::LivePersonalStoreCount() < g_inv0) { Next(Step::R2Wait); return; }
    if (g_calls >= kEmTakeCalls) { RouteFailTo("R2 four em_take calls put nothing back", Step::R3); return; }
    ++g_calls;
    CallNoArg(Screen(), L"em_take");
}

void StepR2Wait() {
    if (!SliceSent("R2")) return;
    std::vector<uint64_t> after;
    uint64_t whole = 0;
    cs::DevRecordHashes(g_eid, after, whole);
    const uint64_t put = FirstMissing(after, g_hashes);
    const uint64_t r2Adds = cen::RouteVerbs(1, false) - g_r2Add0;
    UE_LOGI("[CI-CENSUS] drill client: R2 put the selected item back after %d em_take call(s): record hash=%016llx "
            "(R1 took %016llx), the backpack %zu -> %zu record(s), add verbs counted R2 +%llu, R1 +%llu",
            g_calls, static_cast<unsigned long long>(put), static_cast<unsigned long long>(g_r1Took), g_hashes.size(),
            after.size(), static_cast<unsigned long long>(r2Adds),
            static_cast<unsigned long long>(cen::RouteVerbs(0, false) - g_r1Add0));
    if (!r2Adds) UE_LOGW("[CI-CENSUS] drill client: R2 put a record and the census counted no add under R2");
    LogSizes("client", "after R2", g_pack.Get());
    Finish("R2", Step::R3);
}

// R3 failed: its FAIL line, the hand emptied as R3's own end empties it, and on to R4.
void R3Fail(const char* phase) {
    coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr);
    RouteFailTo(phase, Step::R4Close);
}

void StepR3() {
    if (!coop::dev::drill_hand::TakeIntoHandNow(L"drive", kDriveClass)) { R3Fail("R3 the drive could not be taken into the hand"); return; }
    Next(Step::R3Use);   // the hand's actor is read a tick later
}

void StepR3Use(void* player) {
    void* hand = HeldActor(player);
    g_slice0 = cen::SlicesSent();
    if (!hand || !UseWithRun(player, g_pack.Get(), hand)) { R3Fail("R3 no hand actor, or playerUsedOn did not dispatch"); return; }
    UE_LOGI("[CI-CENSUS] drill client: R3 hold+run used on the backpack (route entries %llu)",
            static_cast<unsigned long long>(cen::RouteEntries(2)));
    Next(Step::R3Wait);
}

void StepR3Wait() {
    if (!SliceSent("R3")) return;
    LogSizes("client", "after R3", g_pack.Get());
    coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr);
    Finish("R3", Step::R4Close);
}

// ---- the open screen, then the take the host refuses ----------------------------------------------------------

// The view route failed: its FAIL line, the host told the routes are over (its wait ends with the client's), and on to
// the take.
void ViewFailed(const char* phase) {
    RouteFail(phase);
    coop::dev::drill_chat::Say(kLineRoutesDone);
    Next(Step::BeforeReady);
}

// Every route was tried: the run's last lines and the done marker.
void ClientDone() {
    LogLargest("client");
    cen::ReportNow("client end");
    UE_LOGI("[CI-CENSUS] drill client DONE");
    Next(Step::Done);
}

void StepViewOpen() {
    void* pack = g_pack.Get();
    if (CV::Viewed() != pack && !CV::Open(pack)) { ViewFailed("view the screen could not be opened onto the backpack again"); return; }
    Next(Step::ViewOpenWait);
}

void StepViewOpenWait() {
    if (CV::Viewed() != g_pack.Get()) {
        if (Expired(kViewBoundMs)) ViewFailed("view the screen never showed the backpack again");
        return;
    }
    g_slots0 = SlotCount(Screen());
    g_gen0 = cen::GenCalls();
    g_slot0 = SlotAt(Screen(), 0);
    g_slotText0 = FirstSlotText();
    g_count0 = RecordCount(g_eid);
    g_changedMs = 0;
    LogSizes("client", "after the routes", g_pack.Get());
    cen::ReportNow("routes done");
    coop::dev::drill_chat::Say(kLineRoutesDone);
    Next(Step::ViewWait);
}

// The host writes the backpack's contents once (one record added) and publishes it: this peer's open screen is judged
// on whether the applied publication rebuilt it. Nothing else runs in this step; its baseline follows each poll until
// the publication lands, so the last one is taken just before the apply.
void StepViewWait() {
    if (RecordCount(g_eid) != g_count0 + 1) {
        g_gen0 = cen::GenCalls();
        g_slot0 = SlotAt(Screen(), 0);
        g_slotText0 = FirstSlotText();
        if (Expired(kHostBoundMs)) RouteFailTo("view the host's publication never arrived", Step::BeforeReady);
        return;
    }
    if (!g_changedMs) g_changedMs = ::GetTickCount64();
    const bool regen = ScreenRebuilt(g_gen0, g_slot0);
    if (!regen && ::GetTickCount64() - g_changedMs < kRegenWindowMs) return;
    UE_LOGI("[CI-CENSUS] client view regen=%d slots %d -> %d%s", regen ? 1 : 0, g_slots0, SlotCount(Screen()),
            SlotChange(g_slotText0, FirstSlotText()).c_str());
    LogHashes("client", g_eid, "after the host's raw write");
    LogSizes("client", "after the host's raw write", g_pack.Get());
    Finish("view", Step::BeforeReady);
}

// The take route failed: its FAIL line, the host told (its wait ends with the client's), and the run's last lines.
void BeforeFailed(const char* phase) {
    RouteFail(phase);
    coop::dev::drill_chat::Say(kLineBeforeFailed);
    ClientDone();
}

void StepBeforeReady() {
    void* pack = g_pack.Get();
    if (CV::Viewed() != pack) {
        if (!CV::Open(pack) || Expired(kHostBoundMs)) BeforeFailed("before the screen could not be opened for the take");
        return;
    }
    coop::dev::drill_chat::Say(kLineBeforeReady);
    Next(Step::BeforeWait);
}

void StepBeforeWait() {
    if (!Said(kLineBeforeWritten)) {
        if (Expired(kHostBoundMs)) BeforeFailed("before the host never wrote the unpublished change");
        return;
    }
    g_count0 = RecordCount(g_eid);
    g_inv0 = ue_wrap::inventory::LivePersonalStoreCount();
    g_slice0 = cen::SlicesSent();
    if (!PressSlot(Screen(), 0)) { BeforeFailed("before the take's slot press did not dispatch"); return; }
    g_pressMs = ::GetTickCount64();
    UE_LOGI("[CI-CENSUS] drill client: the take pressed on a base the host has moved past (%d record(s) here)", g_count0);
    Next(Step::BeforeTaken);
}

// The press's own local apply has run by the next check: the count the take left here, read before the slice can leave
// and before the host's truth can come back.
void StepBeforeTaken() {
    g_countTaken = RecordCount(g_eid);
    Next(Step::BeforeTake);
}

void StepBeforeTake() {
    if (!SliceSent("the take")) return;
    g_pressToSliceMs = ::GetTickCount64() - g_pressMs;
    Next(Step::BeforeSettle);
}

// The host refuses the stale slice and answers with its truth: the record is back when the count rises again.
void StepBeforeSettle() {
    const int32_t now = RecordCount(g_eid);
    if (now <= g_countTaken && !Expired(kBackBoundMs)) return;
    g_rb = now > g_countTaken ? 1 : 0;
    g_ic = ue_wrap::inventory::LivePersonalStoreCount() > g_inv0 ? 1 : 0;
    UE_LOGI("[CI-CENSUS] drill client: the take left %d record(s), the host's truth brought it to %d", g_countTaken, now);
    char say[48];
    std::snprintf(say, sizeof(say), "%srb=%d ic=%d", kPrefixClientDone, g_rb, g_ic);
    coop::dev::drill_chat::Say(say);
    Next(Step::DoneWait);
}

void StepDoneWait() {
    std::string rest;
    int ih = 0;
    if (!coop::dev::drill_chat::PollDue() || !coop::dev::drill_chat::LiveRestOf(kPrefixHostDone, &rest) ||
        !IntAfter(rest, "ih", ih)) {
        if (Expired(kHostBoundMs)) { RouteFail("before the host never answered the take"); ClientDone(); }
        return;
    }
    UE_LOGI("[CI-CENSUS] before record-back=%d item-client=%d item-host=%d press->slice %llu ms", g_rb, g_ic, ih,
            static_cast<unsigned long long>(g_pressToSliceMs));
    ClientDone();
}

}  // namespace

void ClientTick(coop::net::Session*) {
    if (g_step == Step::Done || !Due()) return;
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    switch (g_step) {
    case Step::Arm:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        switch (SeamsGate("client")) {   // the first step waits for every watch to be live
            case SeamGate::Wait: return;
            case SeamGate::Dead: g_step = Step::Done; return;
            case SeamGate::Go:   break;
        }
        Next(Step::Find);
        return;
    case Step::Find:         StepFind(player); return;
    case Step::Settle:       StepSettle(); return;
    case Step::Open:         StepOpen(); return;
    case Step::OpenWait:     StepOpenWait(); return;
    case Step::R1:           StepR1(); return;
    case Step::R1Wait:       StepR1Wait(); return;
    case Step::R1Done:       StepR1Done(); return;
    case Step::R2Refill:     StepR2Refill(); return;
    case Step::R2:           StepR2(); return;
    case Step::R2Call:       StepR2Call(); return;
    case Step::R2Wait:       StepR2Wait(); return;
    case Step::R3:           StepR3(); return;
    case Step::R3Use:        StepR3Use(player); return;
    case Step::R3Wait:       StepR3Wait(); return;
    case Step::R4Close:      StepR4Close(); return;
    case Step::R4:           StepR4(player); return;
    case Step::R4HoldWait:   StepR4HoldWait(); return;
    case Step::R4Settle:     StepR4Settle(player); return;
    case Step::R4Armed:      StepR4Armed(); return;
    case Step::R4Aim:        StepR4Aim(player); return;
    case Step::R4Wait:       StepR4Wait(); return;
    case Step::ViewOpen:     StepViewOpen(); return;
    case Step::ViewOpenWait: StepViewOpenWait(); return;
    case Step::ViewWait:     StepViewWait(); return;
    case Step::BeforeReady:  StepBeforeReady(); return;
    case Step::BeforeWait:   StepBeforeWait(); return;
    case Step::BeforeTaken:  StepBeforeTaken(); return;
    case Step::BeforeTake:   StepBeforeTake(); return;
    case Step::BeforeSettle: StepBeforeSettle(); return;
    case Step::DoneWait:     StepDoneWait(); return;
    case Step::Done:         return;
    }
}

void ClientReset() {
    g_step = Step::Arm;
    g_stepMs = g_next = 0;
    g_pack.Reset();
    g_drive.Reset();
    HandReset();
    g_eid = static_cast<uint32_t>(coop::element::kInvalidId);
    for (ue_wrap::FVector& v : g_last) v = {};
    g_rest = g_calls = g_inv0 = g_slots0 = g_rb = g_ic = 0;
    g_slice0 = g_gen0 = g_changedMs = 0;
    g_refilled = false;
    g_count0 = g_countTaken = 0;
    g_slot0 = nullptr;
    g_slotText0.clear();
    g_r2Add0 = g_r1Add0 = g_r1Took = g_pressMs = g_pressToSliceMs = 0;
    g_hashes.clear();
}

}  // namespace coop::dev::container_route_drill::detail

// coop/dev/container_intent_drill_put.cpp -- the container intent drill's put arms (putrecord, puthand, putfood,
// puthidden, putnested, putempty and the arms that interleave or break something on purpose, which hook into the
// machine at the ask and at the settle: container_intent_drill_put2.cpp), the CLIENT's half; see
// coop/dev/container_intent_drill.h and, for the host's,
// container_intent_drill_put_host.cpp. Each arm runs alone and MEASURES what the put seats do (container_act_put.cpp):
// the numbers it prints are what the next increment's verdicts are written on.
//   CLIENT -- takes the item by the game's own verb (the pocket, the hold), walks to the target with the director (a
//             CARRY walk for the hand arms, which presses hold+run as it passes), puts, and reads the act's end from
//             container_act_request::dev::LastPut.
// Lines are tagged [CI-DRILL] and end in the arm's own `<role> <arm> PASS` or the drill's `<role> <arm> FAIL: <why>`.

#include "container_intent_drill_put.h"
#include "container_route_drill_detail.h"
#include "drive_drill_verbs.h"  // Pocket, Hold: the player's own verbs (co-located private header)

#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_hand.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/container_act_request.h"
#include "coop/props/prop_act_request.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/prop_gather.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::container_intent_drill::detail {

namespace {
namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace ci = ue_wrap::container_inventory;
namespace CV = ue_wrap::container_view;
namespace DV = coop::dev::drive_drill_verbs;
namespace DR = coop::director;
namespace DC = coop::dev::drill_chat;
namespace AR = coop::props::container_act_request;
namespace RD = coop::dev::container_route_drill::detail;
using ue_wrap::FVector;

constexpr const wchar_t* kFoodClass  = L"prop_food_C";
constexpr const char*    kRefusalText = "Empty it before putting it in a shared container.";

constexpr float    kPassBeyondCm  = 600.f;   // the carry walk's goal, past the target along the line to it
constexpr float    kPassSideCm    = 250.f;   // and to one side, so the walker passes the target and never bumps it
constexpr float    kPressCm       = 250.f;   // the hand arms press hold+run when the target is within this
constexpr float    kPressSpeedCmS = 150.f;   // and the player's horizontal speed is above this
constexpr float    kStrayCm       = 500.f;   // the put item's class is counted this near the target
constexpr uint8_t  kSlateHidden   = 2;       // ESlateVisibility::Hidden
constexpr int      kWalkDeadlineS = 150;
constexpr int      kRestChecks    = 4;
constexpr uint64_t kFixtureBoundMs = 120000;  // the client's wait for the host's line, then for its mirrors
constexpr uint64_t kRestBoundMs    = 30000;
constexpr uint64_t kPocketBoundMs  = 15000;
constexpr uint64_t kOpenBoundMs    = 10000;
constexpr uint64_t kHandBoundMs    = 10000;
constexpr uint64_t kActBoundMs     = 15000;   // the press to the act's end
constexpr uint64_t kSettleMs       = 1500;    // the act's end (or the no-put window) to the counts being read

bool IsRecordArm(Arm a) {   // R2 after a pocket
    return a == Arm::PutRecord || a == Arm::PutNested || a == Arm::PutEmpty || a == Arm::PutHover || a == Arm::PutReserved ||
           a == Arm::PutFull || a == Arm::PutDepart || a == Arm::PutDeadline || a == Arm::PutOwed || a == Arm::PutJoin;
}
bool IsHandArm(Arm a) { return a == Arm::PutHand || a == Arm::PutFood || a == Arm::PutFullHand; }   // R3, walking

// ---- the client ---------------------------------------------------------------------------------------------------

enum class CStep : uint8_t { WaitLine, WaitMirrors, Rest, Pocket, PocketWait, TakeHand, HoldWait, HandRead, Walk, WalkWait,
                             OpenWait, Carry, ActWait, Settle, Quiet, Hook };
CStep    g_cStep = CStep::WaitLine;
uint64_t g_cMs = 0;       // the time the step was entered
uint64_t g_nextMs = 0;
uint32_t g_packEid = 0, g_itemEid = 0, g_item2Eid = 0;
int      g_needCount = 0;   // the records the target holds at the fixture line: the mirror is waited for until it does
int      g_pockets = 0;     // the items pocketed so far
int      g_phase2 = 0;      // the host said on the fixture line that putowed runs its second phase
ue_wrap::CachedObjRef g_pack, g_item, g_item2, g_hand;
FVector  g_packAt{};
FVector  g_lastPos[3] = {};
int      g_rest = 0;
std::shared_ptr<DR::BackgroundWalk> g_walk;
int      g_own0 = -1, g_count0 = -1;
AR::dev::Put g_put0;
uint32_t g_acts0[3] = {};
float    g_minDist = 1e9f, g_topSpeed = 0.f;                          // the carry walk's closest approach and top speed
uint64_t g_endMs = 0;
int      g_settled = 0;   // consecutive reads of the hand holding the item

void CNext(CStep s) { g_cStep = s; g_cMs = ::GetTickCount64(); }
bool CExpired(uint64_t bound) { return ::GetTickCount64() - g_cMs > bound; }

void* HeldActor(void* player) {
    E::MainPlayerGrabState hand{};
    return player && E::ReadMainPlayerGrabState(player, hand) ? hand.holdingActor : nullptr;
}

void WalkStop() {
    if (g_walk && g_walk->state.load() == 0) DR::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
}

// The client's end: the walk stopped and run lowered, the host told, the verdict line printed by the caller's Finish.
void CEnd(int ok, int stray) {
    WalkStop();
    RD::LowerRun();
    // The drive still in the hand is told too, so the host does not count its mirror as a stray.
    const uint32_t held = RD::EidOf(HeldActor(coop::players::Registry::Get().Local()));
    char say[128];
    std::snprintf(say, sizeof(say), "%sok=%d stray=%d mirror=%d held=%u", Put2DonePrefix(), ok, stray, CountOf(g_packEid),
                  held == static_cast<uint32_t>(coop::element::kInvalidId) ? 0u : held);
    DC::Say(say);
}

void CFail(const char* fmt, ...) {
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    CEnd(0, -1);
    Fail("%s", buf);
}

void CPass(int stray) {
    UE_LOGI("[CI-DRILL] client %s PASS", ArmName());
    CEnd(1, stray);
    Finish();
}

// Why a pocket put nothing, said once in the FAIL line: the mirror's root simulating physics (the game's gate), the
// item's Get Volume, and the player's pocket room.
const char* PocketWhy(void* item) {
    static char why[200];
    float volume = -1.f, bound = 0.f, curr = -1.f, room = -1.f;
    ue_wrap::prop_gather::ReadVolumeAndBound(item, volume, bound);
    if (void* pane = CV::PlayerContainer())
        if (void* inv = ci::InventoryOf(pane)) {
            ci::CurrentVolume(inv, curr);
            ci::MaxVolume(inv, room);
        }
    std::snprintf(why, sizeof(why), "root simulating=%d, item volume %.1f, player currVol %.1f maxVol %.1f",
                  ue_wrap::engine::IsActorRootSimulatingPhysics(item) ? 1 : 0, static_cast<double>(volume),
                  static_cast<double>(curr), static_cast<double>(room));
    return why;
}

// The counts a put is judged against, taken just before the press.
void Baseline() {
    g_put0 = AR::dev::LastPut();
    g_acts0[0] = AR::dev::ActsDone();
    g_acts0[1] = AR::dev::ActsAborted();
    g_acts0[2] = AR::dev::ActsRefused();
    g_own0 = ue_wrap::inventory::LivePersonalStoreCount();
    g_count0 = CountOf(g_packEid);
}

bool ActsMoved() {
    return AR::dev::ActsDone() != g_acts0[0] || AR::dev::ActsAborted() != g_acts0[1] ||
           AR::dev::ActsRefused() != g_acts0[2];
}

// The walk to (or past) the target, with the director on its own thread. A carry walk keeps what the hand holds.
bool StartWalk(bool carry) {
    void* player = coop::players::Registry::Get().Local();
    FVector at{};
    if (!player || !E::TryGetActorLocation(player, at)) return false;
    FVector to = g_packAt;
    if (carry) {
        float dx = g_packAt.X - at.X, dy = g_packAt.Y - at.Y;
        const float len = std::hypot(dx, dy);
        if (len < 1.f) return false;
        dx /= len;
        dy /= len;
        to.X = g_packAt.X + dx * kPassBeyondCm - dy * kPassSideCm;
        to.Y = g_packAt.Y + dy * kPassBeyondCm + dx * kPassSideCm;
    }
    g_walk = DR::StartBackgroundWalk(to, kStandCm, kWalkDeadlineS, carry);
    UE_LOGI("[CI-DRILL] client %s walks %s to (%.0f,%.0f,%.0f), target at (%.0f,%.0f,%.0f)", ArmName(),
            carry ? "carrying" : "on", static_cast<double>(to.X), static_cast<double>(to.Y), static_cast<double>(to.Z),
            static_cast<double>(g_packAt.X), static_cast<double>(g_packAt.Y), static_cast<double>(g_packAt.Z));
    return true;
}

// The own inventory's gembox record that was pocketed: its container stack index and that stack's record count.
// putnested reads whether the contents travelled with the pocket; putempty, that the record is empty.
bool MeasureBackpack(int& stackIndex, int& records) {
    ue_wrap::inventory::LivePersonalStore store;
    if (!ue_wrap::inventory::ReadLivePersonalStore(store)) return false;
    for (size_t i = store.records.size(); i-- > 0;) {
        const auto& r = store.records[i];
        if (r.className != kBoxClass) continue;
        int32_t idx = -1;
        if (!ci::ContainerStackIndex(r, idx)) return false;
        int32_t held = 0;
        if (idx >= 0 && !ci::ReadStackCount(idx, held)) held = 0;
        stackIndex = idx;
        records = held;
        return true;
    }
    return false;
}

// ---- the client's steps -------------------------------------------------------------------------------------------

void StepWaitLine() {
    std::string rest;
    if (DC::LiveRestOf(Put2WaitPrefix(), &rest)) {
        int pack = 0, item = 0, item2 = 0, count = 0;
        if (!RD::IntAfter(rest, "pack", pack) || !RD::IntAfter(rest, "item", item) ||
            !RD::IntAfter(rest, "item2", item2) || !RD::IntAfter(rest, "count", count) || pack <= 0) {
            CFail("the host's fixture line does not read (%s)", rest.c_str());
            return;
        }
        g_packEid = static_cast<uint32_t>(pack);
        g_itemEid = static_cast<uint32_t>(item);
        g_item2Eid = static_cast<uint32_t>(item2);
        g_needCount = count;
        if (!RD::IntAfter(rest, "phase2", g_phase2)) g_phase2 = 0;
        if (IsRecordArm(ArmOf()) && (!g_itemEid || (Put2Pockets() > 1 && !g_item2Eid))) {
            CFail("the host's fixture line names no item to pocket (%s)", rest.c_str());
            return;
        }
        CNext(CStep::WaitMirrors);
        return;
    }
    if (CExpired(kFixtureBoundMs)) CFail("the host never said its fixture within %llu s", static_cast<unsigned long long>(kFixtureBoundMs / 1000));
}

void StepWaitMirrors() {
    void* pack = coop::element::LivePropActor(g_packEid);
    void* item = g_itemEid ? coop::element::LivePropActor(g_itemEid) : nullptr;
    void* item2 = g_item2Eid ? coop::element::LivePropActor(g_item2Eid) : nullptr;
    if (pack && (!g_itemEid || item) && (!g_item2Eid || item2) && CountOf(g_packEid) >= g_needCount &&
        E::TryGetActorLocation(pack, g_packAt)) {
        g_pack.Set(pack);
        g_item.Set(item);
        g_item2.Set(item2);
        Put2OnMirrors();
        g_rest = 0;
        UE_LOGI("[CI-DRILL] client %s: the fixture's mirrors are here (target eid=%u holds %d record(s), item eid=%u, "
                "item2 eid=%u)", ArmName(), g_packEid, CountOf(g_packEid), g_itemEid, g_item2Eid);
        CNext(CStep::Rest);
        return;
    }
    if (CExpired(kFixtureBoundMs))
        CFail("the fixture's mirrors never came (target %p, item %p, item2 %p, count %d of %d)", pack, item, item2,
              CountOf(g_packEid), g_needCount);
}

// The host's drive of the fixture (it falls and settles) is over once the mirrors stay put for kRestChecks checks.
void StepRest() {
    void* actors[3] = {g_pack.Get(), g_item.Get(), g_item2.Get()};
    bool still = true;
    for (int i = 0; i < 3; ++i) {
        if (!actors[i]) {
            if ((i == 1 && !g_itemEid) || (i == 2 && !g_item2Eid)) continue;
            CFail("a fixture's mirror died");
            return;
        }
        FVector at{};
        if (!E::TryGetActorLocation(actors[i], at)) {
            CFail("a fixture's mirror does not read");
            return;
        }
        still = still && std::fabs(at.X - g_lastPos[i].X) + std::fabs(at.Y - g_lastPos[i].Y) + std::fabs(at.Z - g_lastPos[i].Z) < 1.f;
        g_lastPos[i] = at;
    }
    g_rest = still ? g_rest + 1 : 0;
    if (g_rest < kRestChecks) {
        if (CExpired(kRestBoundMs)) CFail("the fixture's mirrors never came to rest");
        return;
    }
    E::TryGetActorLocation(g_pack.Get(), g_packAt);
    const Arm a = ArmOf();
    CNext(IsRecordArm(a) ? CStep::Pocket : a == Arm::PutHand || a == Arm::PutFullHand ? CStep::TakeHand
                         : a == Arm::PutFood ? CStep::HoldWait : CStep::Walk);
    if (a == Arm::PutFood) {   // the hold is asked now; HoldWait reads the grant
        void* player = coop::players::Registry::Get().Local();
        if (!player || (!DV::Hold(player, g_item.Get()) && coop::prop_act_request::dev::PendingId() == 0))
            CFail("the food could not be taken into the hand");
    }
}

void StepPocket() {
    void* player = coop::players::Registry::Get().Local();
    g_own0 = ue_wrap::inventory::LivePersonalStoreCount();
    // A mirror's pocket is cancelled and asked of the host: false at once, with the act pending.
    void* const item = g_pockets == 0 ? g_item.Get() : g_item2.Get();   // putdeadline pockets two
    if (!player || (!DV::Pocket(player, item) && coop::prop_act_request::dev::PendingId() == 0)) {
        CFail("the item could not be pocketed");
        return;
    }
    CNext(CStep::PocketWait);
}

void StepPocketWait() {
    if (coop::prop_act_request::dev::PendingId() != 0) {
        if (CExpired(kPocketBoundMs)) CFail("the pocket's grant never came");
        return;
    }
    const int own = ue_wrap::inventory::LivePersonalStoreCount();
    if (own != g_own0 + 1) {
        if (CExpired(kPocketBoundMs))
            CFail("the pocket put no record in the own inventory (%d -> %d): %s", g_own0, own,
                  PocketWhy(g_pockets == 0 ? g_item.Get() : g_item2.Get()));
        return;
    }
    UE_LOGI("[CI-DRILL] client %s pocketed: own inventory %d -> %d", ArmName(), g_own0, own);
    if (++g_pockets < Put2Pockets()) {
        CNext(CStep::Pocket);
        return;
    }
    if (ArmOf() == Arm::PutNested || ArmOf() == Arm::PutEmpty) {
        int stack = -1, records = -1;
        if (!MeasureBackpack(stack, records)) {
            CFail("the pocketed gembox's record does not read");
            return;
        }
        UE_LOGI("[CI-DRILL] client %s pocketed gembox: stack index=%d records=%d", ArmName(), stack, records);
        if (ArmOf() == Arm::PutNested && records == 0) {
            UE_LOGI("[CI-DRILL] client putnested fixture: the contents did not travel with the pocket");
            UE_LOGI("[CI-DRILL] client putnested NOT MEASURED");
            CEnd(2, -1);
            Finish();
            return;
        }
    }
    CNext(CStep::Walk);
}

void StepTakeHand() {
    if (!coop::dev::drill_hand::TakeIntoHandNow(L"drive", RD::kDriveClass)) {
        CFail("the drive could not be taken into the hand");
        return;
    }
    CNext(CStep::HandRead);   // the hand's actor is read a tick later
}

void StepHoldWait() {
    if (coop::prop_act_request::dev::PendingId() != 0) {
        if (CExpired(kHandBoundMs)) CFail("the hold's grant never came");
        return;
    }
    CNext(CStep::HandRead);
}

// The hand holds the item once two consecutive reads name its class; the carry walk then starts.
void StepHandRead() {
    void* hand = HeldActor(coop::players::Registry::Get().Local());
    g_settled = hand && R::ClassNameOf(hand) == PutItemClass(ArmOf()) ? g_settled + 1 : 0;
    if (g_settled < 2) {
        if (CExpired(kHandBoundMs)) CFail("the hand never held the %ls", PutItemClass(ArmOf()));
        return;
    }
    g_settled = 0;
    g_hand.Set(hand);
    if (!StartWalk(/*carry=*/true)) {
        CFail("the carry walk could not start");
        return;
    }
    CNext(CStep::Carry);
}

void StepWalk() {
    if (!StartWalk(/*carry=*/false)) {
        CFail("the walk to the target could not start");
        return;
    }
    CNext(CStep::WalkWait);
}

void StepWalkWait() {
    const int st = g_walk ? g_walk->state.load() : 2;
    if (st == 0) return;
    if (st != 1) {
        CFail("the walk to the target did not arrive");
        return;
    }
    g_walk.reset();
    void* pack = g_pack.Get();
    if (!pack || !CV::Open(pack)) {
        CFail("openPropInv did not dispatch");
        return;
    }
    CNext(CStep::OpenWait);
}

// The hotbar key's put: the screen's em_take pressed with the hover cleared and the own inventory's item selected, the
// baseline taken first. False, with the FAIL printed, when the screen does not read as a put or em_take did not dispatch.
bool AskR2(void* ui) {
    int32_t sel = -1;
    char why[240];
    if (!PutSelectOwnItem(ui, sel, why, sizeof(why))) {
        CFail("%s", why);
        return false;
    }
    Baseline();
    UE_LOGI("[CI-DRILL] client %s presses em_take: item %d selected of %d, target holds %d record(s)", ArmName(), sel,
            g_own0, g_count0);
    Put2BeforeAsk();
    if (!CV::RunPut(ui)) {
        CFail("em_take did not dispatch");
        return false;
    }
    return true;
}

void PressR2(void* ui) {
    if (Put2AskGated()) {   // the hook asks when the host says go
        CNext(CStep::Hook);
        return;
    }
    if (AskR2(ui)) CNext(PutIsNoPutArm(ArmOf()) ? CStep::Quiet : Put2Hooked() ? CStep::Hook : CStep::ActWait);
}

bool HoverSlot(void* ui, void* slot) {
    void* fn = ui ? R::FindDispatchFunctionCached(R::ClassOf(ui), L"setHoverContainerSlot") : nullptr;
    ue_wrap::ParamFrame f(fn);
    return fn && f.valid() && f.Set<void*>(L"containerSlot", slot) && ue_wrap::Call(ui, f);
}

bool HideSlot(void* slot) {
    void* fn = slot ? R::FindDispatchFunctionCached(R::ClassOf(slot), L"SetVisibility") : nullptr;
    ue_wrap::ParamFrame f(fn);
    return fn && f.valid() && f.Set<uint8_t>(L"InVisibility", kSlateHidden) && ue_wrap::Call(slot, f);
}

// puthidden: a container slot hovered and then hidden; the game's em_take only clears such a hover.
void PressHidden(void* ui) {
    void* slot = RD::SlotAt(ui, 0);
    if (!slot || !HoverSlot(ui, slot) || !HideSlot(slot)) {
        CFail("the first container slot could not be hovered and hidden");
        return;
    }
    CV::PutState st{};
    const char* failed = "";
    if (!CV::ReadPutState(ui, st, &failed) || !st.hovered || st.hoverVisible) {
        CFail("the hidden slot reads hovered=%d visible=%d (%s)", st.hovered ? 1 : 0, st.hoverVisible ? 1 : 0, failed);
        return;
    }
    Baseline();
    UE_LOGI("[CI-DRILL] client puthidden presses em_take: a hidden slot is hovered, target holds %d record(s)", g_count0);
    if (!CV::RunPut(ui)) {
        CFail("em_take did not dispatch");
        return;
    }
    CNext(CStep::Quiet);
}

void StepOpenWait() {
    if (CV::Viewed() != g_pack.Get()) {
        if (CExpired(kOpenBoundMs)) CFail("the screen does not show the target %llu s after openPropInv", static_cast<unsigned long long>(kOpenBoundMs / 1000));
        return;
    }
    void* ui = RD::Screen();
    if (!ui) {
        CFail("the inventory screen does not resolve");
        return;
    }
    UE_LOGI("[CI-DRILL] client %s: the screen shows the target", ArmName());
    if (ArmOf() == Arm::PutHidden) PressHidden(ui);
    else PressR2(ui);
}

// The carry walk: every tick the closest approach and the speed are read; the first tick the target is within
// kPressCm with the player above kPressSpeedCmS, hold+run is pressed.
void StepCarry() {
    void* player = coop::players::Registry::Get().Local();
    void* hand = HeldActor(player);
    if (!hand || hand != g_hand.Get()) {
        CFail("the hand lost the %ls on the walk", PutItemClass(ArmOf()));
        return;
    }
    FVector at{};
    if (!E::TryGetActorLocation(player, at)) {
        CFail("the player's place does not read");
        return;
    }
    const FVector vel = E::GetActorVelocity(player);
    const float dist = std::hypot(at.X - g_packAt.X, at.Y - g_packAt.Y);
    const float speed = std::hypot(vel.X, vel.Y);
    if (dist < g_minDist) g_minDist = dist;
    if (speed > g_topSpeed) g_topSpeed = speed;
    if (dist <= kPressCm && speed > kPressSpeedCmS) {
        Baseline();
        if (!RD::UseWithRun(player, g_pack.Get(), hand)) {
            CFail("playerUsedOn did not dispatch");
            return;
        }
        UE_LOGI("[CI-DRILL] client %s presses hold+run: target %.0f cm, speed %.0f cm/s", ArmName(),
                static_cast<double>(dist), static_cast<double>(speed));
        CNext(CStep::ActWait);
        return;
    }
    if (g_walk && g_walk->state.load() != 0)
        CFail("the carry walk ended with no press: closest approach %.0f cm, top speed %.0f cm/s",
              static_cast<double>(g_minDist), static_cast<double>(g_topSpeed));
}

// The put act ends when LastPut names a new id that is no longer only asked.
bool ActEnded() {
    const AR::dev::Put p = AR::dev::LastPut();
    return p.id != g_put0.id && p.outcome != AR::dev::PutOutcome::Asked && p.outcome != AR::dev::PutOutcome::None;
}

void ToSettle() {
    g_endMs = ::GetTickCount64();
    RD::LowerRun();
    WalkStop();
    CNext(CStep::Settle);
}

void StepActWait() {
    const AR::dev::Put p = AR::dev::LastPut();
    if (ActEnded()) {
        ToSettle();
        return;
    }
    if (CExpired(kActBoundMs))
        CFail("no put act ended within %llu s (last put id=%u outcome=%s)", static_cast<unsigned long long>(kActBoundMs / 1000),
              static_cast<unsigned>(p.id), AR::dev::PutOutcomeName(p.outcome));
}

// The act's end and the counts, said and judged once the mirror and the inventory have followed.
void StepSettle() {
    if (::GetTickCount64() - g_endMs < kSettleMs) return;
    const Arm arm = ArmOf();
    const AR::dev::Put p = AR::dev::LastPut();
    const int own = ue_wrap::inventory::LivePersonalStoreCount();
    void* held = HeldActor(coop::players::Registry::Get().Local());
    const wchar_t* cls = PutItemClass(arm);
    const int stray = PutStrayCount(cls, g_pack.Get(), held);   // the drive still in the hand is not a stray
    UE_LOGI("[CI-DRILL] client %s act -> %s route=%u index=%u askHash=%016llx replayHash=%016llx own %d -> %d hand %s", ArmName(),
            AR::dev::PutOutcomeName(p.outcome), static_cast<unsigned>(p.route), static_cast<unsigned>(p.index),
            static_cast<unsigned long long>(p.askHash), static_cast<unsigned long long>(p.replayHash), g_own0, own,
            held ? "held" : "empty");
    if (arm == Arm::PutFood)
        UE_LOGI("[CI-DRILL] client putfood temp %.4f -> %.4f ripeness %.4f -> %.4f clock %.4f -> %.4f hash %016llx -> %016llx "
                "act -> %s",
                static_cast<double>(p.askFloats[0]), static_cast<double>(p.replayFloats[0]),
                static_cast<double>(p.askFloats[1]), static_cast<double>(p.replayFloats[1]),
                static_cast<double>(p.askFloats[2]), static_cast<double>(p.replayFloats[2]),
                static_cast<unsigned long long>(p.askHash),
                static_cast<unsigned long long>(p.replayHash), AR::dev::PutOutcomeName(p.outcome));
    UE_LOGI("[CI-DRILL] client %s stray %ls client=%d", ArmName(), cls, stray);
    if (IsHandArm(arm)) UE_LOGI("[CI-DRILL] client %s carry walk: closest approach %.0f cm, top speed %.0f cm/s", ArmName(),
                                static_cast<double>(g_minDist), static_cast<double>(g_topSpeed));
    if (Put2Judge(PutSettle{p, own, held != nullptr, stray})) return;
    const bool done = p.outcome == AR::dev::PutOutcome::Done;
    const bool placed = IsRecordArm(arm) ? own == g_own0 - 1 : held == nullptr;
    if (done && placed && stray == 0) {
        CPass(stray);
        return;
    }
    if (arm == Arm::PutFood && p.outcome == AR::dev::PutOutcome::Aborted) {
        CFail("the land gate failed: act -> aborted, hash %016llx -> %016llx (values kept above)",
              static_cast<unsigned long long>(p.askHash), static_cast<unsigned long long>(p.replayHash));
        return;
    }
    CFail("act -> %s, own %d -> %d, hand %s, stray %d", AR::dev::PutOutcomeName(p.outcome), g_own0, own,
          held ? "held" : "empty", stray);
}

// puthidden and putnested: no put may be asked. The window runs kSettleMs; a put asked inside it fails the arm at once.
void StepQuiet() {
    const Arm arm = ArmOf();
    const AR::dev::Put p = AR::dev::LastPut();
    if (p.id != g_put0.id || p.outcome != g_put0.outcome || ActsMoved()) {
        CFail("a put was asked (id %u outcome %s, acts %u/%u/%u -> %u/%u/%u)", static_cast<unsigned>(p.id),
              AR::dev::PutOutcomeName(p.outcome), g_acts0[0], g_acts0[1], g_acts0[2], AR::dev::ActsDone(),
              AR::dev::ActsAborted(), AR::dev::ActsRefused());
        return;
    }
    if (!CExpired(kSettleMs)) return;
    const int own = ue_wrap::inventory::LivePersonalStoreCount();
    const int count = CountOf(g_packEid);
    CV::PutState st{};
    const bool read = CV::ReadPutState(RD::Screen(), st);
    const bool line = DC::ChatHasLive(kRefusalText);
    UE_LOGI("[CI-DRILL] client %s: no put asked; hover cleared=%d refusal line=%d own %d -> %d container %d -> %d", ArmName(),
            read && !st.hovered ? 1 : 0, line ? 1 : 0, g_own0, own, g_count0, count);
    const bool judged = arm == Arm::PutHidden ? read && !st.hovered : line;
    if (judged && own == g_own0 && count == g_count0) {
        CPass(-1);
        return;
    }
    CFail("%s, own %d -> %d, container %d -> %d", arm == Arm::PutHidden ? "the hover was not cleared" : "the refusal line was not shown",
          g_own0, own, g_count0, count);
}

}  // namespace

void PutClientTick() {
    PutArmClientKnobs();
    Put2Poll();
    if (Put2Idle()) return;   // a putdepart client relaunched after its park has nothing to do
    if (!g_cMs) CNext(CStep::WaitLine);   // the first step's clock starts when the arm does, never at reset
    if (g_cStep != CStep::Carry && ::GetTickCount64() < g_nextMs) return;   // the carry walk is read every tick
    g_nextMs = ::GetTickCount64() + RD::kCheckEveryMs;
    switch (g_cStep) {
    case CStep::WaitLine:    StepWaitLine(); return;
    case CStep::WaitMirrors: StepWaitMirrors(); return;
    case CStep::Rest:        StepRest(); return;
    case CStep::Pocket:      StepPocket(); return;
    case CStep::PocketWait:  StepPocketWait(); return;
    case CStep::TakeHand:    StepTakeHand(); return;
    case CStep::HoldWait:    StepHoldWait(); return;
    case CStep::HandRead:    StepHandRead(); return;
    case CStep::Walk:        StepWalk(); return;
    case CStep::WalkWait:    StepWalkWait(); return;
    case CStep::OpenWait:    StepOpenWait(); return;
    case CStep::Carry:       StepCarry(); return;
    case CStep::ActWait:     StepActWait(); return;
    case CStep::Settle:      StepSettle(); return;
    case CStep::Quiet:       StepQuiet(); return;
    case CStep::Hook:        Put2Hook(); return;
    }
}

void PutReset() {
    WalkStop();
    RD::LowerRun();
    PutHostReset();
    g_cStep = CStep::WaitLine;
    g_cMs = g_nextMs = g_endMs = 0;
    g_packEid = g_itemEid = g_item2Eid = 0;
    g_needCount = g_pockets = g_phase2 = 0;
    Put2Reset();
    g_pack.Reset();
    g_item.Reset();
    g_item2.Reset();
    g_hand.Reset();
    g_packAt = FVector{};
    g_lastPos[0] = g_lastPos[1] = g_lastPos[2] = FVector{};
    g_rest = 0;
    g_own0 = g_count0 = -1;
    g_put0 = AR::dev::Put{};
    g_acts0[0] = g_acts0[1] = g_acts0[2] = 0;
    g_topSpeed = 0.f;
    g_minDist = 1e9f;
    g_settled = 0;
}

const wchar_t* PutItemClass(Arm a) {
    switch (a) {
    case Arm::PutRecord:
    case Arm::PutHover:
    case Arm::PutReserved:
    case Arm::PutFull:
    case Arm::PutFullHand:
    case Arm::PutDepart:
    case Arm::PutDeadline:
    case Arm::PutOwed:
    case Arm::PutJoin:
    case Arm::PutHand:   return RD::kDriveClass;
    case Arm::PutFood:   return kFoodClass;
    case Arm::PutNested:
    case Arm::PutEmpty:  return kBoxClass;
    default:             return nullptr;
    }
}

int PutStrayCount(const wchar_t* cls, void* target, void* skip) {
    static std::vector<coop::element::Registry::ActorIdPair> pairs;
    FVector at{};
    if (!cls || !target || !E::TryGetActorLocation(target, at)) return -1;
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, pairs);
    int n = 0;
    for (const auto& a : pairs) {
        FVector pos{};
        if (!a.actor || a.actor == target || a.actor == skip || !R::IsLiveByIndex(a.actor, a.internalIdx) ||
            R::ClassNameOf(a.actor) != cls || !E::TryGetActorLocation(a.actor, pos))
            continue;
        const float dx = pos.X - at.X, dy = pos.Y - at.Y, dz = pos.Z - at.Z;
        if (dx * dx + dy * dy + dz * dz <= kStrayCm * kStrayCm) ++n;
    }
    return n;
}


// ---- what put2.cpp and the host's half use of the client's machine ----------------------------------------------------

PutBase PutBaseOf() { return PutBase{g_put0, g_own0, g_count0, g_packEid, g_pack.Get()}; }

void PutFail(const char* fmt, ...) {
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    CFail("%s", buf);
}

void PutPass(int stray) { CPass(stray); }
int PutPhase2() { return g_phase2; }

void PutNotMeasured(const char* why) {
    UE_LOGI("[CI-DRILL] client %s NOT MEASURED (%s)", ArmName(), why);
    CEnd(2, -1);
    Finish();
}
bool PutActEnded() { return ActEnded(); }
void PutToSettle() { ToSettle(); }
void PutEnterHook() { CNext(CStep::Hook); }
bool PutAskR2(void* ui) { return AskR2(ui); }
bool PutHoverSlot(void* ui, void* slot) { return HoverSlot(ui, slot); }

bool PutSelectOwnItem(void* ui, int32_t& sel, char* why, size_t whyLen) {
    sel = -1;
    if (!RD::ClearHover(ui)) {
        std::snprintf(why, whyLen, "the screen's hovered container slot would not clear");
        return false;
    }
    ue_wrap::inventory::LivePersonalStore store;
    if (ue_wrap::inventory::ReadLivePersonalStore(store))
        for (size_t i = store.records.size(); i-- > 0 && sel < 0;)
            if (store.records[i].className == PutItemClass(ArmOf())) sel = static_cast<int32_t>(i);
    const int32_t off = R::FindPropertyOffset(R::ClassOf(ui), L"selected");
    if (sel < 0 || off < 0) {
        std::snprintf(why, whyLen, "the own inventory holds no %ls to select, or the screen has no selection",
                      PutItemClass(ArmOf()));
        return false;
    }
    std::memcpy(static_cast<uint8_t*>(ui) + off, &sel, sizeof(sel));
    RD::CallNoArg(ui, L"updSelect");
    CV::PutState st{};
    const char* failed = "";
    if (!CV::ReadPutState(ui, st, &failed) || st.hovered || !st.takeEnabled || st.index != sel) {
        std::snprintf(why, whyLen, "the screen does not read as a put of item %d (hovered=%d takeEnabled=%d index=%d, %s)",
                      sel, st.hovered ? 1 : 0, st.takeEnabled ? 1 : 0, st.index, failed);
        return false;
    }
    return true;
}

}  // namespace coop::dev::container_intent_drill::detail

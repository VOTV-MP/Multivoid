// coop/dev/dronesack_drill_take.cpp -- the drone sack drill's arm 4 on the client: its take from the sold sack through
// the act, and the watch of its mirror of the sack. See coop/dev/dronesack_drill.h.

#include "dronesack_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/dev/container_pick.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/dev/drill_props.h"
#include "coop/net/protocol.h"
#include "coop/player/players_registry.h"
#include "coop/props/container_act_request.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/actors/inventory.h"
#include "ue_wrap/actors/save_record.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::dronesack_drill {

namespace D  = coop::director;
namespace E  = ue_wrap::engine;
namespace SR = ue_wrap::save_record;
namespace CI = ue_wrap::container_inventory;
namespace CV = ue_wrap::container_view;
namespace AR = coop::props::container_act_request;
namespace CP = coop::dev::container_pick;

namespace detail {
namespace {

std::wstring FirstRecordClass(void* container) {
    void* const inv = CI::InventoryOf(container);
    std::vector<SR::SaveRecord> recs;
    if (!inv || !CI::ReadContents(inv, recs, 4096) || recs.empty()) return L"?";
    return recs[0].className;
}

// A player's take at the sale: the client walks to the sack and opens its screen BEFORE the sale. At the host's sale
// marker it first only watches its mirror (MirrorWatch); if the mirror reads 0 within kMirrorSettleMs the publication
// reached it and there is nothing to press. If the mirror still lists records the client presses the first slot, and the
// host answers Gone. The line the host judges is "<took> <reply>": took 1 when an item came out, reply -1 for nothing
// to take, -2 for a failed drill, else the refusal reason (0 when granted).
constexpr uint64_t kTakeOpenBoundMs = 10000;   // the screen shows the sack's container within this after openPropInv
constexpr uint64_t kTakeAnswerBoundMs = 15000; // the host answers the press within this
constexpr uint64_t kTakeSettleMs = 3000;       // the own inventory and the props are read this long after the answer

void TakeSay(int took, int reply) {
    g_c.took = took;
    g_c.reply = reply;
    g_c.take = Take::Done;
    Say(std::string(kTookPrefix) + std::to_string(took) + " " + std::to_string(reply));
}

void TakeFail(const char* why) {
    UE_LOGW("[SACK-DRILL] client arm 4 FAIL: %s", why);
    TakeSay(0, kReplyFailed);
}

void TakeNothing() {
    UE_LOGI("[SACK-DRILL] client arm 4: nothing to take (the sale's publication came first)");
    g_c.takeOk = true;
    TakeSay(0, kReplyNothing);
}

}  // namespace

// Before the sale the client walks to the sack, so the screen of a take stays open (container_view_close closes one
// out of reach); a route that reaches it, walked within the budget of its length.
void WalkToSack() {
    void* const player = coop::players::Registry::Get().Local();
    if (!player) { TakeFail("the local player is not resolved"); return; }
    ue_wrap::FVector me{}, at{};
    float len = 0.f;
    if (!E::TryGetActorLocation(player, me) || !E::TryGetActorLocation(g_c.sack.Get(), at) ||
        !D::RouteInLegs(player, me, at, CP::kStandCm, &len)) {
        g_c.noRoute = true;
        UE_LOGW("[SACK-DRILL] client arm 4: no route reaches the sack");
        return;
    }
    UE_LOGI("[SACK-DRILL] client arm 4 walks to the sack, route %.0f cm", static_cast<double>(len));
    g_c.walk = D::StartBackgroundWalk(at, CP::kStandCm, CP::WalkSeconds(len));
}

void TakeStart(uint64_t now) {
    if (g_c.noRoute) { TakeFail("no route reaches the sack"); return; }
    if (g_c.walk) {
        const int w = WalkState();
        if (w == 0) return;   // still walking; asked again next tick
        g_c.walk.reset();
        if (w != 1) { TakeFail("the walk to the sack did not arrive"); return; }
    }
    if (!g_c.sackContainer.Get()) g_c.sackContainer.Set(ContainerOf(g_c.sack.Get()));
    void* const container = g_c.sackContainer.Get();
    const int records = container ? RecordsOf(container) : -1;
    if (records < 0) { TakeFail("the sack's container does not read"); return; }
    if (records == 0) return;   // the mirror has not filled yet; asked again next tick, up to the sale's marker
    g_c.mirrorPeak = (std::max)(g_c.mirrorPeak, records);
    if (!CV::Open(container)) { TakeFail("openPropInv did not dispatch"); return; }
    UE_LOGI("[SACK-DRILL] client opens the sack's screen before the sale: it reads %d records, record 0 is '%ls'",
            records, FirstRecordClass(container).c_str());
    g_c.takeMs = now;
    g_c.take = Take::Opening;
}

void TakeTick(uint64_t now) {
    if (g_c.take == Take::Done) return;
    if (g_c.take == Take::Idle) {
        // The sale's marker came and the screen was never opened: there was no screen to press on.
        if (g_c.soldSeenMs)
            TakeFail(g_c.mirrorPeak > 0 ? "the sack's screen was not open at the sale"
                                        : "the mirror never held records before the sale, nothing was measured");
        return;
    }
    void* const container = g_c.sackContainer.Get();
    if (!container) {
        TakeFail("the sack's container is gone");
        return;
    }
    switch (g_c.take) {
    case Take::Opening: {
        if (CV::Viewed() != container) {
            if (now - g_c.takeMs >= kTakeOpenBoundMs) TakeFail("the screen does not show the sack's container");
            return;
        }
        UE_LOGI("[SACK-DRILL] client: the sack's screen is open, waiting for the sale");
        g_c.take = Take::Open;
        return;
    }
    case Take::Open: {
        if (!g_c.soldSeenMs) {   // the screen is held open; nothing is pressed before the host's sale marker
            if (CV::Viewed() != container) TakeFail("the sack's screen closed before the sale");
            return;
        }
        // After the marker the mirror is watched alone for kMirrorSettleMs: a press would make the host answer Gone and
        // send its truth, which would empty the mirror and hide a display that never converged on its own.
        if (g_c.mirror == 0) return;
        if (g_c.mirror == 1) { TakeNothing(); return; }   // it reached 0 by itself: nothing is left to press
        if (CV::Viewed() != container) { TakeFail("the sack's screen closed before the press"); return; }
        void* const slot = CV::FirstSlot(CV::InventoryScreen());
        CV::Slot s{};
        if (!slot) {   // the publication arrived first: its redraw has no slot left to press
            if (RecordsOf(container) == 0) TakeNothing();
            else TakeFail("the open screen has no container slot though the sack still reads records");
            return;
        }
        if (!CV::ReadSlot(slot, s)) { TakeFail("the first slot does not read"); return; }
        g_c.ownBefore = ue_wrap::inventory::LivePersonalStoreCount();
        g_c.propsBefore = coop::dev::drill_props::PropsNearPlayer();
        g_c.takesBefore = AR::dev::LastTake().count;
        const bool pressed = CV::PressSlot(slot);
        UE_LOGI("[SACK-DRILL] client pressed=%d slot id=%d pending id=%u", pressed ? 1 : 0, s.id,
                static_cast<unsigned>(AR::dev::PendingId()));
        if (!pressed) { TakeFail("the slot's pressButton did not dispatch"); return; }
        if (!AR::dev::PendingId() && AR::dev::LastTake().count == g_c.takesBefore) {
            if (RecordsOf(container) == 0) TakeNothing();   // the publication came between the read and the press
            else TakeFail("the press opened no act");
            return;
        }
        g_c.takeMs = now;
        g_c.take = Take::Pressed;
        return;
    }
    case Take::Pressed: {
        const bool committed = AR::dev::LastTake().count != g_c.takesBefore;
        const uint8_t refusal = AR::dev::LastRefusal();
        if (!committed && !refusal) {
            if (now - g_c.takeMs >= kTakeAnswerBoundMs) TakeFail("the host did not answer the press");
            return;
        }
        g_c.reply = refusal;
        g_c.takeMs = now;
        g_c.take = Take::Settling;
        return;
    }
    case Take::Settling: {
        if (now - g_c.takeMs < kTakeSettleMs) return;
        const int own = ue_wrap::inventory::LivePersonalStoreCount();
        const int props = coop::dev::drill_props::PropsNearPlayer();
        const bool granted = AR::dev::LastTake().count != g_c.takesBefore;
        const bool gone = g_c.reply == static_cast<int>(coop::net::ContainerActReason::Gone);
        const bool unchanged = own == g_c.ownBefore && props == g_c.propsBefore;
        UE_LOGI("[SACK-DRILL] client arm 4 answer: granted=%d refusal=%d own %d -> %d props %d -> %d", granted ? 1 : 0,
                g_c.reply, g_c.ownBefore, own, g_c.propsBefore, props);
        if (!granted && gone && unchanged) {
            UE_LOGI("[SACK-DRILL] client arm 4 press: the sold sack's take was refused Gone and nothing came out");
            g_c.takeOk = true;
            TakeSay(0, g_c.reply);
        } else if (granted) {
            UE_LOGW("[SACK-DRILL] client arm 4 FAIL: the host granted a take from the sold sack");
            TakeSay(1, 0);
        } else {
            UE_LOGW("[SACK-DRILL] client arm 4 FAIL: refusal %d, the own inventory %d -> %d, props %d -> %d", g_c.reply,
                    g_c.ownBefore, own, g_c.propsBefore, props);
            TakeSay(0, g_c.reply);
        }
        return;
    }
    case Take::Idle:
    case Take::Done:
        return;
    }
}

namespace {

// The display answer: the client's mirror of the sack reads 0 within kMirrorSettleMs of the sale's marker.
constexpr uint64_t kMirrorSettleMs = 2000;

}  // namespace

void MirrorWatch(uint64_t now) {
    if (g_c.mirror != 0) return;
    const int n = SackRecords(g_c.sack.Get());
    if (g_c.mirrorN < 0) g_c.mirrorN = n;
    g_c.mirrorPeak = (std::max)(g_c.mirrorPeak, n);
    const uint64_t since = now - g_c.soldSeenMs;
    if (n == 0) {
        UE_LOGI("[SACK-DRILL] client arm 4 mirror %d -> 0 after %llu ms", g_c.mirrorN, static_cast<unsigned long long>(since));
        g_c.mirror = 1;
    } else if (since >= kMirrorSettleMs) {
        g_c.mirrorLeft = n;   // said with the verdict, once the press has answered
        g_c.mirror = 2;
    }
}

}  // namespace detail

}  // namespace coop::dev::dronesack_drill

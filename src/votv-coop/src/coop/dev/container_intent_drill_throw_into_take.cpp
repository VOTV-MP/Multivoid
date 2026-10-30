// coop/dev/container_intent_drill_throw_into_take.cpp -- the throwinto arm's preamble of a taken run (n=4, and the first client of n=5). A
// basket the fixture did not arm holds one record; the client names itself to the host (`need`), waits for the basket's mirror (which the host
// placed within the view's reach of the client), walks to the basket only when the view's margin is short, aims at it, takes
// the record through the game's own screen (CV::Open, then CV::PressSlot on the first slot), which is the route that makes the HOST call
// the container's dingus() at the take's final, and reads its mirror's box armed from the publication's flag. The host's half is
// FillBasket: the record that is taken.

#include "container_intent_drill_throw_into.h"
#include "container_intent_drill_throw_into_stand.h"
#include "container_route_drill_detail.h"

#include "coop/dev/container_route_census.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat.h"
#include "coop/player/players_registry.h"
#include "coop/props/container_act_request.h"
#include "coop/props/container_view_close.h"

#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace coop::dev::container_intent_drill::detail::into {
namespace {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace CV = ue_wrap::container_view;
namespace AR = coop::props::container_act_request;
namespace DC = coop::dev::drill_chat;
namespace RD = coop::dev::container_route_drill::detail;
namespace DR = coop::director;
namespace VC = coop::props::container_view_close;

constexpr uint64_t kLineBoundMs    = 120000;  // the need to the host's basket line (its spawn, its settling, its record)
constexpr uint64_t kMirrorBoundMs   = 60000;   // the basket's line to its mirror standing still
constexpr int      kRestChecks      = 4;
constexpr float    kOpenMarginUu    = 20.f;   // the screen opens only with at least this much room left
constexpr int      kTakeWalkDeadlineS = 60;
constexpr uint64_t kRecordBoundMs   = 15000;   // the host's record to the mirror
constexpr uint64_t kViewBoundMs     = 10000;   // openPropInv to the screen showing the basket
constexpr uint64_t kCommitBoundMs   = 15000;   // the slot pressed to the take's commit sent
constexpr uint64_t kTakeArmBoundMs  = 4000;    // the commit sent to the mirror's box reading on: the host's take final, the BP's 0.1 s delay, a sweep, the wire

enum class S : uint8_t { WaitLine, WaitMirror, WaitRecord, Approach, Walk, Open, WaitView, Press, WaitCommit, WaitTook, WaitArmed };
S        g_s = S::WaitLine;
bool     g_taker = true;
uint64_t g_ms = 0;
int      g_r = 0, g_rest = 0;
uint32_t g_eid = 0, g_take0 = 0;
FVector  g_last{}, g_at{};
ue_wrap::CachedObjRef g_basket;
std::shared_ptr<DR::BackgroundWalk> g_walk;

// The room the mod's view close leaves a screen on the basket, in uu: the drill reads the product's own rule, so its stand is judged by
// the rule that closes the screen. Negative: the screen would close.
float ViewMargin(void* basket) { return VC::ViewReachMargin(coop::players::Registry::Get().Local(), basket); }

// The camera turned at the basket, as the player's own look does before the take.
void AimAt(void* player, void* basket) {
    FVector origin{}, ext{};
    if (player && E::GetActorBounds(basket, false, origin, ext)) E::SetControlRotation(E::GetController(player), DR::LookAt(E::GetCameraLocation(), origin));
}

void Go(S s) {
    g_s = s;
    g_ms = ::GetTickCount64();
}
bool Over(uint64_t ms) { return ::GetTickCount64() - g_ms > ms; }

// The screen the take opened is closed; the arm's ClientFail says the rest (a fixture failure before n=5 is held there).
PreState Failed(char* why, size_t len, const char* text) {
    CV::Close();
    std::snprintf(why, len, "%s", text);
    return PreState::Failed;
}

}  // namespace

// The host fills a basket the way a hand puts a drive in: addObject on the container's inventory, no route counted.
bool FillBasket(void* basket) {
    void* cls = R::FindClass(RD::kDriveClass);
    FVector at{};
    if (!cls || !E::TryGetActorLocation(basket, at)) return false;
    coop::dev::container_route_census::Quiet quiet;
    void* item = E::SpawnActor(cls, {at.X, at.Y, at.Z + 90.f});
    const bool took = item && RD::PutIn(basket, item);
    if (item) E::DestroyActor(item);
    return took;
}

void PreBegin(int r, const FVector& at, bool taker) {
    g_r = r;
    g_taker = taker;
    if (taker) {
        char say[64];
        std::snprintf(say, sizeof(say), "at=%d,%d,%d", static_cast<int>(at.X), static_cast<int>(at.Y), static_cast<int>(at.Z));
        DC::Say(Ln("client", "need", r, 1, true) + say);
    }
    Go(S::WaitLine);
}

PreState PreTick(char* why, size_t len, PreOut& out) {
    switch (g_s) {
    case S::WaitLine: {
        std::string rest;
        int pack = 0;
        if (DC::SeenRestOf(Ln("host", "basket", g_r, 1, true), &rest) && RD::IntAfter(rest, "pack", pack) && pack > 0) {
            g_eid = static_cast<uint32_t>(pack);
            g_rest = 0;
            g_last = FVector{};
            Go(S::WaitMirror);
        } else if (Over(kLineBoundMs)) {
            return Failed(why, len, "the host never named the taken run's basket");
        }
        return PreState::Working;
    }
    case S::WaitMirror: {   // the basket's mirror is here and has stood still for kRestChecks checks
        void* pack = coop::element::LivePropActor(g_eid);
        FVector at{};
        if (pack && E::TryGetActorLocation(pack, at)) {
            const bool still = std::fabs(at.X - g_last.X) + std::fabs(at.Y - g_last.Y) + std::fabs(at.Z - g_last.Z) < 1.f;
            g_last = at;
            g_rest = still ? g_rest + 1 : 0;
            if (g_rest >= kRestChecks) {
                g_basket.Set(pack);
                g_at = at;
                UE_LOGI("[CI-DRILL] client %s n=%d: the taken run's basket (eid=%u) is here and at rest, its box reads collision-enabled %d",
                        ArmName(), g_r, g_eid, TriggerState(pack));
                Go(g_taker ? S::WaitRecord : S::WaitTook);
                return PreState::Working;
            }
        }
        if (Over(kMirrorBoundMs)) return Failed(why, len, "the taken run's basket mirror never came to rest");
        return PreState::Working;
    }
    default: break;
    }
    void* basket = g_basket.Get();
    if (!basket) return Failed(why, len, "the basket's mirror died during the take");
    switch (g_s) {
    case S::WaitRecord:   // the host's record reaches the mirror
        if (CountOf(g_eid) >= 1) Go(S::Approach);
        else if (Over(kRecordBoundMs)) return Failed(why, len, "the host's record never reached the basket's mirror");
        return PreState::Working;
    case S::Approach: {   // the basket was placed within the view's reach of this client: a walk only when the margin is short
        void* player = coop::players::Registry::Get().Local();
        FVector me{}, goal{};
        if (!player || !E::TryGetActorLocation(player, me) || !E::TryGetActorLocation(basket, goal))
            return Failed(why, len, "the client's or the basket's location does not read");
        const float margin = ViewMargin(basket);
        UE_LOGI("[CI-DRILL] client %s n=%d: the basket is %.0f cm from the client, view margin %.0f uu", ArmName(), g_r,
                static_cast<double>(std::hypot(goal.X - me.X, goal.Y - me.Y)), static_cast<double>(margin));
        if (margin >= kOpenMarginUu) {   // the view would stay open already: no walk
            Go(S::Open);
            return PreState::Working;
        }
        g_walk = DR::StartBackgroundWalk(goal, kStandReachCm, kTakeWalkDeadlineS, /*carry=*/false);
        UE_LOGI("[CI-DRILL] client %s n=%d: walks to the basket at (%.0f,%.0f,%.0f) to take from it", ArmName(), g_r, static_cast<double>(goal.X),
                static_cast<double>(goal.Y), static_cast<double>(goal.Z));
        Go(S::Walk);
        return PreState::Working;
    }
    case S::Walk: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) return PreState::Working;
        g_walk.reset();
        if (st != 1) return Failed(why, len, "the walk to the basket did not arrive");
        const float margin = ViewMargin(basket);
        if (margin < kOpenMarginUu) {   // arrived, but the view would not stay open
            char text[160];
            std::snprintf(text, sizeof(text), "the view's reach does not hold at the basket (margin %.0f uu)", static_cast<double>(margin));
            return Failed(why, len, text);
        }
        Go(S::Open);
        return PreState::Working;
    }
    case S::Open:   // the take is the game's own: the screen opens on the basket, then the slot is pressed
        AimAt(coop::players::Registry::Get().Local(), basket);
        if (!CV::Open(basket)) return Failed(why, len, "openPropInv did not dispatch on the client's mirror");
        Go(S::WaitView);
        return PreState::Working;
    case S::WaitView:
        if (CV::Viewed() == basket) Go(S::Press);
        else if (Over(kViewBoundMs)) return Failed(why, len, "the screen does not show the basket after openPropInv");
        return PreState::Working;
    case S::Press: {
        void* slot = CV::FirstSlot(CV::InventoryScreen());
        CV::Slot s{};
        if (!slot || !CV::ReadSlot(slot, s)) return Failed(why, len, "the open screen has no readable first container slot");
        g_take0 = AR::dev::LastTake().count;
        const bool pressed = CV::PressSlot(slot);
        UE_LOGI("[CI-DRILL] client %s n=%d: the take is the slot's pressButton (R1) on the opened screen, slot id %d: pressed=%d", ArmName(), g_r,
                s.id, pressed ? 1 : 0);
        if (!pressed) return Failed(why, len, "the slot's pressButton did not dispatch");
        Go(S::WaitCommit);
        return PreState::Working;
    }
    case S::WaitCommit:   // the take's commit is sent: the host's grant, the replay and the commit have run on this peer
        if (AR::dev::LastTake().count != g_take0) {
            if (!AR::dev::LastTake().committed) return Failed(why, len, "the take was aborted");
            CV::Close();
            Go(S::WaitArmed);
        } else if (Over(kCommitBoundMs)) {
            return Failed(why, len, "no take commit after the press (the grant or the ask was refused)");
        }
        return PreState::Working;
    case S::WaitTook:   // the watcher: the taker's take final arms the host's box
        if (DC::SeenHas(Ln("client", "took", g_r, 1, false))) Go(S::WaitArmed);
        else if (Over(kLineBoundMs)) return Failed(why, len, "the first client never said it took the basket's record");
        return PreState::Working;
    case S::WaitArmed: {   // the host's take armed its box; the mirror's box reads it from the publication
        const int state = TriggerState(basket);
        if (state > 0) {
            UE_LOGI("[CI-DRILL] client %s n=%d: the take committed and the mirror's box reads collision-enabled %d (armed by the host's take)",
                    ArmName(), g_r, state);
            if (g_taker) DC::Say(Ln("client", "took", g_r, 1, false));
            out.pack = basket;
            out.eid = g_eid;
            out.at = g_at;
            return PreState::Done;
        }
        if (Over(kTakeArmBoundMs)) {
            char text[160];
            std::snprintf(text, sizeof(text), "the mirror's box did not arm from the host's take (collision enabled %d within %llu ms of the commit)",
                          state, U(kTakeArmBoundMs));
            return Failed(why, len, text);
        }
        return PreState::Working;
    }
    default: break;
    }
    return PreState::Working;
}

void PreReset() {
    if (g_walk && g_walk->state.load() == 0) DR::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
    g_s = S::WaitLine;
    g_ms = 0;
    g_taker = true;
    g_r = g_rest = 0;
    g_eid = g_take0 = 0;
    g_last = g_at = FVector{};
    g_basket.Reset();
}

}  // namespace coop::dev::container_intent_drill::detail::into

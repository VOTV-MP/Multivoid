// coop/dev/hand_record_drill_can.cpp -- the held item's record drill's can arm, both halves; see coop/dev/hand_record_drill.h
// and hand_record_drill_detail.h (the step state and the marks it shares with hand_record_drill.cpp).

#include "hand_record_drill_detail.h"

#include "coop/dev/drill_chat.h"
#include "coop/player/hand_item.h"
#include "coop/net/session.h"
#include "coop/props/prop_act_request.h"     // dev::PendingId: a client's pickup of a shared prop is asked of the host
#include "coop/props/prop_can_state.h"
#include "coop/props/prop_key_index.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace coop::dev::hand_record_drill::detail {
namespace {

namespace D  = coop::dev::hand_record_drill::detail;
namespace DC = coop::dev::drill_chat;
namespace E  = ue_wrap::engine;
namespace PR = ue_wrap::prop;
namespace R  = ue_wrap::reflection;
namespace RV = coop::rig_verdict;

uint32_t g_r0 = 0;                // the host's rebirth counters at the second drop's start
uint32_t g_r1 = 0, g_p0 = 0;      // ... and the records it had published for the can's Key
ue_wrap::CachedObjRef g_before;   // the host's copy of the dropped can, before its pickup

// The live can actor of `key` that is not the hand's.
struct KeyCtx { const std::wstring* key; void* found; };
void* CanByKey(const std::wstring& key) {
    void* cls = ue_wrap::object_index::ClassByName(D::kCanClass);
    if (!cls) return nullptr;
    KeyCtx ctx{&key, nullptr};
    ue_wrap::object_index::ForEachInstance(cls, [](void* c, void* obj, int32_t) {
        auto* k = static_cast<KeyCtx*>(c);
        if (k->found || !obj || !R::IsLive(obj) || R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
        if (PR::GetInteractableKeyString(obj) == *k->key && obj != coop::hand_item::LocalHandActor()) k->found = obj;
    }, &ctx);
    return ctx.found;
}

// The game's own drop of the hand item, as the drop key's call does it: not into the inventory (dontCollect).
bool DropHeld(void* player) {
    void* fn = R::FindDispatchFunctionCached(R::ClassOf(player), L"simulateDrop");
    ue_wrap::ParamFrame f(fn);
    return fn && f.valid() && f.Set<bool>(L"dontCollect", true) && ue_wrap::Call(player, f);
}

// A number the mark carries as `tag<digits>`.
bool ParseNum(const std::string& text, const char* tag, uint32_t& out) {
    const size_t at = text.find(tag);
    if (at == std::string::npos) return false;
    const char* p = text.c_str() + at + std::char_traits<char>::length(tag);
    if (*p < '0' || *p > '9') return false;
    out = static_cast<uint32_t>(std::strtoul(p, nullptr, 10));
    return true;
}

}  // namespace

// can: take, open, eat; drop; the dropped can at rest, marked; eat from it, marked. Then the second birth: pick the
// dropped can up and drop it again, the new actor under the Key this peer already knows, marked at its rest with the
// rebirths counted since the pickup.
void ClientCan(void* player) {
    enum { Arm, Take, Hold, Drop, Find, Rest, Ack1, Eat, Ack2, Pick, PickWait, HeldAck, Redrop, Find2, Rest2, Ack3 };
    namespace PCS = coop::props::prop_can_state;
    switch (g_step) {
    case Arm:
        if (!ClientReady()) return;
        Next(Take);
        return;
    case Take:
        if (!PutCanInHand()) { Abandon("the can could not be taken into the hand"); return; }
        Next(Hold);
        return;
    case Hold: {
        void* can = D::HeldCan();
        if (!can || !coop::hand_item::OwnAnnounced(D::kCanClass)) {
            if (Expired()) Abandon("the hand never held the can, announced");
            return;
        }
        g_key = PR::GetInteractableKeyString(can);
        if (g_key.empty() || g_key == L"None") { Abandon("the can in the hand has no key"); return; }
        if (!PrepareHeldCan(can) || !D::CallOn(can, L"open", player) || !D::CallOn(can, L"eat", player)) {
            Abandon("the can's own open or eat did not run");
            return;
        }
        const D::Can s = D::ReadCan(can);
        if (s.opened != 1 || s.foods < 1) { Abandon("the can in the hand did not open and lose a bite"); return; }
        g_mark = s;
        UE_LOGI("%sclient: the can '%ls' in hand reads %s -- dropping it", kHead, g_key.c_str(), D::Fmt(s).c_str());
        if (!DropHeld(player)) { Abandon("the drop did not dispatch"); return; }
        Next(Find);
        return;
    }
    case Find: {
        void* can = CanByKey(g_key);
        if (!can) { if (Expired()) Abandon("the dropped can never stood in the world under its key"); return; }
        Next(Rest);
        return;
    }
    case Rest: {
        void* can = CanByKey(g_key);
        if (!can) { Abandon("the dropped can left the world"); return; }
        if (!AtRest(can)) { if (Expired()) Abandon("the dropped can did not come to rest"); return; }
        const D::Can s = D::ReadCan(can);
        if (!(s == g_mark)) {
            char why[120];
            std::snprintf(why, sizeof(why), "the dropped can reads %s here, the hand's read %s", D::Fmt(s).c_str(),
                          D::Fmt(g_mark).c_str());
            Fail(why);
            return;
        }
        DC::Say(ClientLine("m1 key=" + std::string(g_key.begin(), g_key.end()) + " " + D::Fmt(s)));
        Next(Ack1);
        return;
    }
    case Ack1:
        if (!DC::SeenHas(HostLine("ack1"))) { if (Expired()) Abandon("the host never answered the first mark"); return; }
        Next(Eat);
        return;
    case Eat: {
        void* can = CanByKey(g_key);
        if (!can) { Abandon("the dropped can left the world"); return; }
        const coop::props::prop_can_state::Counts c0 = coop::props::prop_can_state::CountsNow();
        if (!D::CallOn(can, L"eat", player)) { Abandon("the dropped can's eat did not run"); return; }
        const D::Can s = D::ReadCan(can);
        const coop::props::prop_can_state::Counts c1 = coop::props::prop_can_state::CountsNow();
        if (c1.withheld != c0.withheld) {
            Fail("the dropper's bite of its dropped can was withheld: the can never got its birth's record");
            return;
        }
        g_mark = s;   // the state the can is re-dropped with
        DC::Say(ClientLine("m2 " + D::Fmt(s)));
        Next(Ack2);
        return;
    }
    case Ack2:
        if (!DC::SeenHas(HostLine("ack2"))) { if (Expired()) Abandon("the host never answered the second mark"); return; }
        Next(Pick);
        return;
    case Pick: {
        // A client's pickup of a shared prop is asked of the host: the call may end with a request open, and the hand
        // says what it came to once none is.
        void* can = CanByKey(g_key);
        if (!can) { Abandon("the dropped can left the world before its pickup"); return; }
        bool collected = false;
        if (!E::CallMainPlayerHoldObject(player, can, collected)) { Abandon("the game's pickup did not dispatch"); return; }
        if (!collected && coop::prop_act_request::dev::PendingId() == 0) { Abandon("the game refused the pickup"); return; }
        Next(PickWait);
        return;
    }
    case PickWait: {
        if (coop::prop_act_request::dev::PendingId() != 0) { if (Expired()) Abandon("the host never answered the pickup"); return; }
        void* held = D::HeldCan();
        if (!held || !coop::hand_item::OwnAnnounced(D::kCanClass)) {
            if (Expired()) Abandon("the pickup never put the can in the hand, announced");
            return;
        }
        if (PR::GetInteractableKeyString(held) != g_key) { Abandon("the picked-up can is held under another key"); return; }
        const D::Can s = D::ReadCan(held);
        if (!(s == g_mark)) {
            char why[150];
            std::snprintf(why, sizeof(why), "the picked-up can reads %s, the dropped can read %s", D::Fmt(s).c_str(),
                          D::Fmt(g_mark).c_str());
            Abandon(why);
            return;
        }
        // From here on, the rebirths counted are the drop's.
        g_c0 = PCS::CountsNow().rebirths;
        g_c1 = PCS::CountsNow().rebirthsCarried;
        DC::Say(ClientLine("p1 held"));
        Next(HeldAck);
        return;
    }
    case HeldAck:
        if (!DC::SeenHas(HostLine("ackp1"))) { if (Expired()) Abandon("the host never answered the pickup mark"); return; }
        if (!DropHeld(player)) { Abandon("the second drop did not dispatch"); return; }
        Next(Find2);
        return;
    case Find2:
        if (!CanByKey(g_key)) { if (Expired()) Abandon("the re-dropped can never stood in the world under its key"); return; }
        Next(Rest2);
        return;
    case Rest2: {
        void* can = CanByKey(g_key);
        if (!can) { Abandon("the re-dropped can left the world"); return; }
        if (!AtRest(can)) { if (Expired()) Abandon("the re-dropped can did not come to rest"); return; }
        const D::Can s = D::ReadCan(can);
        const PCS::Counts c = PCS::CountsNow();
        UE_LOGI("%sclient: second birth of '%ls' -- %u new actor(s) under a known key since the pickup, %u born carrying "
                "the known state; reads %s, dropped with %s", kHead, g_key.c_str(), c.rebirths - g_c0,
                c.rebirthsCarried - g_c1, D::Fmt(s).c_str(), D::Fmt(g_mark).c_str());
        DC::Say(ClientLine("m3 " + D::Fmt(s) + " rb=" + std::to_string(c.rebirths - g_c0) +
                           " eq=" + std::to_string(c.rebirthsCarried - g_c1)));
        Next(Ack3);
        return;
    }
    case Ack3:
        if (!DC::SeenHas(HostLine("ack3"))) { if (Expired()) Abandon("the host never answered the third mark"); return; }
        ClientReport("ok", "the three marks answered");
        return;
    default:
        return;
    }
}

// can: the client's mark, the host's copy of the dropped can at rest with a record landed on it, compared; again after
// the client's bite; then the second birth, the can picked up and dropped again, both copies compared to the state it was
// dropped with, the rebirths counted on each peer.
void HostCan() {
    enum { Arm, M1, M2, P1, M3 };
    namespace PCS = coop::props::prop_can_state;
    switch (g_step) {
    case Arm:
        if (!g_s->IsSlotWorldReady(1)) return;
        g_c0 = PCS::CountsNow().landed;
        Next(M1);
        return;
    case M1: {
        std::string rest;
        if (!DC::SeenRestOf(std::string(kC) + "m1 key=", &rest)) { if (Expired()) Abandon("the client's first mark never came"); return; }
        const size_t sp = rest.find(' ');
        std::string keyA = rest.substr(0, sp);
        g_key.assign(keyA.begin(), keyA.end());
        D::Can mark;
        if (sp == std::string::npos || !D::ParseCan(rest.substr(sp), mark)) { Abandon("the client's mark carries no state"); return; }
        void* copy = coop::prop_key_index::Find(g_key);
        if (!copy) { if (Expired()) Abandon("the dropped can never stood on the host under its key"); return; }
        if (!AtRest(copy) || PCS::CountsNow().landed == g_c0) { if (Expired()) Abandon("the host's copy never came to rest with a record landed"); return; }
        const D::Can mine = D::ReadCan(copy);
        if (!(mine == mark)) {
            char why[150];
            std::snprintf(why, sizeof(why), "the host's copy of the dropped can reads %s, the client's reads %s",
                          D::Fmt(mine).c_str(), D::Fmt(mark).c_str());
            HostEnds(RV::Outcome::Fail, why);
            return;
        }
        DC::Say(HostLine("ack1"));
        g_c0 = PCS::CountsNow().landed;
        Next(M2);
        return;
    }
    case M2: {
        std::string rest;
        if (!DC::SeenRestOf(std::string(kC) + "m2 ", &rest)) { if (Expired()) Abandon("the client's second mark never came"); return; }
        D::Can mark;
        if (!D::ParseCan(rest, mark)) { Abandon("the client's mark carries no state"); return; }
        void* copy = coop::prop_key_index::Find(g_key);
        if (!copy) { Abandon("the host's copy of the dropped can left the world"); return; }
        if (PCS::CountsNow().landed == g_c0) { if (Expired()) Abandon("the client's bite never landed on the host's copy"); return; }
        const D::Can mine = D::ReadCan(copy);
        if (!(mine == mark)) {
            char why[150];
            std::snprintf(why, sizeof(why), "after the dropper's bite the host's copy reads %s, the client's reads %s",
                          D::Fmt(mine).c_str(), D::Fmt(mark).c_str());
            HostEnds(RV::Outcome::Fail, why);
            return;
        }
        DC::Say(HostLine("ack2"));
        g_before.Set(copy);
        g_mark = mark;
        Next(P1);
        return;
    }
    case P1: {
        // The client holds the can again (the host's copy is gone with the pickup): the rebirths and publishes counted from
        // here are the second drop's. The client drops once it has this answer.
        if (!DC::SeenHas(std::string(kC) + "p1 held")) { if (Expired()) Abandon("the client's pickup mark never came"); return; }
        g_r0 = PCS::CountsNow().rebirths;
        g_r1 = PCS::CountsNow().rebirthsCarried;
        g_p0 = PCS::PublishedFor(g_key);
        DC::Say(HostLine("ackp1"));
        Next(M3);
        return;
    }
    case M3: {
        // The second birth: a new copy under the key this peer knows, at rest, and the client's mark at its own rest, both
        // compared to the state the can was dropped with; the host publishing for that key meanwhile is a rebirth born with
        // defaults (measured), which a later record would hide. No rebirth counted on a peer: nothing measured there.
        std::string rest;
        if (!DC::SeenRestOf(std::string(kC) + "m3 ", &rest)) { if (Expired()) Abandon("the client's third mark never came"); return; }
        D::Can theirs;
        uint32_t cRb = 0, cEq = 0;
        if (!D::ParseCan(rest, theirs) || !ParseNum(rest, "rb=", cRb) || !ParseNum(rest, "eq=", cEq)) {
            Abandon("the client's third mark carries no state or counter");
            return;
        }
        void* copy = coop::prop_key_index::Find(g_key);
        if (!copy || g_before.Is(copy)) { if (Expired()) Abandon("no new copy of the re-dropped can stood on the host under its key"); return; }
        if (!AtRest(copy)) { if (Expired()) Abandon("the host's new copy never came to rest"); return; }
        const D::Can mine = D::ReadCan(copy);
        const PCS::Counts c = PCS::CountsNow();
        const uint32_t hRb = c.rebirths - g_r0, hEq = c.rebirthsCarried - g_r1, hPub = PCS::PublishedFor(g_key) - g_p0;
        UE_LOGI("%srebirths of the re-dropped can '%ls': host %u new actor(s) under a known key, %u born carrying the known "
                "state, %u record(s) published; client %u, %u; host copy reads %s, client's reads %s, dropped with %s", kHead,
                g_key.c_str(), hRb, hEq, hPub, cRb, cEq, D::Fmt(mine).c_str(), D::Fmt(theirs).c_str(), D::Fmt(g_mark).c_str());
        DC::Say(HostLine("ack3"));
        if (hPub != 0) { HostEnds(RV::Outcome::Fail, "the host published a record for the re-dropped can between its pickup and its rest: its rebirth was born with defaults"); return; }
        if (!(mine == g_mark) || !(theirs == g_mark)) {
            char why[170];
            std::snprintf(why, sizeof(why), "the re-dropped can's host copy reads %s and the client's %s, it was dropped with %s",
                          D::Fmt(mine).c_str(), D::Fmt(theirs).c_str(), D::Fmt(g_mark).c_str());
            HostEnds(RV::Outcome::Fail, why);
            return;
        }
        if (hRb == 0) { HostEnds(RV::Outcome::Invalid, "no actor under a known key was born on the host at the second drop: nothing measured there"); return; }
        if (cRb == 0) { HostEnds(RV::Outcome::Invalid, "no actor under a known key was born on the client at the second drop: nothing measured there"); return; }
        HostEnds(RV::Outcome::Pass, "the re-dropped can's host copy and client copy carried the state it was dropped with");
        return;
    }
    default:
        return;
    }
}


void ResetCan() {
    g_r0 = g_r1 = g_p0 = 0;
    g_before.Set(nullptr);
}

}  // namespace coop::dev::hand_record_drill::detail

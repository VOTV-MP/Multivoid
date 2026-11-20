// coop/dev/hand_record_drill_display.cpp -- the held item's record drill's display arm, both halves; see
// coop/dev/hand_record_drill.h and hand_record_drill_detail.h (the step state it shares with hand_record_drill.cpp).

#include "hand_record_drill_detail.h"

#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_hand.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/hand_record_sync.h"
#include "coop/session/host_world_settle.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine_attach.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace coop::dev::hand_record_drill::detail {
namespace {

namespace DC = coop::dev::drill_chat;
namespace E  = ue_wrap::engine;
namespace HS = coop::hand_record_sync;
namespace R  = ue_wrap::reflection;
namespace RV = coop::rig_verdict;

constexpr uint8_t kHostSlot = 0;

// The client's mirror of the host's hand, once the host's record is written into it and the lane has settled on it.
void* WrittenMirror() {
    void* m = coop::hand_item::MirrorActorForSlot(kHostSlot);
    if (!m || R::ClassNameOf(m) != kDiscClass) return nullptr;
    if (HS::CountsFor(kHostSlot).written == g_c0 || !HS::StoredClassIs(kHostSlot, kDiscClass) || !HS::Settled(kHostSlot)) return nullptr;
    return m;
}

}  // namespace

// display: the client waits for the host's disc record to be written into its mirror, and reads the mirror's root one
// drill tick after: only another write can change what the write left, so a second write before the read starts the wait
// again.
void ClientDisplay() {
    enum { Arm, Written, Read };
    switch (g_step) {
    case Arm:
        if (!ClientReady()) return;
        g_c0 = HS::CountsFor(kHostSlot).written;
        Next(Written);
        return;
    case Written:
        if (!WrittenMirror()) { if (Expired()) Abandon("the host's disc record was never written into the mirror here"); return; }
        g_c1 = HS::CountsFor(kHostSlot).written;
        Next(Read);
        return;
    case Read: {
        void* m = WrittenMirror();
        if (!m || HS::CountsFor(kHostSlot).written != g_c1) { Next(Written); return; }
        const bool sim = E::IsActorRootSimulatingPhysics(m);
        const int coll = E::GetActorRootCollisionEnabled(m);
        if (coll < 0) { Abandon("the mirror's collision could not be read"); return; }
        char say[48];
        std::snprintf(say, sizeof(say), "d1 sim=%d coll=%d", sim ? 1 : 0, coll);
        DC::Say(ClientLine(say));
        ClientReport("ok", "the mirror's display read said");
        return;
    }
    default:
        return;
    }
}

// display: the host takes the disc into its hand, whose loadData turns physics on, so its record carries it to the
// client's mirror; the client's read is the verdict.
void HostDisplay() {
    enum { Arm, Take, Hold, Verdict };
    switch (g_step) {
    case Arm:
        if (!g_s->IsSlotWorldReady(1) || !coop::host_world_settle::Settled()) return;
        Next(Take);
        return;
    case Take:
        if (!coop::dev::drill_hand::TakeIntoHandNow(kDiscRow, kDiscClass)) { Abandon("the disc could not be taken into the hand"); return; }
        Next(Hold);
        return;
    case Hold:
        if (!coop::hand_item::OwnAnnounced(kDiscClass)) { if (Expired()) Abandon("the hand never held the disc, announced"); return; }
        Next(Verdict);
        return;
    case Verdict: {
        std::string rest;
        if (!DC::SeenRestOf(std::string(kC) + "d1 ", &rest)) { if (Expired()) Abandon("the client's display read never came"); return; }
        const size_t s = rest.find("sim="), c = rest.find("coll=");
        if (s == std::string::npos || c == std::string::npos) { Abandon("the client's display read carries no state"); return; }
        const int sim = std::atoi(rest.c_str() + s + 4);
        const int coll = std::atoi(rest.c_str() + c + 5);
        char why[170];
        if (sim != 0 || coll != 0) {
            std::snprintf(why, sizeof(why), "the client's mirror of the host's disc simulates physics=%d, collision=%d after its record was written", sim, coll);
            HostEnds(RV::Outcome::Fail, why);
            return;
        }
        HostEnds(RV::Outcome::Pass, "the client's mirror of the host's disc neither simulates physics nor collides after its record was written");
        return;
    }
    default:
        return;
    }
}

}  // namespace coop::dev::hand_record_drill::detail

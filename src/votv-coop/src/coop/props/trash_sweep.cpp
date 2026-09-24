// coop/props/trash_sweep.cpp -- see coop/props/trash_sweep.h.

#include "coop/props/trash_sweep.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"      // the clump's eid, read back after the carry opened
#include "coop/net/protocol.h"          // TrashClumpPoseSnapshot
#include "coop/net/session.h"           // TrashCarryPoseTurn / PublishTrashCarryPose
#include "coop/props/trash_channel.h"   // OpenBornCarry / OpenPushedCarry / IsCarrying / CtxForEid
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <cstdint>
#include <vector>

namespace coop::trash_sweep {
namespace {

namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

// A clump is held by its object-array index as well as its pointer: it lives across ticks, and a
// dead actor's address can be recycled.
struct Swept {
    void*    clump = nullptr;
    int32_t  idx   = -1;
    uint32_t eid   = 0;
};
std::vector<Swept> g_pending;   // born of a sweep since the last tick, not opened yet
std::vector<Swept> g_pushed;    // pushed by a stroke since the last tick
std::vector<Swept> g_rolling;   // opened, streaming until the latch closes
size_t             g_turn = 0;  // where the next tick's publishing starts, so no roll waits forever

// What the lane did this session, reported at its end.
int g_noted = 0, g_opened = 0, g_reopened = 0, g_ended = 0;

// An id already rolling moves onto the new clump: a rolling clump that re-piled and was swept again
// within one tick is one roll whose actor changed, and the old actor is gone.
void Roll(void* clump, int32_t idx) {
    const coop::element::ElementId eid = coop::element::Registry::Get().EidForActor(clump);
    if (eid == coop::element::kInvalidId) return;
    for (Swept& r : g_rolling) {
        if (r.eid == static_cast<uint32_t>(eid)) { r.clump = clump; r.idx = idx; return; }
    }
    g_rolling.push_back(Swept{clump, idx, static_cast<uint32_t>(eid)});
}

}  // namespace

void NoteSwept(void* clump) {
    if (!clump) return;
    g_pending.push_back(Swept{clump, R::InternalIndexOf(clump), 0});
    ++g_noted;
}

void NotePushed(void* clump) {
    if (!clump) return;
    g_pushed.push_back(Swept{clump, R::InternalIndexOf(clump), 0});
}

void Tick(coop::net::Session& s, void* localPlayer) {
    if (g_pending.empty() && g_pushed.empty() && g_rolling.empty()) return;
    for (const Swept& p : g_pending) {
        if (!R::IsLiveByIndex(p.clump, p.idx)) continue;   // gone before it was placed; its own destroy spoke
        if (coop::trash_channel::OpenBornCarry(s, p.clump, "SWEEP OPEN") != coop::trash_channel::BornCarry::Opened)
            continue;
        Roll(p.clump, p.idx);
        if (++g_opened <= 3)
            UE_LOGI("[TRASH-SWEEP] HOST swept clump %p opened -- its roll streams until it re-piles",
                    p.clump);
    }
    g_pending.clear();
    // A clump the same stroke both made and pushed is carrying already, so only a resting one opens:
    // one still holding its birth certificate -- its grab never reached the hand -- was never a clump
    // anywhere else and opens with its convert, and a tracked one reopens with none.
    for (const Swept& p : g_pushed) {
        if (!R::IsLiveByIndex(p.clump, p.idx)) continue;
        // An unplaceable certificate clump opens neither way: a pushed carry would stream a clump no
        // peer was ever told of.
        const coop::trash_channel::BornCarry born = coop::trash_channel::OpenBornCarry(s, p.clump, "PUSH OPEN");
        if (born == coop::trash_channel::BornCarry::Unplaceable) continue;
        if (born == coop::trash_channel::BornCarry::NoCertificate && !coop::trash_channel::OpenPushedCarry(p.clump))
            continue;
        Roll(p.clump, p.idx);
        ++g_reopened;
    }
    g_pushed.clear();

    // The local hand now, not as the last pump pass saw it: a clump the host grabbed this tick
    // streams on the hand's channel from here, and on this one no longer.
    E::MainPlayerGrabState hand{};
    if (!localPlayer || !E::ReadMainPlayerGrabState(localPlayer, hand)) hand = E::MainPlayerGrabState{};
    for (auto it = g_rolling.begin(); it != g_rolling.end(); ) {
        // The latch closing is the land (or the rest, or the death, which trash_channel reports);
        // a clump that died with a settle pending has re-piled and the commit will say so.
        if (!coop::trash_channel::IsCarrying(static_cast<coop::element::ElementId>(it->eid)) ||
            !R::IsLiveByIndex(it->clump, it->idx) ||
            it->clump == hand.grabbingActor || it->clump == hand.holdingActor) {
            ++g_ended;
            it = g_rolling.erase(it);
        } else {
            ++it;
        }
    }
    const size_t n = g_rolling.size();
    size_t joined = 0;
    for (size_t k = 0; k < n; ++k) {
        const Swept& r = g_rolling[(g_turn + k) % n];
        // A pose read before its turn would be overwritten before any send: two reflected calls
        // for nothing.
        const coop::net::PoseTurn turn = s.TrashCarryPoseTurn(r.eid, /*ahead=*/false);
        if (turn == coop::net::PoseTurn::Wait) continue;
        ue_wrap::FVector loc{};
        ue_wrap::FRotator rot{};
        if (!E::TryGetActorLocation(r.clump, loc) || !E::TryGetActorRotation(r.clump, rot)) continue;   // no pose this tick
        coop::net::TrashClumpPoseSnapshot snap{};
        snap.eid   = r.eid;
        snap.x = loc.X; snap.y = loc.Y; snap.z = loc.Z;
        snap.pitch = ue_wrap::NormalizeAxis(rot.Pitch);
        snap.yaw   = ue_wrap::NormalizeAxis(rot.Yaw);
        snap.roll  = ue_wrap::NormalizeAxis(rot.Roll);
        snap.ctx   = coop::trash_channel::CtxForEid(static_cast<coop::element::ElementId>(r.eid));
        s.PublishTrashCarryPose(snap, /*ahead=*/false);
        if (turn == coop::net::PoseTurn::Join) ++joined;
    }
    g_turn = n ? (g_turn + joined) % n : 0;
}

void OnDisconnect() {
    if (g_noted > 0 || g_reopened > 0)
        UE_LOGI("[TRASH-SWEEP] session tally -- %d swept clump(s) noted, %d opened, %d resting clump(s) "
                "pushed back into a roll, %d rolls ended", g_noted, g_opened, g_reopened, g_ended);
    g_pending.clear();
    g_pushed.clear();
    g_rolling.clear();
    g_turn = 0;
    g_noted = g_opened = g_reopened = g_ended = 0;
}

}  // namespace coop::trash_sweep

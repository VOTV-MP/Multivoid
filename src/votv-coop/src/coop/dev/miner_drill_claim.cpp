// coop/dev/miner_drill_claim.cpp -- the claim arm of the mining rig drill: a client's claim of a rig's points, asked of the host
// (MinerIntent op 3), which runs it and pays its own balance; see coop/dev/miner_drill.h.
//
// The rig holds no GPU, so nothing mines: the host powers it and writes the points. The client waits for its copy to show them,
// then runs the game's claim on its copy. The claim is asked and cancelled there; the host's rig pays the shared balance, and the
// client's copy shows no points at the state the host sends behind its answer.

#include "miner_drill_detail.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/miner_sync.h"

#include "ue_wrap/actors/miner.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::dev::miner_drill::detail {

namespace M  = ue_wrap::miner;
namespace N  = coop::net;
namespace MS = coop::miner_sync;

namespace {

enum HS { H_Start, H_Settle, H_WaitReady, H_WaitCall, H_WaitRan, H_WaitClient };
int g_h = H_Start;
int g_points0 = 0, g_balance0 = 0;
uint32_t g_asks0 = 0, g_ran0 = 0, g_ans0 = 0;

void HostStep(N::Session& s) {
    void* const me = coop::players::Registry::Get().Local();
    if (!me) return;
    void* const rig = g_rig.Get();
    M::Fields hf;
    switch (g_h) {
    case H_Start:
        if (!HostMayStart(s)) return;
        g_rig.Set(SpawnRig(me, 320.f));
        if (!g_rig.Get()) return Invalid("the rig class did not resolve or the world refused the spawn");
        Go(g_h, H_Settle);
        return;
    case H_Settle: {
        if (!rig) return Invalid("the rig died before the drill began");
        g_eid = static_cast<uint32_t>(EidOf(rig));
        if (!g_eid || !RigQuiet(rig)) {
            Late("the rig never got an element id and came to rest");
            return;
        }
        const float d = PuppetDistance(rig);
        if (d < 0.f || d > kReachCm) return Invalid("the client's body is not within reach of the rig");
        if (!M::Read(rig, hf)) return Invalid("the rig does not read");
        hf.powered = true;
        hf.points = kClaimPoints;
        if (!M::Write(rig, hf) || !M::CallPlugged(rig)) return Invalid("the rig could not be powered and given points");
        Fact("rig eid %u has no GPU, is powered and holds %d points | %s", g_eid, kClaimPoints, RigText(rig).c_str());
        Say("rig " + std::to_string(g_eid) + " " + std::to_string(kClaimPoints));
        Go(g_h, H_WaitReady);
        return;
    }
    case H_WaitReady:
        if (!Seen("c-ready ok")) { Late("the client never said its copy shows the points"); return; }
        if (!rig || !M::Read(rig, hf)) return Invalid("the rig is gone");
        if (hf.points != kClaimPoints) return Invalid("the rig's points moved before the claim");
        g_points0 = hf.points;
        g_balance0 = Balance();
        g_asks0 = MS::HostClaimAsks();
        g_ran0 = MS::HostClaimsRan();
        g_ans0 = MS::HostLastAnswer(1).seq;
        if (g_balance0 < 0) return Invalid("the host's balance does not read");
        Say("claim go");
        Go(g_h, H_WaitCall);
        return;
    case H_WaitCall:   // the client's call has returned; its ask, when it was made, went ahead of this line on the ordered lane
        if (!Seen("c-claimed ok")) { Late("the client never ran its claim"); return; }
        if (MS::HostClaimAsks() == g_asks0)
            return Verdict(coop::rig_verdict::Outcome::Fail, "the client's claim was never asked of the host (claim asks %u)", MS::HostClaimAsks());
        Go(g_h, H_WaitRan);
        return;
    case H_WaitRan: {
        const MS::Answer a = MS::HostLastAnswer(1);
        if (a.seq != g_ans0 && a.reason != N::kMinerReasonGranted)
            return Verdict(coop::rig_verdict::Outcome::Fail, "the host refused the claim with reason %u", static_cast<unsigned>(a.reason));
        if (MS::HostClaimsRan() == g_ran0) { Late("the host's claim never ran"); return; }
        const int delta = Balance() - g_balance0;
        Fact("HOST balance +%d (the rig held %d points)", delta, g_points0);
        if (delta != g_points0)
            return Verdict(coop::rig_verdict::Outcome::Fail, "the claim paid %d into the host's balance, the rig held %d", delta, g_points0);
        Say("claim paid");
        Go(g_h, H_WaitClient);
        return;
    }
    case H_WaitClient: {
        std::string r;
        if (!Rest("c-claim ", &r)) { Late("the client's copy never reported its points after the claim"); return; }
        if (r != "ok 0") return Verdict(coop::rig_verdict::Outcome::Fail, "after the claim the client's copy reads: %s", r.c_str());
        if (!rig || !M::Read(rig, hf) || hf.points != 0) return Verdict(coop::rig_verdict::Outcome::Fail, "the host's rig still holds points");
        Verdict(coop::rig_verdict::Outcome::Pass, "the claim was asked, ran on the host once, balance +%d, the client's copy shows 0 points", g_points0);
        return;
    }
    }
}

enum CS { C_WaitRig, C_WaitCopy, C_Go, C_Wait, C_Idle };
int g_c = C_WaitRig;
int g_points = 0;
uint32_t g_ansC = 0, g_seq0 = 0;

void ClientStep(N::Session&) {
    void* const me = coop::players::Registry::Get().Local();
    if (!me) return;
    void* const copy = g_rig.Get();
    M::Fields f;
    switch (g_c) {
    case C_WaitRig: {
        std::string r;
        if (!ClientMayStart()) return;
        if (!Rest("rig ", &r)) { Late("the host never said its rig"); return; }
        long v[2] = {};
        if (Numbers(r, v, 2) != 2) return Invalid("the host's rig line could not be read");
        g_eid = static_cast<uint32_t>(v[0]);
        g_points = static_cast<int>(v[1]);
        Go(g_c, C_WaitCopy);
        return;
    }
    case C_WaitCopy: {
        g_rig.Set(LiveProp(g_eid));
        const MS::Applied ap = MS::LastApplied(g_eid);
        if (!g_rig.Get() || ap.seq == 0 || ap.points != g_points || !ap.powered) {
            Late("the client's copy never showed the host's points and power");
            return;
        }
        Fact("the copy of rig %u shows %d points, powered | %s", g_eid, ap.points, RigText(g_rig.Get()).c_str());
        Say("c-ready ok");
        Go(g_c, C_Go);
        return;
    }
    case C_Go:
        if (!Seen("claim go")) { Late("the host never said the claim"); return; }
        if (!copy) return Invalid("the client's copy is gone");
        g_ansC = MS::ClientLastAnswer().seq;
        g_seq0 = MS::LastApplied(g_eid).seq;
        if (!M::CallClaim(copy, me)) return Invalid("the game's claim did not run on the client's copy");
        Say("c-claimed ok");   // the ask, when the claim made one, left inside the call
        Go(g_c, C_Wait);
        return;
    case C_Wait: {
        const MS::Answer a = MS::ClientLastAnswer();
        const MS::Applied ap = MS::LastApplied(g_eid);
        if (a.seq == g_ansC) { Late("the host never answered the claim"); return; }
        char say[64];
        if (a.reason != N::kMinerReasonGranted) {
            std::snprintf(say, sizeof(say), "c-claim refused %u", static_cast<unsigned>(a.reason));
            Say(say);
            Go(g_c, C_Idle);
            return;
        }
        if (ap.seq == g_seq0 || ap.points != 0 || !copy || !M::Read(copy, f)) { Late("the client's copy never showed the claim's state"); return; }
        std::snprintf(say, sizeof(say), "c-claim ok %d", f.points);
        Say(say);
        Go(g_c, C_Idle);
        return;
    }
    case C_Idle:
        return;
    }
}

}  // namespace

void ClaimHost(N::Session& s) { HostStep(s); }
void ClaimClient(N::Session& s) { ClientStep(s); }

void ClaimReset() {
    g_h = H_Start;
    g_c = C_WaitRig;
    g_points0 = g_balance0 = g_points = 0;
    g_asks0 = g_ran0 = g_ans0 = g_ansC = g_seq0 = 0;
}

}  // namespace coop::dev::miner_drill::detail

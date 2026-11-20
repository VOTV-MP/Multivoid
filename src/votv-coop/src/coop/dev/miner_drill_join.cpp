// coop/dev/miner_drill_join.cpp -- the join arm of the mining rig drill: a rig built before a client joins, shown to the joiner
// by the host's snapshot at its ready edge; see coop/dev/miner_drill.h.
//
// The rig is powered and has two more slots and no GPU or cooler, so nothing mines and no tick state repairs the joiner's copy:
// only the snapshot can make it equal. The joiner decides at the host's state applied to its copy of that rig. When the host's
// snapshot is withheld (the red arm), no state comes: the joiner decides when its copy binds, and the copy differs.

#include "miner_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
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

constexpr int kSlotsBuilt = 2;   // two scrap metals' worth of slots

enum HS { H_Start, H_Settle, H_WaitJoiner, H_WaitReport };
int g_h = H_Start;

void HostStep(N::Session& s) {
    void* const me = coop::players::Registry::Get().Local();
    if (!me) return;
    void* const rig = g_rig.Get();
    M::Fields hf;
    switch (g_h) {
    case H_Start:   // no client is here yet: the rig is built before the join
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
        for (int i = 0; i < kSlotsBuilt; ++i)
            if (!M::CallUpgrade(rig, M::kItemSlot)) return Invalid("the host's rig refused a slot");
        if (!M::Read(rig, hf)) return Invalid("the rig does not read");
        hf.powered = true;
        if (!M::Write(rig, hf) || !M::CallPlugged(rig)) return Invalid("the rig could not be powered");
        Fact("join rig built: eid %u | %s", g_eid, RigText(rig).c_str());
        Go(g_h, H_WaitJoiner);
        return;
    }
    case H_WaitJoiner: {   // the joiner's own wait has no bound of this drill's: the run's budget is its bound
        if (!HostMayStart(s)) return;
        if (!rig || !M::Read(rig, hf)) return Invalid("the rig is gone");
        const bool withheld = coop::config::ResolveFlag(::coop::config_registry::rows::miner_snapshot_off);
        Say("rig " + std::to_string(g_eid) + " " + std::to_string(hf.slots) + " " + std::to_string(hf.powered ? 1 : 0) + " " +
            std::to_string(withheld ? 1 : 0));
        Fact("the joiner is ready; its snapshot is %s", withheld ? "withheld" : "sent");
        Go(g_h, H_WaitReport);
        return;
    }
    case H_WaitReport: {
        std::string r;
        if (!Rest("c-join ", &r)) { Late("the joiner never decided"); return; }
        if (MS::SnapshotEidless() != 0)
            return Verdict(coop::rig_verdict::Outcome::Fail, "the snapshot skipped %u rigs with no element id", MS::SnapshotEidless());
        if (r != "ok") return Verdict(coop::rig_verdict::Outcome::Fail, "the joiner's copy of rig %u differs: %s", g_eid, r.c_str());
        Verdict(coop::rig_verdict::Outcome::Pass, "the joiner's copy of the rig built before its join shows the host's counts and power");
        return;
    }
    }
}

enum CS { C_WaitRig, C_WaitCopy, C_Idle };
int g_c = C_WaitRig;
int g_slots = 0, g_powered = 0, g_withheld = 0;

void ClientStep(N::Session&) {
    switch (g_c) {
    case C_WaitRig: {
        std::string r;
        if (!ClientMayStart()) return;
        if (!Rest("rig ", &r)) { Late("the host never said its rig"); return; }
        long v[4] = {};
        if (Numbers(r, v, 4) != 4) return Invalid("the host's rig line could not be read");
        g_eid = static_cast<uint32_t>(v[0]);
        g_slots = static_cast<int>(v[1]);
        g_powered = static_cast<int>(v[2]);
        g_withheld = static_cast<int>(v[3]);
        Go(g_c, C_WaitCopy);
        return;
    }
    case C_WaitCopy: {
        int gpus = 0, slots = 0, fans = 0, powered = 0;
        if (g_withheld == 0) {
            const MS::Applied ap = MS::LastApplied(g_eid);   // the marker: the host's state applied to this eid
            if (ap.seq == 0) { Late("the host's state of the rig never reached the joiner"); return; }
            gpus = ap.gpus;
            slots = ap.slots;
            fans = ap.fans;
            powered = ap.powered ? 1 : 0;
        } else {   // nothing is sent: the copy's bind is the point after which nothing can come
            g_rig.Set(LiveProp(g_eid));
            M::Fields f;
            if (!g_rig.Get() || !M::Read(g_rig.Get(), f)) { Late("the joiner's copy of the rig never bound"); return; }
            Fact("the copy of rig %u is bound | %s", g_eid, RigText(g_rig.Get()).c_str());
            gpus = f.gpus;
            slots = f.slots;
            fans = f.fans;
            powered = f.powered ? 1 : 0;
        }
        char say[100];
        if (gpus == 0 && fans == 0 && slots == g_slots && powered == g_powered) std::snprintf(say, sizeof(say), "c-join ok");
        else std::snprintf(say, sizeof(say), "c-join %d GPUs, %d slots, %d coolers, powered %d; host %d slots, powered %d", gpus, slots, fans, powered, g_slots, g_powered);
        Fact("join: %s", say);
        Say(say);
        Go(g_c, C_Idle);
        return;
    }
    case C_Idle:
        return;
    }
}

}  // namespace

void JoinHost(N::Session& s) { HostStep(s); }
void JoinClient(N::Session& s) { ClientStep(s); }

void JoinReset() {
    g_h = H_Start;
    g_c = C_WaitRig;
    g_slots = g_powered = g_withheld = 0;
}

}  // namespace coop::dev::miner_drill::detail

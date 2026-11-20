// coop/dev/miner_drill_record.cpp -- the record arm of the mining rig drill: the class gate on a client's save record of a rig
// (coop/props/miner_record.cpp); see coop/dev/miner_drill.h.
//
// Leg 1, forged: the client writes a GPU count on its copy of the host's rig, which it never held, and publishes the copy's
// record itself; the record and the chat line behind it ride one ordered lane, so the host decides at that line. The gate refuses
// the record, the host's count stays and its state goes back to the client, whose copy shows it. Leg 2, an honest drop: the
// client takes a rig with counts into its hand and throws it with the game's own verb; the record its drop owes lands, and the
// host's rig shows the same counts.

#include "miner_drill_detail.h"
#include "drive_drill_verbs.h"

#include "coop/dev/drill_hand.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/props/miner_sync.h"
#include "coop/props/prop_key_index.h"
#include "coop/props/prop_save_data.h"

#include "ue_wrap/actors/miner.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace coop::dev::miner_drill::detail {

namespace M  = ue_wrap::miner;
namespace N  = coop::net;
namespace MS = coop::miner_sync;
namespace HB = coop::hand_item;

namespace {

constexpr int kForgedGpus = 9;   // inside the bounds, so only the gate's first rule can refuse it
constexpr int kDropGpus = 2, kDropSlots = 3, kDropFans = 1;   // the counts the honest drop carries
constexpr const wchar_t* kDropRow = L"miner";
constexpr const wchar_t* kDropCls = L"prop_miner_C";

// ---- the host ----
enum HS { H_Start, H_Settle, H_WaitReady, H_L1Sent, H_L1Client, H_L2 };
int g_h = H_Start;
int g_gpus0 = 0;
uint32_t g_refused0 = 0, g_applied0 = 0;

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
        if (!M::CallUpgrade(rig, M::kItemSlot) || !M::CallUpgrade(rig, M::kItemSlot) || !M::CallUpgrade(rig, M::kItemGpu))
            return Invalid("the host's rig refused its first slots and GPU");
        if (!M::Read(rig, hf)) return Invalid("the rig does not read");
        g_gpus0 = hf.gpus;
        Fact("rig eid %u built | %s", g_eid, RigText(rig).c_str());
        Say("rig " + std::to_string(g_eid) + " " + std::to_string(g_gpus0));
        Go(g_h, H_WaitReady);
        return;
    }
    case H_WaitReady:
        if (!Seen("c-ready ok")) { Late("the client never said its copy shows the rig's counts"); return; }
        g_refused0 = MS::RecordsRefused();
        Say("r1 go");
        Go(g_h, H_L1Sent);
        return;
    case H_L1Sent: {
        std::string key;
        if (!Rest("record sent ", &key)) { Late("the client never published its forged record"); return; }
        if (!rig || !M::Read(rig, hf)) return Invalid("the rig is gone");
        // The record came ahead of the client's line on the ordered lane: a gate that refused it has said so by now, and a record
        // nothing refused has landed or waits in the park.
        if (hf.gpus == kForgedGpus)
            return Verdict(coop::rig_verdict::Outcome::Fail, "leg 1: the client's forged record landed, the host's rig holds %d GPUs", hf.gpus);
        if (MS::RecordsRefused() == g_refused0) { Late("the forged record was neither refused nor landed"); return; }
        if (hf.gpus != g_gpus0)
            return Verdict(coop::rig_verdict::Outcome::Fail, "leg 1: the gate refused the record but the host's GPUs went %d -> %d", g_gpus0, hf.gpus);
        Fact("leg 1: the gate refused the forged record %s; the host's rig holds %d GPUs", key.c_str(), hf.gpus);
        Say("r1 refused " + std::to_string(g_gpus0));
        Go(g_h, H_L1Client);
        return;
    }
    case H_L1Client: {
        std::string r;
        if (!Rest("c-r1 ", &r)) { Late("the client never reported leg 1"); return; }
        if (r != "ok") return Verdict(coop::rig_verdict::Outcome::Fail, "leg 1: after the refusal the client's copy %s", r.c_str());
        g_refused0 = MS::RecordsRefused();
        g_applied0 = MS::RecordsApplied();
        Say("r2 go");
        Go(g_h, H_L2);
        return;
    }
    case H_L2: {
        std::string r;
        if (!Rest("drop ", &r)) { Late("the client never named the rig it dropped"); return; }
        const size_t space = r.find(' ');
        long v[3] = {};
        if (space == std::string::npos || Numbers(r.substr(space + 1), v, 3) != 3) return Invalid("the client's drop line could not be read");
        const std::string key = r.substr(0, space);
        const int g = static_cast<int>(v[0]), sl = static_cast<int>(v[1]), fa = static_cast<int>(v[2]);
        if (MS::RecordsRefused() != g_refused0)
            return Verdict(coop::rig_verdict::Outcome::Fail, "leg 2: the gate refused the record of an honest drop (%u refused)", MS::RecordsRefused());
        void* const dropped = coop::prop_key_index::Find(Widen(key));
        if (!dropped) { Late("the host never had the dropped rig"); return; }
        if (MS::RecordsApplied() == g_applied0) { Late("the dropped rig's record never showed on the host's rig"); return; }
        if (!M::Read(dropped, hf)) return Invalid("the host's dropped rig does not read");
        Fact("leg 2: the host's copy of the drop %s | %s", key.c_str(), RigText(dropped).c_str());
        if (hf.gpus != g || hf.slots != sl || hf.fans != fa)
            return Verdict(coop::rig_verdict::Outcome::Fail, "leg 2: the host's rig holds %d/%d/%d (GPUs/slots/coolers), the client dropped %d/%d/%d",
                           hf.gpus, hf.slots, hf.fans, g, sl, fa);
        Verdict(coop::rig_verdict::Outcome::Pass, "a forged record of a world rig refused, the record of an honest drop landed with %d/%d/%d", g, sl, fa);
        return;
    }
    }
}

// ---- the client ----
enum CS { C_WaitRig, C_WaitCopy, C_R1, C_WaitRefused, C_R2, C_HeldRig, C_Find, C_Key, C_Idle };
int g_c = C_WaitRig;
int g_hostGpus = 0;
uint32_t g_seq0 = 0;
ue_wrap::CachedObjRef g_held, g_thrown;
std::vector<void*> g_base;

void CollectRig(void* ctx, void* rig) { static_cast<std::vector<void*>*>(ctx)->push_back(rig); }

void ClientStep(N::Session& s) {
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
        g_hostGpus = static_cast<int>(v[1]);
        Go(g_c, C_WaitCopy);
        return;
    }
    case C_WaitCopy: {
        g_rig.Set(LiveProp(g_eid));
        void* const c = g_rig.Get();
        if (!c || MS::LastApplied(g_eid).seq == 0 || !M::Read(c, f) || f.gpus != g_hostGpus) {
            Late("the client's copy never showed the host's GPUs");
            return;
        }
        Say("c-ready ok");
        Go(g_c, C_R1);
        return;
    }
    case C_R1: {
        if (!Seen("r1 go")) { Late("the host never said leg 1"); return; }
        if (!copy || !M::Read(copy, f)) return Invalid("the client's copy is gone");
        const std::wstring key = ue_wrap::prop::GetKeyString(copy);
        if (key.empty()) return Invalid("the client's copy has no key");
        g_seq0 = MS::LastApplied(g_eid).seq;
        f.gpus = kForgedGpus;
        if (!M::Write(copy, f)) return Invalid("the client's copy refused the forged count");
        if (!coop::prop_save_data::Publish(&s, copy, key)) return Invalid("the forged record was not published");
        Say("record sent " + Narrow(key));
        Go(g_c, C_WaitRefused);
        return;
    }
    case C_WaitRefused: {
        std::string r;
        if (!Rest("r1 refused ", &r)) { Late("the host never said it refused the forged record"); return; }
        const int host = std::atoi(r.c_str());
        if (!copy || !M::Read(copy, f)) return Invalid("the client's copy is gone");
        // The host's state went back behind the refusal, ahead of this line on the ordered lane.
        const bool back = MS::LastApplied(g_eid).seq != g_seq0 && f.gpus == host;
        char say[80];
        if (back) std::snprintf(say, sizeof(say), "c-r1 ok");
        else std::snprintf(say, sizeof(say), "kept %d GPUs, host %d, states applied since: %d", f.gpus, host, MS::LastApplied(g_eid).seq != g_seq0 ? 1 : 0);
        Say(say);
        Go(g_c, C_R2);
        return;
    }
    case C_R2:
        if (!Seen("r2 go")) { Late("the host never said leg 2"); return; }
        if (!coop::dev::drill_hand::TakeIntoHandNow(kDropRow, kDropCls)) return Invalid("the client's hand could not take a rig");
        Go(g_c, C_HeldRig);
        return;
    case C_HeldRig: {
        void* const held = HB::LocalHandActor();
        if (!held || !M::IsMiner(held) || !HB::OwnAnnounced(kDropCls)) { Late("the hand never held an announced rig"); return; }
        M::Fields counts;
        counts.gpus = kDropGpus;
        counts.slots = kDropSlots;
        counts.fans = kDropFans;
        if (!M::Write(held, counts)) return Invalid("the held rig refused its counts");
        g_held.Set(held);
        g_base.clear();
        M::ForEachRig(&CollectRig, &g_base);
        if (!coop::dev::drive_drill_verbs::Throw(me)) return Invalid("throwHoldingProp did not run");
        Go(g_c, C_Find);
        return;
    }
    case C_Find: {
        std::vector<void*> now;
        M::ForEachRig(&CollectRig, &now);
        void* thrown = nullptr;
        for (void* r : now)
            if (std::find(g_base.begin(), g_base.end(), r) == g_base.end()) thrown = r;
        // The game may let the held actor go into the world as it is, in which case it is the one the baseline already has.
        void* const held = g_held.Get();
        if (!thrown && held && !HB::IsLocalHandViewActor(held)) thrown = held;
        if (!thrown) { Late("the throw left no rig in the world"); return; }
        g_thrown.Set(thrown);
        Go(g_c, C_Key);
        return;
    }
    case C_Key: {
        void* const t = g_thrown.Get();
        if (!t || !M::Read(t, f)) return Invalid("the thrown rig is gone or does not read");
        const std::wstring key = ue_wrap::prop::GetKeyString(t);
        if (key.empty()) { Late("the thrown rig never had a key"); return; }
        if (f.gpus != kDropGpus || f.slots != kDropSlots || f.fans != kDropFans) return Invalid("the thrown rig lost its counts");
        Fact("the client threw a rig %ls | %s", key.c_str(), RigText(t).c_str());
        Say("drop " + Narrow(key) + " " + std::to_string(f.gpus) + " " + std::to_string(f.slots) + " " + std::to_string(f.fans));
        Go(g_c, C_Idle);
        return;
    }
    case C_Idle:
        return;
    }
}

}  // namespace

void RecordHost(N::Session& s) { HostStep(s); }
void RecordClient(N::Session& s) { ClientStep(s); }

void RecordReset() {
    g_h = H_Start;
    g_c = C_WaitRig;
    g_gpus0 = g_hostGpus = 0;
    g_refused0 = g_applied0 = g_seq0 = 0;
    g_held.Reset();
    g_thrown.Reset();
    g_base.clear();
}

}  // namespace coop::dev::miner_drill::detail

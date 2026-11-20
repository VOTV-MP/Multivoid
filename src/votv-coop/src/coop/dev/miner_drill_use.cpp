// coop/dev/miner_drill_use.cpp -- the use arm of the mining rig drill: a client's use of an item on a rig, judged by the host
// (MinerIntent op 2); see coop/dev/miner_drill.h.
//
// Leg 1, the honest use: the client takes a GPU and runs the game's use on its copy, with a blocking hit; the host grants it,
// and both peers' GPU counts rise by one and the client's hand is empty. Leg 2, forged: with an empty hand the client raises its
// copy's GPU count and sends the op itself; the host refuses it as "no hold" and puts its own count back on the copy. Leg 4, the
// refund: the host builds a second rig with a GPU in every slot; the client holds a real GPU and runs the game's use on its copy
// of it (written one GPU short, as a use that raced the rig's last GPU sees it), which spends the item and asks; the host
// refuses (the rig is full, reason 6), gives the GPU back beside the client, and the client looks for a GPU it did not have.
// Leg 3, the flood: the client sends kFloodAsks ops in one tick, each behind a hand state of its own, so every ask has a hold
// behind it and only the host's bucket limits them; the host grants its burst. The flood runs after leg 4 because its refused
// asks spend the slot's refund budget, which leg 4's refund needs.

#include "miner_drill_detail.h"

#include "coop/dev/drill_hand.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"
#include "coop/props/miner_sync.h"

#include "ue_wrap/actors/miner.h"
#include "ue_wrap/core/log.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::miner_drill::detail {

namespace M  = ue_wrap::miner;
namespace N  = coop::net;
namespace MS = coop::miner_sync;
namespace HB = coop::hand_item;

namespace {

// The hand state a forged ask rides behind: the wire layout of HandItem (slot, has, the class, the name and the item as
// length-prefixed strings, then the six floats of the hold transform), the class and item the real hand announces for a GPU.
std::vector<uint8_t> HandState(uint8_t slot, const std::string& name) {
    const std::string cls = Narrow(M::ItemClass(M::kItemGpu)), item = Narrow(M::ItemHoldName(M::kItemGpu));
    std::vector<uint8_t> out{slot, 1};
    for (const std::string* t : {&cls, &name, &item}) {
        out.push_back(static_cast<uint8_t>(t->size()));
        out.insert(out.end(), t->begin(), t->end());
    }
    const float rel[6] = {45.f, 10.f, -40.f, 0.f, 0.f, 0.f};
    const size_t base = out.size();
    out.resize(base + sizeof(rel));
    std::memcpy(out.data() + base, rel, sizeof(rel));
    return out;
}

std::vector<uint8_t> EmptyHand(uint8_t slot) { return {slot, 0, 0, 0, 0}; }

bool SendHand(N::Session& s, const std::vector<uint8_t>& p) {
    return s.SendReliableToSlot(0, N::ReliableKind::HandItem, p.data(), static_cast<int>(p.size()));
}

bool SendAsk(N::Session& s, uint32_t eid) {
    N::MinerPayload p{};
    p.eid = eid;
    p.op = N::kMinerOpAdd;
    p.item = N::kMinerItemGpu;
    return s.SendReliable(N::ReliableKind::MinerIntent, &p, sizeof(p));
}

// ---- the host ----
enum HS { H_Start, H_Settle, H_WaitClient, H_L1, H_L2Hand, H_L2Sent, H_L2Judge, H_L4Spawn, H_L4Settle, H_L4Sent, H_L4Judge,
          H_L3Hand, H_L3 };
int g_h = H_Start;
int g_gpu0 = 0;
uint32_t g_ans0 = 0, g_granted0 = 0, g_judged0 = 0, g_refunds0 = 0;
ue_wrap::CachedObjRef g_full;   // leg 4's rig, the host's own
uint32_t g_fullEid = 0;

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
        for (int i = 0; i < kSlots; ++i)
            if (!M::CallUpgrade(rig, M::kItemSlot)) return Invalid("the host's rig refused a slot");
        Fact("rig eid %u raised to %d slots, %.0f cm from the client | %s", g_eid, kSlots, static_cast<double>(d), RigText(rig).c_str());
        Say("rig " + std::to_string(g_eid) + " " + std::to_string(kSlots));
        Go(g_h, H_WaitClient);
        return;
    }
    case H_WaitClient:
        if (!Seen("c-ready ok")) { Late("the client never said its copy shows the slots"); return; }
        if (!rig || !M::Read(rig, hf)) return Invalid("the rig is gone");
        g_gpu0 = hf.gpus;
        Say("l1 go");
        Go(g_h, H_L1);
        return;
    case H_L1: {
        std::string r;
        if (!Rest("c-l1 ", &r)) { Late("the client never reported leg 1"); return; }
        long v[4] = {};
        if (Numbers(r, v, 4) != 4)
            return Verdict(coop::rig_verdict::Outcome::Fail, "leg 1: the client's use was not granted (%s)", r.c_str());
        const unsigned reason = static_cast<unsigned>(v[0]);
        const int copy = static_cast<int>(v[1]), applied = static_cast<int>(v[2]), empty = static_cast<int>(v[3]);
        if (!rig || !M::Read(rig, hf)) return Invalid("the rig is gone");
        const MS::Answer a = MS::HostLastAnswer(1);
        const bool ok = reason == 0 && a.op == N::kMinerOpAdd && a.item == N::kMinerItemGpu && a.reason == 0 && MS::HostUsesGranted() == 1 &&
                        hf.gpus == g_gpu0 + 1 && copy == g_gpu0 + 1 && applied == g_gpu0 + 1 && empty == 1;
        if (!ok)
            return Verdict(coop::rig_verdict::Outcome::Fail,
                           "leg 1: host GPUs %d (was %d), the client's copy %d, its applied state %d, hand empty %d, uses granted %u, "
                           "answer reason %u",
                           hf.gpus, g_gpu0, copy, applied, empty, MS::HostUsesGranted(), static_cast<unsigned>(a.reason));
        Fact("leg 1 holds: both peers' GPUs %d -> %d, the client's hand empty | %s", g_gpu0, hf.gpus, RigText(rig).c_str());
        g_gpu0 = hf.gpus;
        Go(g_h, H_L2Hand);
        return;
    }
    case H_L2Hand:   // the client's emptied hand has reached the hand lane's record
        if (HB::HoldsItem(1)) { Late("the hand lane never recorded the client's hand as empty"); return; }
        g_ans0 = MS::HostLastAnswer(1).seq;
        g_granted0 = MS::HostUsesGranted();
        Say("l2 " + std::to_string(g_gpu0));
        Go(g_h, H_L2Sent);
        return;
    case H_L2Sent: {
        if (!Seen("c-l2sent ok")) { Late("the client never sent its forged use"); return; }
        const MS::Answer a = MS::HostLastAnswer(1);
        if (a.seq == g_ans0) return Verdict(coop::rig_verdict::Outcome::Fail, "leg 2: the forged use was never answered");
        if (a.reason == N::kMinerReasonGranted || MS::HostUsesGranted() != g_granted0)
            return Verdict(coop::rig_verdict::Outcome::Fail, "leg 2: the forged use with an empty hand was granted (reason %u)",
                           static_cast<unsigned>(a.reason));
        if (a.reason != N::kMinerReasonNoHold)
            return Verdict(coop::rig_verdict::Outcome::Fail, "leg 2: the forged use was refused with reason %u, not 1 (no hold)",
                           static_cast<unsigned>(a.reason));
        // The state owed behind the refusal leaves at the host's next tick, ahead of this line on the ordered lane.
        if (MS::HostOwesState(1)) { Late("the host never sent the state owed behind the refusal"); return; }
        Say("l2judged ok");
        Go(g_h, H_L2Judge);
        return;
    }
    case H_L2Judge: {
        std::string r;
        if (!Rest("c-l2 ", &r)) { Late("the client never reported leg 2"); return; }
        if (r != "ok") return Verdict(coop::rig_verdict::Outcome::Fail, "leg 2: after the refusal the client's copy %s", r.c_str());
        Fact("leg 2 holds: refused as no hold, and the client's copy shows the host's count again");
        Go(g_h, H_L4Spawn);
        return;
    }
    case H_L4Spawn:
        g_full.Set(SpawnRig(me, 320.f, 150.f));
        if (!g_full.Get()) return Invalid("the second rig did not spawn");
        Go(g_h, H_L4Settle);
        return;
    case H_L4Settle: {
        void* const full = g_full.Get();
        if (!full) return Invalid("the second rig died before leg 4");
        g_fullEid = static_cast<uint32_t>(EidOf(full));
        if (!g_fullEid || !RigQuiet(full)) {
            Late("the second rig never got an element id and came to rest");
            return;
        }
        const float d = PuppetDistance(full);
        if (d < 0.f || d > kReachCm) return Invalid("the client's body is not within reach of the second rig");
        for (int i = 0; i < kSlots; ++i)
            if (!M::CallUpgrade(full, M::kItemSlot)) return Invalid("the host's second rig refused a slot");
        for (int i = 0; i < kSlots; ++i)
            if (!M::CallUpgrade(full, M::kItemGpu)) return Invalid("the host's second rig refused a GPU");
        g_ans0 = MS::HostLastAnswer(1).seq;
        g_refunds0 = MS::HostRefunds();
        Fact("rig eid %u is full, %.0f cm from the client | %s", g_fullEid, static_cast<double>(d), RigText(full).c_str());
        Say("l4 " + std::to_string(g_fullEid));
        Go(g_h, H_L4Sent);
        return;
    }
    case H_L4Sent: {   // the client's use has run on its copy; its ask went ahead of this line on the ordered lane
        if (!Seen("c-l4sent ok")) { Late("the client never ran its use on the full rig"); return; }
        const MS::Answer a = MS::HostLastAnswer(1);
        if (a.seq == g_ans0) return Verdict(coop::rig_verdict::Outcome::Fail, "leg 4: the use on the full rig was never answered");
        if (a.reason != N::kMinerReasonGameRefused)
            return Verdict(coop::rig_verdict::Outcome::Fail, "leg 4: the use on the full rig was answered with reason %u, not 6",
                           static_cast<unsigned>(a.reason));
        // The refund is made inside the judging of the ask, which is over by now: what it did is final.
        Say("l4judged " + std::to_string(MS::HostRefunds() - g_refunds0));
        Go(g_h, H_L4Judge);
        return;
    }
    case H_L4Judge: {
        std::string r;
        if (!Rest("c-l4 ", &r)) { Late("the client never reported its look for the refund"); return; }
        if (r != "found")
            return Verdict(coop::rig_verdict::Outcome::Fail, "leg 4: the refused GPU was not given back (the host spawned %u), the client %s",
                           MS::HostRefunds() - g_refunds0, r.c_str());
        Fact("leg 4 holds: the full rig refused the use and the GPU came back beside the client");
        Go(g_h, H_L3Hand);
        return;
    }
    case H_L3Hand:   // the client's emptied hand has reached the hand lane's record
        if (HB::HoldsItem(1)) { Late("the hand lane never recorded the client's hand as empty after leg 4"); return; }
        g_granted0 = MS::HostUsesGranted();
        g_judged0 = MS::HostUsesJudged(1);
        Say("l3 go");
        Go(g_h, H_L3);
        return;
    case H_L3: {
        // A dropped ask is not answered, so the judging itself is the event: every ask of the flood has been decided.
        if (MS::HostUsesJudged(1) - g_judged0 < static_cast<uint32_t>(kFloodAsks)) { Late("the host never judged all the asks of the flood"); return; }
        const uint32_t granted = MS::HostUsesGranted() - g_granted0;
        if (!rig || !M::Read(rig, hf)) return Invalid("the rig is gone");
        Fact("leg 3: %d asks, %u granted | %s", kFloodAsks, granted, RigText(rig).c_str());
        if (granted != static_cast<uint32_t>(kBurst))
            return Verdict(coop::rig_verdict::Outcome::Fail, "leg 3: the flood of %d asks was granted %u times, the burst is %d",
                           kFloodAsks, granted, kBurst);
        Verdict(coop::rig_verdict::Outcome::Pass,
                "use granted with a hold, forged use refused with the host's count put back, a refused GPU given back, flood of %d "
                "granted %d", kFloodAsks, kBurst);
        return;
    }
    }
}

// ---- the client ----
enum CS { C_WaitRig, C_WaitCopy, C_L1Go, C_L1Take, C_L1Use, C_L1Wait, C_L2, C_L2Judged, C_L4Rig, C_L4Copy, C_L4Take, C_L4Use,
          C_L4Judged, C_L3, C_Idle };
int g_c = C_WaitRig;
int g_slots = 0, g_hostGpu = 0;
uint32_t g_ansC = 0, g_seq0 = 0;
ue_wrap::CachedObjRef g_copyFull;      // leg 4: the client's copy of the host's full rig
uint32_t g_fullEidC = 0;
std::vector<uint64_t> g_gpusBefore;    // leg 4: the GPUs beside the client before its use

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
        g_slots = static_cast<int>(v[1]);
        Go(g_c, C_WaitCopy);
        return;
    }
    case C_WaitCopy: {
        g_rig.Set(LiveProp(g_eid));
        void* const c = g_rig.Get();
        if (!c || MS::LastApplied(g_eid).seq == 0 || !M::Read(c, f) || f.slots != g_slots) {
            Late("the client's copy never showed the host's slots");
            return;
        }
        Fact("the copy of rig %u shows %d slots | %s", g_eid, g_slots, RigText(c).c_str());
        Say("c-ready ok");
        Go(g_c, C_L1Go);
        return;
    }
    case C_L1Go:
        if (!Seen("l1 go")) { Late("the host never said leg 1"); return; }
        Go(g_c, C_L1Take);
        return;
    case C_L1Take:
        if (!coop::dev::drill_hand::TakeIntoHandNow(M::ItemHoldName(M::kItemGpu), M::ItemClass(M::kItemGpu)))
            return Invalid("the client's hand could not take a GPU");
        Go(g_c, C_L1Use);
        return;
    case C_L1Use: {
        // The hand lane has queued the hold, so the host has it ahead of the use's ask on the one ordered lane.
        if (!HB::LocalHandActor() || !HB::OwnAnnounced(M::ItemClass(M::kItemGpu))) { Late("the hand lane never announced the GPU"); return; }
        Held h;
        if (!copy || !ReadHeld(me, h) || !h.actor) return Invalid("the client's hand holds no actor");
        g_ansC = MS::ClientLastAnswer().seq;
        g_seq0 = MS::LastApplied(g_eid).seq;
        if (!CallUse(copy, me, h)) return Invalid("the game's use did not run on the client's copy");
        Go(g_c, C_L1Wait);
        return;
    }
    case C_L1Wait: {
        const MS::Answer a = MS::ClientLastAnswer();
        const MS::Applied ap = MS::LastApplied(g_eid);
        if (a.seq == g_ansC) { Late("the host never answered the use"); return; }
        char say[64];
        if (a.reason != N::kMinerReasonGranted) {
            std::snprintf(say, sizeof(say), "c-l1 refused %u", static_cast<unsigned>(a.reason));
            Say(say);
            Go(g_c, C_Idle);
            return;
        }
        if (ap.seq == g_seq0 || !copy || !M::Read(copy, f)) { Late("the host's state never reached the client after its grant"); return; }
        std::snprintf(say, sizeof(say), "c-l1 %u %d %d %d", static_cast<unsigned>(a.reason), f.gpus, ap.gpus, HB::LocalHandActor() ? 0 : 1);
        Say(say);
        Go(g_c, C_L2);
        return;
    }
    case C_L2: {
        std::string r;
        if (!Rest("l2 ", &r)) { Late("the host never said leg 2"); return; }
        g_hostGpu = std::atoi(r.c_str());
        if (!copy || !M::Read(copy, f)) return Invalid("the client's copy is gone");
        f.gpus += 1;   // the forged count the host's own must overwrite
        if (!M::Write(copy, f)) return Invalid("the client's copy refused the forged count");
        g_ansC = MS::ClientLastAnswer().seq;
        g_seq0 = MS::LastApplied(g_eid).seq;
        if (!SendAsk(s, g_eid)) return Invalid("the forged use was not sent");
        Say("c-l2sent ok");
        Go(g_c, C_L2Judged);
        return;
    }
    case C_L2Judged: {
        if (!Seen("l2judged ok")) { Late("the host never judged the forged use"); return; }
        const MS::Answer a = MS::ClientLastAnswer();
        const MS::Applied ap = MS::LastApplied(g_eid);
        char say[96];
        if (!copy || !M::Read(copy, f)) return Invalid("the client's copy is gone");
        if (a.seq == g_ansC) std::snprintf(say, sizeof(say), "c-l2 unanswered");
        else if (ap.seq == g_seq0) std::snprintf(say, sizeof(say), "c-l2 got no state, copy %d host %d, reason %u", f.gpus, g_hostGpu, static_cast<unsigned>(a.reason));
        else if (f.gpus != g_hostGpu) std::snprintf(say, sizeof(say), "c-l2 kept %d, host %d", f.gpus, g_hostGpu);
        else std::snprintf(say, sizeof(say), "c-l2 ok");
        Say(say);
        Go(g_c, C_L4Rig);
        return;
    }
    case C_L4Rig: {
        std::string r;
        if (!Rest("l4 ", &r)) { Late("the host never said its full rig"); return; }
        long v[1] = {};
        if (Numbers(r, v, 1) != 1) return Invalid("the host's full rig line could not be read");
        g_fullEidC = static_cast<uint32_t>(v[0]);
        Go(g_c, C_L4Copy);
        return;
    }
    case C_L4Copy: {
        g_copyFull.Set(LiveProp(g_fullEidC));
        void* const c = g_copyFull.Get();
        if (!c || MS::LastApplied(g_fullEidC).seq == 0 || !M::Read(c, f) || f.gpus != kSlots) {
            Late("the client's copy never showed the host's full rig");
            return;
        }
        Fact("the copy of rig %u shows %d GPUs | %s", g_fullEidC, f.gpus, RigText(c).c_str());
        Go(g_c, C_L4Take);
        return;
    }
    case C_L4Take:
        if (!coop::dev::drill_hand::TakeIntoHandNow(M::ItemHoldName(M::kItemGpu), M::ItemClass(M::kItemGpu)))
            return Invalid("the client's hand could not take a GPU for leg 4");
        Go(g_c, C_L4Use);
        return;
    case C_L4Use: {
        if (!HB::LocalHandActor() || !HB::OwnAnnounced(M::ItemClass(M::kItemGpu))) { Late("the hand lane never announced the GPU for leg 4"); return; }
        Held h;
        void* const c = g_copyFull.Get();
        if (!c || !ReadHeld(me, h) || !h.actor || !M::Read(c, f)) return Invalid("the client holds no GPU or its copy of the full rig is gone");
        // A copy of a full rig refuses the use itself, so no ask would be made: the copy is written one GPU short, as a use that
        // raced the rig's last GPU sees it, and the game's own use then spends the item and the lane asks the host.
        f.gpus = kSlots - 1;
        if (!M::Write(c, f)) return Invalid("the client's copy refused the stale count");
        g_gpusBefore = GpusNear(me);
        if (!CallUse(c, me, h)) return Invalid("the game's use did not run on the client's copy of the full rig");
        Say("c-l4sent ok");
        Go(g_c, C_L4Judged);
        return;
    }
    case C_L4Judged: {
        std::string r;
        if (!Rest("l4judged ", &r)) { Late("the host never judged the use on the full rig"); return; }
        const int refunds = std::atoi(r.c_str());
        const int found = NewGpusNear(me, g_gpusBefore);
        // The host's refund is over when it says so; the item reaches this peer through the prop lane, so a refund the host made
        // is waited for, and one it did not make is looked for once.
        if (found == 0 && refunds > 0) { Late("the host's refunded GPU never reached the client"); return; }
        Fact("refund looked for: the host spawned %d, this peer finds %d new GPU(s) within %.0f cm of its body", refunds, found,
             static_cast<double>(kNearCm));
        Say(found > 0 ? "c-l4 found" : "c-l4 none found beside the client");
        Go(g_c, found > 0 ? C_L3 : C_Idle);
        return;
    }
    case C_L3: {
        if (!Seen("l3 go")) { Late("the host never said the flood"); return; }
        const uint8_t self = coop::players::Registry::Get().LocalPeerId();
        for (int i = 0; i < kFloodAsks; ++i) {
            if (!SendHand(s, HandState(self, "flood" + std::to_string(i))) || !SendAsk(s, g_eid))
                return Invalid("a message of the flood was not sent");
        }
        SendHand(s, EmptyHand(self));   // the forged hold goes, as the real hand's does; it follows the asks on the one lane
        Fact("sent %d asks in one tick, each behind a hand state of its own", kFloodAsks);
        Go(g_c, C_Idle);
        return;
    }
    case C_Idle:
        return;
    }
}

}  // namespace

void UseHost(N::Session& s) { HostStep(s); }
void UseClient(N::Session& s) { ClientStep(s); }

void UseReset() {
    g_h = H_Start;
    g_c = C_WaitRig;
    g_gpu0 = g_slots = g_hostGpu = 0;
    g_ans0 = g_granted0 = g_judged0 = g_refunds0 = g_ansC = g_seq0 = 0;
    g_full.Reset();
    g_copyFull.Reset();
    g_fullEid = g_fullEidC = 0;
    g_gpusBefore.clear();
}

}  // namespace coop::dev::miner_drill::detail

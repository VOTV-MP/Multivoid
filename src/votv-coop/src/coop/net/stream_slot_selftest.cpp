// coop/net/stream_slot_selftest.cpp -- the un-gated arithmetic selftest of coop/net/stream_slot.h,
// the eid merge and the host's stream group, run once per session start: a LAN reorders nothing, so
// the latches' refusals are proved on pinned sequences here, the reordered batch behind a taken one
// above all (a run proves the wiring under net.fakereorder_pct).

#include "coop/net/stream_slot.h"

#include "coop/net/remote_streams.h"  // HostStreams, EidPoseMerge

#include "ue_wrap/core/log.h"

#include <cstdint>
#include <vector>

namespace coop::net::stream_slot {

bool RunSelftest() {
    int pass = 0, total = 0;
    auto check = [&](bool ok, const char* what) {
        ++total;
        if (ok) { ++pass; return; }
        UE_LOGE("stream_slot selftest FAIL: %s", what);
    };

    // 1. A read stream: nothing before the first value, then the value with a fresh flag once.
    {
        StreamSlot<int> s;
        int out = -1;
        bool isNew = true;
        check(!s.Read(out, &isNew), "nothing to read before the first value");
        check(s.Offer(0, 7), "a first datagram is stored, sequence 0 included");
        check(s.Read(out, &isNew) && out == 7 && isNew, "the first read is fresh");
        check(s.Read(out, &isNew) && out == 7 && !isNew, "a re-read is not");
        check(!s.Offer(0, 8), "a duplicate of sequence 0 is refused");
        check(s.Offer(1, 9) && s.Read(out, &isNew) && out == 9 && isNew, "a newer one is fresh again");
        check(!s.Offer(1, 10) && s.LastSeq() == 1, "a duplicate is refused and the latch stays");
    }

    // 2. The newest wins across the sequence's wrap, and an older one is refused on either side.
    {
        StreamSlot<int> s;
        check(s.Offer(0xFFFFFFF0u, 1), "a sequence near the top is stored");
        check(s.Offer(5u, 2), "one past the wrap is newer");
        check(!s.Offer(0xFFFFFFF8u, 3), "one behind the wrap is older");
        check(!s.Offer(4u, 4), "and so is one just behind the newest");
        int out = 0;
        check(s.Read(out, nullptr) && out == 2 && s.LastSeq() == 5u, "the newest is kept");
    }

    // 3. A taken stream: consumed once, and a batch reordered behind one already taken is refused.
    {
        StreamSlot<std::vector<int>> s;
        std::vector<int> out;
        check(!s.Take(out), "nothing to take before the first batch");
        std::vector<int>* v = s.Claim(10);
        check(v != nullptr, "a first batch is claimed");
        if (v) *v = {1, 2, 3};
        check(s.Take(out) && out.size() == 3 && out[2] == 3, "the batch is taken whole");
        out.clear();
        check(!s.Take(out), "a batch is taken once");
        check(s.Claim(9) == nullptr, "an older batch after the take is refused");
        check(s.Claim(10) == nullptr, "and so is a duplicate");
        v = s.Claim(11);
        if (v) *v = {4};
        check(v != nullptr && s.Take(out) && out.size() == 1 && out[0] == 4, "a newer one is taken");
    }

    // 4. A take hands its buffer back: the next claim writes into the reader's old capacity.
    {
        StreamSlot<std::vector<int>> s;
        std::vector<int>* v = s.Claim(1);
        if (v) v->assign(4, 1);
        std::vector<int> out;
        out.reserve(64);
        check(s.Take(out) && out.size() == 4, "a batch is taken into a reserved buffer");
        v = s.Claim(2);
        check(v != nullptr && v->capacity() >= 64 && v->empty(),
              "the next claim gets the reader's empty buffer, capacity kept");
    }

    // 5. A reset slot takes a new sender whose count starts again from zero.
    {
        StreamSlot<int> s;
        check(s.Offer(500, 1), "the old sender's datagram is stored");
        s = StreamSlot<int>{};
        int out = 0;
        check(!s.Read(out, nullptr), "a reset slot has nothing to read");
        check(s.Offer(3, 2) && s.Read(out, nullptr) && out == 2, "a new sender's low sequence is stored");
    }

    // 6. A refused datagram leaves nothing fresh behind it.
    {
        StreamSlot<int> s;
        int out = 0;
        bool isNew = false;
        s.Offer(5, 1);
        s.Read(out, &isNew);
        check(!s.Offer(4, 2) && s.Read(out, &isNew) && out == 1 && !isNew,
              "a refused datagram changes neither the value nor the fresh flag");
    }

    // 7. The eid merge: sequence 0 latches, the latch outlives a take, and a take hands the buffer
    //    back cleared, so a reader's leftovers are never merged in.
    {
        EidPoseMerge<PropPoseSnapshot> m;
        PropPoseSnapshot p{};
        p.elementId = 7;
        p.x = 1.f;
        m.Merge(&p, 1, 0);
        std::vector<PropPoseSnapshot> out;
        check(m.Take(out) && out.size() == 1, "a first merge at sequence 0 is taken");
        m.Merge(&p, 1, 0);
        std::vector<PropPoseSnapshot> none;
        check(!m.Take(none), "a duplicate of sequence 0 is refused after the take");
        PropPoseSnapshot other{};
        other.elementId = 8;
        out.assign(1, other);  // the reader hands back a buffer still holding another element's pose
        p.x = 2.f;
        m.Merge(&p, 1, 1);
        check(m.Take(out) && out.size() == 1 && out[0].elementId == 7 && out[0].x == 2.f,
              "a newer merge is taken");
        p.x = 3.f;
        m.Merge(&p, 1, 2);
        std::vector<PropPoseSnapshot> next;
        check(m.Take(next) && next.size() == 1 && next[0].x == 3.f,
              "the reader's leftover, another element's pose, was not merged into the next take");
    }

    // 8. The host's group resets whole: a batch slot's latch and a merge's latch both take a new
    //    sender's low sequence after it.
    {
        HostStreams h;
        h.npcBatch.Claim(900);
        TrashClumpPoseSnapshot t{};
        t.eid = 3;
        h.trashCarry.Merge(&t, 1, 900);
        h.Reset();
        check(h.npcBatch.Claim(4) != nullptr, "a reset batch slot takes a new sender's low sequence");
        h.trashCarry.Merge(&t, 1, 4);
        std::vector<TrashClumpPoseSnapshot> out;
        check(h.trashCarry.Take(out) && out.size() == 1, "and so does a reset merge");
    }

    if (pass == total) {
        UE_LOGI("stream_slot selftest: ALL PASS (%d checks)", total);
        return true;
    }
    UE_LOGE("stream_slot selftest: %d/%d checks passed", pass, total);
    return false;
}

}  // namespace coop::net::stream_slot

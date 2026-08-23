// coop/session/join_seed.cpp -- see coop/session/join_seed.h.

#include "coop/session/join_seed.h"

#include "coop/net/session.h"

#include "ue_wrap/core/log.h"

namespace coop::join_seed {

static_assert(Seeder::kMaxPeers == static_cast<int>(coop::net::kMaxPeers),
              "join_seed slot array must match the session's peer count");

void Seeder::Capture(int peerSlot) {
    if (peerSlot <= 0 || peerSlot >= kMaxPeers) return;
    SlotSnap& snap = snaps_[peerSlot];
    snap = SlotSnap{};
    if (!a_.HashArray(snap.counts)) {
        UE_LOGW("%s: join snapshot for slot %d unreadable -- no seed", a_.name, peerSlot);
        return;
    }
    snap.valid = true;
    UE_LOGI("%s: join snapshot for slot %d (%zu distinct hashes)", a_.name, peerSlot,
            snap.counts.size());
}

void Seeder::Cancel(int peerSlot) {
    if (peerSlot <= 0 || peerSlot >= kMaxPeers) return;
    snaps_[peerSlot] = SlotSnap{};
    seededOnce_[peerSlot] = false;
}

void Seeder::SeedForSlot(coop::net::Session* s, int peerSlot) {
    if (peerSlot <= 0 || peerSlot >= kMaxPeers) return;
    if (!s || !s->connected()) return;
    SlotSnap& snap = snaps_[peerSlot];
    if (!snap.valid) {
        // No snapshot = no save-baseline knowledge; seeding the full array would
        // duplicate the joiner's save copy. Loud only at the slot's FIRST replay
        // (ConnectReplayForSlot re-fires on every mid-session world-change
        // re-announce, where a consumed snapshot is normal -- meadow audit fix 2).
        if (!seededOnce_[peerSlot])
            UE_LOGW("%s: no join snapshot for slot %d -- seed skipped", a_.name, peerSlot);
        return;
    }
    seededOnce_[peerSlot] = true;

    std::map<uint64_t, int32_t> cur;
    if (!a_.HashArray(cur)) {
        snap.valid = false;
        UE_LOGW("%s: array unreadable at seed time for slot %d -- no seed", a_.name, peerSlot);
        return;
    }

    // seedDelta(h) = cur - snap, per hash over the union. No pending-mask term:
    // these lanes have no cross-edge resend structure (measured, design doc par.2.6);
    // the live leg is CLOSED for the whole capture->ready gap by the B2 gate.
    std::map<uint64_t, int32_t> delta = cur;
    for (const auto& [h, c] : snap.counts) delta[h] -= c;

    int sentA = 0, sentD = 0;
    for (const auto& [h, d] : delta) {
        if (d > 0)      sentA += a_.SendAppendToSlot(s, peerSlot, h, d);
        else if (d < 0) sentD += a_.SendDeleteToSlot(s, peerSlot, h, -d);
    }
    if (sentA || sentD)
        UE_LOGI("%s: seed slot=%d +%d/-%d rows", a_.name, peerSlot, sentA, sentD);
    snap.valid = false;  // consume-once
}

void Seeder::Reset() {
    for (int i = 0; i < kMaxPeers; ++i) {
        snaps_[i] = SlotSnap{};
        seededOnce_[i] = false;
    }
}

bool RunSelfTest() {
    // Engine-free delta math over a fake adapter: multiset counts + gap-deletion +
    // both signs, deterministically (the /qf R7-R8 selftest rows).
    struct Case {
        const char* name;
        std::map<uint64_t, int32_t> snap, cur;
        std::map<uint64_t, int32_t> wantAppend, wantDelete;
    };
    const Case cases[] = {
        {"gap-append", {{1, 1}}, {{1, 1}, {2, 1}}, {{2, 1}}, {}},
        {"gap-deletion", {{1, 1}, {2, 1}}, {{1, 1}}, {}, {{2, 1}}},
        {"multiset-two-identical", {{7, 1}}, {{7, 3}}, {{7, 2}}, {}},
        {"multiset-remove-one-of-two", {{7, 2}}, {{7, 1}}, {}, {{7, 1}}},
        {"net-zero-author-and-delete", {{1, 1}}, {{1, 1}}, {}, {}},
        {"both-signs", {{1, 2}, {2, 1}}, {{1, 1}, {3, 1}}, {{3, 1}}, {{1, 1}, {2, 1}}},
    };
    bool pass = true;
    for (const Case& c : cases) {
        std::map<uint64_t, int32_t> delta = c.cur;
        for (const auto& [h, n] : c.snap) delta[h] -= n;
        std::map<uint64_t, int32_t> gotA, gotD;
        for (const auto& [h, d] : delta) {
            if (d > 0) gotA[h] = d;
            else if (d < 0) gotD[h] = -d;
        }
        const bool ok = (gotA == c.wantAppend) && (gotD == c.wantDelete);
        if (!ok) pass = false;
        UE_LOGI("join_seed selftest: %s %s", ok ? "PASS" : "FAIL", c.name);
    }
    UE_LOGI("join_seed selftest: %s (6 cases)", pass ? "ALL PASS" : "FAILURES");
    return pass;
}

}  // namespace coop::join_seed

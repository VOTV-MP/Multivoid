// coop/session/join_seed.h -- the shared ready-edge SEED for shadow-diff lanes
// (signal_sync + email_sync; the third instance of the meadow_db_sync v120 idiom,
// extracted per the extract-on-third rule).
//
// THE GAP IT CLOSES: SendReliable and the host relay skip !IsSlotWorldReady slots
// with no queue (B2, by design -- queueing pre-world lines would dupe the connect
// replay), so every line broadcast during a joiner's 30-60 s load window was
// silently NEVER delivered to it, and a no-reconcile shadow lane made that a
// PERMANENT divergence (b125 R-A shape (b)). The cure is a per-slot SEED:
//   - CAPTURE at the save_transfer OnRequest scratch-serialize (the same GT
//     callback that decides what the joiner's save will contain): a content-hash
//     MULTISET of the lane's array. Unreadable array => fail the WHOLE capture
//     (meadow :763 precedent) -- no seed beats a wrong seed.
//   - SEED at the ready edge (subsystems::ConnectReplayForSlot, which runs in the
//     SAME GT drain case that flips MarkSlotWorldReady -- event_feed.cpp:230-231,
//     so no GT-authored broadcast can interleave): seedDelta(h) = cur(h) - snap(h)
//     over the union; d>0 sends d append copies, d<0 sends -d hash-keyed deletes.
//   - Consume-once; the no-snapshot warn fires once per slot (cave-travel
//     re-announces re-run the replay with no snapshot -- normal, not a failure).
//
// Design of record (9-round /qf "that holds"):
// research/findings/network/votv-signal-email-ready-seeds-DESIGN-2026-08-23.md
// Meadow does NOT migrate here: its pending-mask + order-channel legs are
// meadow-specific (measured absent in the twins).
//
// Game thread throughout (capture rides the OnRequest GT callback, seed rides the
// event_feed drain).

#pragma once

#include <cstdint>
#include <map>

namespace coop::net { class Session; }

namespace coop::join_seed {

// Per-lane callbacks. All run on the game thread.
struct LaneAdapter {
    const char* name;  // for logs, e.g. "signal_sync"

    // Read the lane's CURRENT array as a content-hash multiset. Return false if
    // any row is unreadable (the caller fails the whole capture/seed).
    bool (*HashArray)(std::map<uint64_t, int32_t>& out);

    // Send `count` append copies of the row whose content hash is `hash` to
    // `peerSlot`. Returns the number actually sent (a raced-away row -- the
    // store moved since capture -- legitimately sends 0; meadow :817 precedent).
    int (*SendAppendToSlot)(coop::net::Session* s, int peerSlot, uint64_t hash,
                            int32_t count);

    // Send `count` hash-keyed deletes to `peerSlot`. Returns the number sent.
    int (*SendDeleteToSlot)(coop::net::Session* s, int peerSlot, uint64_t hash,
                            int32_t count);
};

// One instance per lane (static in the lane's TU).
class Seeder {
public:
    static constexpr int kMaxPeers = 4;  // static_assert'd == net::kMaxPeers in the .cpp

    explicit Seeder(const LaneAdapter& a) : a_(a) {}

    // save_transfer OnRequest: snapshot what the joiner's save will contain.
    void Capture(int peerSlot);

    // Teardown (save_transfer's cancel site + the lane's OnDisconnect).
    void Cancel(int peerSlot);

    // The ready edge: send seedDelta to the joiner. Consumes the snapshot.
    void SeedForSlot(coop::net::Session* s, int peerSlot);

    void Reset();  // whole-session teardown

private:
    struct SlotSnap {
        std::map<uint64_t, int32_t> counts;
        bool valid = false;
    };
    const LaneAdapter& a_;
    SlotSnap snaps_[kMaxPeers];
    bool seededOnce_[kMaxPeers] = {};
};

// Deterministic delta-math selftest (engine-free): multiset counts, gap-deletion,
// both-signs. Logs "join_seed selftest: PASS|FAIL ..." lines; returns pass.
// Run in the smoke via VOTVCOOP_RUN_SEED_SELFTEST=1.
bool RunSelfTest();

}  // namespace coop::join_seed

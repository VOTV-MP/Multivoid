// coop/session/join_seed.h -- the shared ready-edge SEED for shadow-diff lanes (signal_sync and
// email_sync).
//
// THE GAP IT CLOSES: SendReliable and the host relay skip a slot that is not world-ready, and by
// design do not queue for it, since queueing pre-world lines would duplicate the connect replay. So
// every line broadcast during a joiner's 30-60 s load window was silently never delivered, and a
// shadow lane with no reconcile made that a PERMANENT divergence. The cure is a per-slot SEED:
// capture what the joiner's save will contain, then at the ready edge send it whatever has changed
// since. Capture and seed are the two calls below.
//
// Meadow does NOT use this: its pending-mask and order-channel legs are absent in the twins. Game
// thread throughout.

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

    // save_transfer OnRequest: snapshot what the joiner's save will contain, as a content-hash
    // MULTISET of the lane's array. It runs in the same game-thread callback that decides the
    // save's contents, and an unreadable array fails the WHOLE capture -- no seed beats a wrong
    // seed.
    void Capture(int peerSlot);

    // Teardown (save_transfer's cancel site + the lane's OnDisconnect).
    void Cancel(int peerSlot);

    // The ready edge: send seedDelta to the joiner, consuming the snapshot. Called from
    // subsystems::ConnectReplayForSlot, which runs in the SAME drain case that flips
    // MarkSlotWorldReady, so no game-thread-authored broadcast can interleave. seedDelta(h) =
    // cur(h) - snap(h) over the union of hashes: d>0 sends d append copies, d<0 sends -d hash-keyed
    // deletes. The no-snapshot warning fires once per slot, because a cave-travel re-announce
    // re-runs the replay with no snapshot.
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

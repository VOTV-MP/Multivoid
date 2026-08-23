// coop/element/object_scan_hub.h -- THE shared GUObjectArray discovery pass (R-2, 2026-08-23).
//
// WHY (design of record: research/findings/architecture-audits/
// votv-shared-scan-hub-R2-DESIGN-2026-08-23.md, 8-round /qf "that holds"): thirteen *_sync
// consumers each full-walked ~270k GUObjectArray objects on their own 2 s cadence while
// unsettled -- ~220 full walks x 270k per session start, and settleScans=15 turned ANY
// index-count change into 15 more full walks PER CONSUMER. On the b133 field reporter's
// machine that stacked into the felt once-per-1-2 s stutter (sync:interactable median 74 ms).
// The class-level fix (MTA CClientStreamer shape -- the shared iterator lives in the mod
// layer, elements register into it): ONE shared pass serves every consumer, each object pays
// ONE ObjectAt + ONE ClassOf + one memoized class-verdict lookup instead of 13 predicate
// chains, and the pass is SLICED (~1 ms/frame) so even the single shared walk never lands
// whole on one frame.
//
// CONTRACT (all game-thread):
//   - Consumers register ONCE (Register during an active pass queues to the next pass start).
//   - IsInstance MUST be class-pure (verdict depends only on ClassOf(obj)) -- all 13 migrated
//     predicates are (ClassOf + IsDescendantOfAny / class-desc tables); the memo caches the
//     verdict per distinct UClass* per pass, re-verified per hit by {InternalIndex, SlotSerial}
//     (a recycled UClass* fails the compare and re-derives; the memo dies with the pass).
//   - OnPassBegin clears the consumer's pass scratch; OnMatch runs the consumer's own filter
//     tail (Default__ skip, IsLive, GetKey) and pushes scratch; OnPassComplete swaps the index
//     under the consumer's existing mutex, stamps `worldGen` into it, and returns the live
//     count for the settle gate. An ABORTED pass never calls OnPassComplete -- scratch is
//     cleared by the next OnPassBegin.
//   - WORLD-STAMPED passes: the pass records world_identity::Generation() at start and aborts
//     on a flip; consumers MUST treat an index whose stamped gen != current gen as EMPTY on
//     every read path (one int compare per Tick) -- this closes the pre-existing 44-s
//     dead-world window (R-1 class) for the whole family: slot+serial liveness is world-blind.
//   - Settle semantics preserved verbatim from the retired SettledObjectScan: a consumer's
//     count change (or zero) re-arms full passes; `settleScans` consecutive unchanged non-zero
//     counts settle it; the pass runs FULL while ANY consumer is unsettled (each consumer's
//     settleScans is a term of the shared demand function); all-settled -> tail passes +
//     one full backstop every kBackstopEvery passes (~60 s).
//
// Retired by this arc (RULE 2): ue_wrap/core/settled_object_scan.h +
// incremental_object_scan.h and all 13 per-consumer walks (zero other users, grep-verified).
#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::element::scan_hub {

struct Consumer {
    const char* name;                          // diag + parity attribution (string literal)
    void* ctx;                                 // consumer instance (nullptr for file-static modules)
    bool (*EnsureResolved)();                  // class resolution attempt; false -> sits out this pass
    bool (*IsInstance)(void* obj);             // CLASS-PURE predicate (see contract)
    void (*OnPassBegin)(void* ctx, bool isFull);
    void (*OnMatch)(void* ctx, void* obj);
    size_t (*OnPassComplete)(void* ctx, bool isFull, uint32_t worldGen);  // swap + return count
    int settleScans;                           // demand term (2 = churny, 15 = static classes)
};

// Register a consumer (game thread). During an active pass the registration is QUEUED and
// joins at the next pass start (the pass-scoped memo makes set changes otherwise hazard-free).
void Register(const Consumer& c);

// The per-frame driver: runs at most ~1 ms of slice work, starts passes on cadence, feeds the
// settle gates. Call once per net-pump tick (game thread).
void Tick();

// DEV-DRILL ONLY (parity mode A): run one complete FULL pass synchronously in this call --
// un-sliced, so a same-GT-task probe comparison has zero staleness. Never called in production.
void ForceSyncFullPass();

// True while a started pass has not yet completed (diagnostic).
bool PassActive();

// DEV-DRILL ONLY: the live count of consumer `name`'s last COMPLETED pass (SIZE_MAX when the
// consumer is unknown or has not completed a pass). The parity drill compares this against an
// independent old-shape probe walk -- see autotest_scanparity.cpp.
size_t DebugConsumerCount(const char* name);

// DEV-DRILL ONLY: true when consumer `name` is currently SETTLED (its completed-count has
// been stable for its settleScans). Parity mode B skips unsettled consumers -- a churning
// class (grime in live play) is <=1 pass stale by design and is certified by mode A instead.
bool DebugConsumerSettled(const char* name);

}  // namespace coop::element::scan_hub

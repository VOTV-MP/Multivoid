// coop/element/object_scan_hub.h -- THE shared GUObjectArray discovery pass. All
// game-thread.
//
// Thirteen *_sync consumers each full-walked ~270k GUObjectArray objects on their
// own 2 s cadence while unsettled -- about 220 full walks per session start -- and
// a settle window of 15 turned ANY index-count change into 15 more full walks PER
// CONSUMER, which stacked into a felt once-per-second stutter in the field.
//
// The fix is class-level, in the shape of MTA's CClientStreamer: the shared
// iterator lives in the mod layer and elements register into it. ONE shared pass
// serves every consumer, each object pays one ObjectAt, one ClassOf and one
// memoized class-verdict lookup instead of thirteen predicate chains, and the pass
// is SLICED at about a millisecond a frame, so even the single shared walk never
// lands whole on one frame.

#pragma once

#include <cstddef>
#include <cstdint>

namespace coop::element::scan_hub {

// CONTRACT (all game-thread). A pass calls OnPassBegin, then OnMatch per hit, then
// OnPassComplete; an ABORTED pass never reaches OnPassComplete, and the next
// OnPassBegin clears the scratch it left. IsInstance MUST be class-pure, because
// the memo caches its verdict per distinct UClass* for the whole pass, re-verified
// per hit by {InternalIndex, SlotSerial} so that a recycled UClass* fails the
// compare and re-derives; the memo dies with the pass.

struct Consumer {
    const char* name;                          // diag + parity attribution (string literal)
    void* ctx;                                 // consumer instance (nullptr for file-static modules)
    bool (*EnsureResolved)();                  // class resolution attempt; false -> sits out this pass
    bool (*IsInstance)(void* obj);             // CLASS-PURE predicate (see the contract)
    void (*OnPassBegin)(void* ctx, bool isFull);  // clears the pass scratch
    void (*OnMatch)(void* ctx, void* obj);  // the consumer's own filter tail
                                            // (Default__ skip, IsLive, GetKey), pushing scratch
    size_t (*OnPassComplete)(void* ctx, bool isFull, uint32_t worldGen);  // swap + stamp + count
    int settleScans;                           // demand term (2 = churny, 15 = static classes;
                                               // 0 = DEMAND-EXEMPT: 0<0 is false at every settle
                                               // site, so the consumer never forces full passes
                                               // and rides tails + the backstop -- the R-2b
                                               // reseed consumer, which builds no hub index)
};

// WORLD-STAMPED passes: a pass records world_identity::Generation() at its start
// and aborts on a flip, and consumers MUST treat an index whose stamped generation
// is not the current one as EMPTY on every read path -- one int compare per Tick.
// Together with the per-MATCH WorldOf() == CurrentWorld() term inside the pass,
// that closes both halves of the dead-world window: the stale index, and the
// re-admission of a DYING world's actor during world coexistence, since
// slot-and-serial liveness is world-blind.

// Register a consumer (game thread). During an active pass the registration is QUEUED and
// joins at the next pass start (the pass-scoped memo makes set changes otherwise hazard-free).
void Register(const Consumer& c);

// SETTLE: a consumer's count change, or a zero, re-arms full passes; settleScans
// consecutive unchanged non-zero counts settle it; the pass runs FULL while ANY
// consumer is unsettled; once all are settled it runs tail passes with one full
// backstop every kBackstopEvery passes (about a minute).

// The per-frame driver: runs at most ~1 ms of slice work, starts passes on cadence, feeds the
// settle gates. Call once per net-pump tick (game thread).
void Tick();

// DEV-DRILL ONLY (parity mode A): run one complete FULL pass synchronously in this call --
// un-sliced, so a same-GT-task probe comparison has zero staleness. Never called in production.
void ForceSyncFullPass();

// DEV-DRILL ONLY: the live count of consumer `name`'s last COMPLETED pass (SIZE_MAX when the
// consumer is unknown or has not completed a pass). The parity drill compares this against an
// independent old-shape probe walk -- see autotest_scanparity.cpp.
size_t DebugConsumerCount(const char* name);

// DEV-DRILL ONLY: true when consumer `name` is currently SETTLED (its completed-count has
// been stable for its settleScans). Parity mode B skips unsettled consumers -- a churning
// class (grime in live play) is <=1 pass stale by design and is certified by mode A instead.
bool DebugConsumerSettled(const char* name);

}  // namespace coop::element::scan_hub

// coop/props/prop_census.cpp -- the seed / re-seed GUObjectArray census walk +
// the world-coherence stamp (Fork A) + the purge-episode flag.
//
// EXTRACTED from prop_element_tracker.cpp 2026-07-10 (the tracker passed the
// 800-LOC soft cap; the census family was the flagged extraction). Behavior
// preserved byte-for-byte: same two-phase walk, same mutex scopes, same
// memory orders, same idempotency semantics. Shares the maintained
// known-keyed-props set with the tracker via prop_element_tracker_detail.h.

#include "coop/props/prop_element_tracker.h"

#include "prop_element_tracker_detail.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"          // ReadEnv (R-2b drill switches, dev-only)
#include "coop/element/object_scan_hub.h"  // R-2b: the steady re-seed is hub consumer #14
#include "coop/element/registry.h"
#include "coop/player/hand_item.h"  // hand-axis boundary: CollectHandAxisActors (SeedWalk_ skip; local hand + remote mirrors)
#include "coop/props/prop_snapshot.h"      // DeliverLateRegisteredProps (per drained chunk)
#include "ue_wrap/engine/engine.h"  // IsChildActor (child-actor exclusion, take-7 floating-CCTV RCA)
#include "ue_wrap/engine/world_identity.h"  // R-2b: queue worldGen + per-item WorldOf term
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/walk_timer.h"       // ScopedWalkTimer (reseed:sync-walk / reseed:drain)

#include <windows.h>  // QueryPerformanceCounter (the drain's ~1 ms budget)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace coop::prop_element_tracker {

namespace R = ue_wrap::reflection;
namespace P = ue_wrap::profile;

// ---- Seed / re-seed scan ------------------------------------------------
//
// Shared walk body for the one-shot boot seed AND an explicit re-seed (level/
// world change). Two-phase to avoid holding the mutex for the full ~150k
// GUObjectArray walk: phase 1 builds a local vector of live keyed-interactable
// pointers without any lock (reflection probes are thread-safe in our setup);
// phase 2 takes the mutex once and bulk-inserts. Both phases are IDEMPOTENT --
// g_knownKeyedProps.insert is a set insert (no-op if present) and MarkPropElement
// no-ops on an already-tracked actor -- so a re-seed only ADDS props the world
// gained that we are not yet tracking. Returns counts; `newlyTracked` is how many
// keyed actors this walk added to g_knownKeyedProps (= the whole set on the first
// boot seed; the delta on a re-seed).
namespace {
struct SeedCounts { int liveFound = 0; int newlyTracked = 0; int cdo = 0; int dying = 0; int keylessPiles = 0; };

// ---- World-coherence stamp (Fork A, 2026-06-10) --------------------------
// SeedWalk_ stamps the live gameplay UWorld it ran against; the snapshot
// gate (prop_snapshot::TriggerForSlot) refuses to open a bracket unless that
// world is still the live one. During a world transition the registry holds
// the dead world's props until the drain-complete re-seed -- a bracket built
// then is near-empty and the client's adoption sweep destroys against it
// (the 2026-06-10 1300-actor mass destroy). GT-write (seeds are GT
// contracts), GT-read (TriggerForSlot/DrainChunk); atomics for the file's
// sibling-symmetry only.
std::atomic<bool>     g_seededOnce{false};
std::atomic<void*>    g_seedWorld{nullptr};
std::atomic<int32_t>  g_seedWorldIdx{-1};
std::atomic<uint64_t> g_seedGeneration{0};
// Purge-episode flag (gate hardening, 2026-06-10 smoke falsification): the
// reaper detected a mass purge and the registry is draining dead elements
// until the episode-end re-seed. The world-stamp above is NOT sufficient on
// its own: VOTV's boot/save-load flow can leave the stamped UWorld ALIVE
// while the registry is majority-dead (smoke 15:16: stamp live, 1161 dying
// vs 88 live at enumerate -> the gate passed and the client swept 3067
// actors against an 88-prop bracket). net_pump owns the detection edges;
// this is registry-coherence state so the tracker owns the flag.
std::atomic<bool>     g_inPurgeEpisode{false};

SeedCounts SeedWalk_(std::vector<void*>* outNewActors) {
    const int32_t n = R::NumObjects();
    SeedCounts c;
    // v105 axis boundary (2026-07-06), WIDENED 2026-07-10 (audit HIGH): hand-axis
    // actors are PLAYER EXPRESSION, not world entities -- the LOCAL player's
    // hotbar hand (updateHold destroys + respawns it per quick-slot switch;
    // peers see it via the HandItem lane) AND every REMOTE peer's display
    // mirror (SpawnMirror mints a real Aprop_C whose adoption here would
    // broadcast a phantom keyed PropSpawn to all peers + the connect snapshot,
    // with its destroy echo-suppressed -- permanent phantom; the other half of
    // the 13:44:00 eid=5377 dupe class). Hoisted once per walk (<= 8 entries,
    // stable within one GT walk).
    void* handAxis[1 + coop::players::kMaxPeers];
    const size_t handAxisN =
        coop::hand_item::CollectHandAxisActors(handAxis, 1 + coop::players::kMaxPeers);
    const auto isHandAxis = [&](void* obj) {
        for (size_t h = 0; h < handAxisN; ++h)
            if (handAxis[h] == obj) return true;
        return false;
    };
    std::vector<void*> live;
    live.reserve(4096);
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (isHandAxis(obj)) continue;  // hand-axis display actors are not world props
        if (!ue_wrap::prop::IsKeyedInteractable(obj)) continue;
        // CHILD-ACTOR EXCLUSION (2026-07-12, take-7 floating-CCTV RCA): a ChildActorComponent
        // child (kerfur eye cam) is parent-owned, never an independent world prop -- keep it out
        // of g_knownKeyedProps + outNewActors so the steady re-seed never treats a toggle-fresh
        // eye cam as "new" (the incremental express would broadcast it; audit CRITICAL). Matches
        // the Init-POST + MarkPropElement gates; predicate: ue_wrap::engine::IsChildActor.
        if (ue_wrap::engine::IsChildActor(obj)) continue;
        const std::wstring nm = R::ToString(R::NameOf(obj));
        if (nm.rfind(L"Default__", 0) == 0) { ++c.cdo; continue; }
        if (!R::IsLive(obj)) { ++c.dying; continue; }
        live.push_back(obj);
    }
    c.liveFound = static_cast<int>(live.size());
    {
        std::lock_guard<std::mutex> lk(g_knownKeyedPropsMutex);
        for (void* obj : live) {
            if (g_knownKeyedProps.size() >= kKnownKeyedPropsCap) break;
            if (g_knownKeyedProps.insert(obj).second) {
                // CHURN GUARD (2026-07-03, the 11:48:59 keyless-PropSpawn re-broadcast):
                // an actor ALREADY BOUND to a live owned element is NOT new -- it is the
                // churned actor of a tracked element (a host RE-PILE land re-creates the
                // pile actor in place and the pile layer rebinds the element same-tick).
                // The private known-set is a pointer set, blind to actor churn (the
                // re-bind thread's exact lesson: identity maps that don't track churn
                // smear) -- without this test every land result re-entered outNewActors
                // and net_pump re-broadcast it as an incremental keyless PropSpawn every
                // 20 s re-seed. The set insert above still refreshes membership (the
                // O(1) snapshot de-dupe), it just is not "newness" any more.
                // FRESHNESS (audit 45bdb7ac W-3, the IsBoundMirrorNative D1 pattern):
                // trust the binding only if THIS actor is still the live occupant of
                // the element's slot -- a recycled address pointing at a dead element's
                // row must still express as new (the express is that prop's only
                // delivery).
                if (const coop::element::ElementId beid = GetPropElementIdForActor(obj);
                    beid != coop::element::kInvalidId) {
                    coop::element::Element* el = coop::element::Registry::Get().Get(beid);
                    if (el && R::IsLiveByIndex(obj, el->GetInternalIdx())) continue;
                }
                ++c.newlyTracked;
                // R1: yield the newly-adopted actor so the steady-world re-seed
                // can broadcast ONE incremental PropSpawn for it (the eid is
                // minted in phase 2 below, before this function returns, so a
                // caller iterating outNewActors resolves GetPropElementIdForActor).
                // Captured here (the ONLY newness signal -- phase-2 MarkPropElement
                // is idempotent + silent on already-tracked). May include keyless
                // non-pile actors that phase 2 won't express; the express filters them.
                if (outNewActors) outNewActors->push_back(obj);
            }
        }
    }
    // Tier 3 Props migration 2026-05-28: also create Prop Element shadows
    // for each seeded actor so Registry::SnapshotByType<Prop> works as the
    // unified late-joiner snapshot path. Class + key resolved per-actor;
    // skip MarkPropElement on actors whose key is empty/None.
    //
    // Audit fix 2026-05-29: re-check IsLive at the start of phase 2. Phase 1
    // built `live` without holding any lock; an actor's K2_DestroyActor PRE
    // observer can fire between phases -- if MarkPropElement then commits a
    // dangling actor pointer into the shared PropMirrors() manager + the
    // g_actorToPropElementId reverse map, the eid leaks for the session
    // lifetime because UnmarkKnownKeyedProp already ran for that actor and
    // won't fire again.
    // (KEY-UNIQUENESS AUTHORITY note, 2026-07-11 take-3: the duplicate-Key re-key lives inside
    // MarkPropElement -- the ONE enrollment owner -- so this walk AND the Init-POST late-load
    // catch AND every other enroll path are all covered. See prop_element_tracker.cpp.)
    for (void* obj : live) {
        if (!R::IsLive(obj)) continue;
        const std::wstring cls = R::ClassNameOf(obj);
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(obj);
        if (key.empty() || key == L"None") {
            // Fork B HALF 1 (2026-06-10): keyless chipPile -- its cross-peer
            // identity is the ElementId (the v52 eid lane; precedent
            // trash_collect_sync::BroadcastConvertNear's idempotent
            // MarkPropElement). Minting an Element here puts the host's
            // world piles into the connect snapshot (the keyless skip in
            // DrainChunk routes them down the eidOnly receiver lane), which
            // is what lets the client adopt the host's pile set instead of
            // sweeping its own and receiving nothing. All OTHER keyless
            // actors (a held clump mid-flight, a pre-Init Aprop_C, a keyless
            // trashBits straggler) are NOT expressible and stay untracked --
            // symmetric with the sweep's universe test.
            if (ue_wrap::prop::IsChipPile(obj)) {
                MarkPropElement(obj, L"", cls, EnrollSource::kPassiveCensus);
                ++c.keylessPiles;
            }
            continue;
        }
        // v122 (B): on a CLIENT this keyed call is index-refresh-only (no Element mint) --
        // the host expresses keyed identity by key; the express seams own client births.
        MarkPropElement(obj, key, cls, EnrollSource::kPassiveCensus);  // idempotent
    }
    // Stamp the gameplay world this walk expressed. MUST IsLive-filter
    // (mid-transition the DYING old world sits at a lower GUObjectArray
    // index) and gameplay-name-filter (matches the reaper's gate in
    // net_pump -- a menu/preLoad world never opens the snapshot gate).
    // Perf audit W-2 (2026-06-10): inline pointer-compare walk, NOT
    // FindObjectsByClass -- that helper allocates a ClassNameOf wstring per
    // GUObjectArray entry (~250k allocs), doubling every seed walk. The
    // UWorld UClass resolves once (sticky); per entry this walk is two
    // pointer reads, with ToString only on actual World instances (a
    // handful).
    {
        static std::atomic<void*> sWorldCls{nullptr};
        void* worldCls = sWorldCls.load(std::memory_order_acquire);
        if (!worldCls) {
            worldCls = R::FindClass(P::name::WorldClass);
            if (worldCls) sWorldCls.store(worldCls, std::memory_order_release);
        }
        void* w = nullptr;
        if (worldCls) {
            for (int32_t i = 0; i < n; ++i) {
                void* cand = R::ObjectAt(i);
                if (!cand || R::ClassOf(cand) != worldCls) continue;
                if (!R::IsLive(cand)) continue;
                if (R::ToString(R::NameOf(cand)).find(L"ntitled") == std::wstring::npos) continue;
                w = cand;
                break;
            }
        }
        if (w) {
            g_seedWorld.store(w, std::memory_order_release);
            g_seedWorldIdx.store(R::InternalIndexOf(w), std::memory_order_release);
        }
        // Bump on EVERY walk (even if no gameplay world resolved): every
        // coherence-restoring event is a generation bump, and the deferred-
        // slot flush in prop_snapshot keys on it.
        g_seedGeneration.fetch_add(1, std::memory_order_release);
    }
    return c;
}
}  // namespace

void SeedKnownKeyedProps() {
    // Latch promoted to the file-scope g_seededOnce (Fork A) so the snapshot
    // gate can refuse to bracket before the boot seed has ever run (RULE 2:
    // one latch, not a function-local twin).
    if (g_seededOnce.load(std::memory_order_acquire)) return;
    // R-2b (blind-instrument lesson): EVERY synchronous census walk is labeled here, at
    // the shared body's door -- the retired steady-branch label lived at ONE caller and
    // left the other five structurally invisible in field logs.
    ue_wrap::ScopedWalkTimer _wt("reseed:sync-walk");
    const SeedCounts c = SeedWalk_(nullptr);
    UE_LOGI("prop_element_tracker: seeded known-keyed-props set with %d live actors (%d new, %d keyless chipPile element(s), %d CDOs, %d dying skipped) -- subsequent snapshots skip GUObjectArray walk",
            c.liveFound, c.newlyTracked, c.keylessPiles, c.cdo, c.dying);
    g_seededOnce.store(true, std::memory_order_release);
}

size_t ReSeedKnownKeyedProps(std::vector<void*>* outNewActors) {
    ue_wrap::ScopedWalkTimer _wt("reseed:sync-walk");  // R-2b: see SeedKnownKeyedProps
    const SeedCounts c = SeedWalk_(outNewActors);
    UE_LOGI("prop_element_tracker: re-seed found %d live keyed props, added %d NEW to tracking (%d keyless chipPile element(s), %d CDOs, %d dying) -- world/level-change reconcile [snapshot-completeness]",
            c.liveFound, c.newlyTracked, c.keylessPiles, c.cdo, c.dying);
    return static_cast<size_t>(c.newlyTracked);
}


bool HasSeededOnce() {
    return g_seededOnce.load(std::memory_order_acquire);
}

bool IsRegistrySeededForCurrentWorld() {
    // O(1): IsLiveByIndex reads ONLY the GUObjectArray slot metadata at the
    // captured index -- never the (possibly freed) world's memory. The
    // instant a world swap's GC purge kills the stamped UWorld, this reads
    // false with zero detection latency; the next SeedWalk_ (episode-end
    // re-seed / small-travel re-seed / self-heal) re-stamps the new world.
    void* w = g_seedWorld.load(std::memory_order_acquire);
    return w && R::IsLiveByIndex(w, g_seedWorldIdx.load(std::memory_order_acquire));
}

uint64_t SeedGeneration() {
    return g_seedGeneration.load(std::memory_order_acquire);
}

void SetInPurgeEpisode(bool active) {
    g_inPurgeEpisode.store(active, std::memory_order_release);
}

bool InPurgeEpisode() {
    return g_inPurgeEpisode.load(std::memory_order_acquire);
}

// ---- R-2b: the STEADY re-seed as scan-hub consumer #14 --------------------
// Design of record: votv-reseed-hub-consumer-DESIGN-2026-08-23.md (11-round /qf).
// The retired registry_reaper steady branch paid a single-frame ~270k full census
// every ~20 s (field: 120 ms avg / 1,880 ms max on the reporter's host). Here the
// shared sliced pass collects candidates and a ~1 ms/tick budget drain adjudicates
// them with the phase-1/phase-2 semantics relocated verbatim.

namespace {

// The reaper's 4 s gameplay-vs-menu verdict (see SetReaperInGameplayWorld).
std::atomic<bool> g_reaperInGameplay{false};

struct ReseedItem {
    void*   obj;
    int32_t idx;     // InternalIndex captured at match time
    int32_t serial;  // SlotSerial captured at match time (D1 re-verify pair)
};

// All game-thread (hub passes, drain, and the synchronous walks share the GT).
std::vector<ReseedItem> g_reseedScratch;         // pass-scoped (cleared at OnPassBegin)
std::vector<ReseedItem> g_reseedQueue;           // the adjudication queue
size_t   g_reseedQueueHead    = 0;
uint32_t g_reseedQueueGen     = 0;               // world generation the queue belongs to
bool     g_reseedQueueHasFull = false;           // diag: queue contains a FULL batch
int32_t  g_reseedLastSeenNum  = -1;              // grew detector (parity with the old NumObjects guard)
bool     g_reseedRegistered   = false;
uint64_t g_reseedDrainTicks   = 0;               // diag: drain ticks spent on the current queue
size_t   g_reseedDrainedNew   = 0;               // diag: adoptions from the current queue
size_t   g_reseedDrainedRejects = 0;             // diag: dying-world rejects from the current queue
// 60 s summary window (the bump-cadence observable the acceptance greps; a per-bump
// line would be ~0.5 Hz spam). Session-window counters, GT.
uint64_t g_reseedSumBumps = 0, g_reseedSumQueues = 0, g_reseedSumNew = 0,
         g_reseedSumRejects = 0, g_reseedSumDrops = 0;
std::chrono::steady_clock::time_point g_reseedSumSince{};

// Gate = code-identical to the retired steady `else if` (registry_reaper): the
// gameplay-world verdict is the reaper's own published 4 s read (same source +
// cadence the old branch used; world_identity pointers are identities -- never
// dereferenced for a name check).
bool ReseedGatePasses_() {
    return g_reaperInGameplay.load(std::memory_order_acquire) &&
           HasSeededOnce() && IsRegistrySeededForCurrentWorld() && !InPurgeEpisode();
}

// SeedGeneration bump, grew-based parity with the old walk's unconditional bump
// (the old walk only RAN on grew/periodic). Env mute = the RED calibration for
// the acceptance bump-cadence gate -- never set outside a drill.
void BumpSeedGeneration_() {
    static const bool sMuted = !coop::config::ReadEnv("VOTVCOOP_RESEED_MUTE_BUMP").empty();
    if (sMuted) {
        static bool sLogged = false;
        if (!sLogged) { sLogged = true; UE_LOGW("reseed: [drill] SeedGeneration bump MUTED (RED calibration)"); }
        return;
    }
    g_seedGeneration.fetch_add(1, std::memory_order_release);
    // Counted HERE, past the mute, not at the call site: the first RED-calibration run
    // counted bump ATTEMPTS (summary bumps=19 with the generation frozen) -- a gate
    // observable that cannot go red observes nothing.
    ++g_reseedSumBumps;
}

void ReseedPassBegin_(void*, bool) { g_reseedScratch.clear(); }

void ReseedMatch_(void*, void* obj) {
    const int32_t idx = R::InternalIndexOf(obj);
    g_reseedScratch.push_back(ReseedItem{obj, idx, R::SlotSerial(idx)});
}

size_t ReseedPassComplete_(void*, bool isFull, uint32_t worldGen) {
    const size_t enqueuedCandidates = g_reseedScratch.size();
    const int32_t curNum = R::NumObjects();
    const bool grew = (curNum != g_reseedLastSeenNum);
    g_reseedLastSeenNum = curNum;
    if (!ReseedGatePasses_()) {
        // Audit MINOR-5: gate-fail scratch drops are counted (and logged when non-empty)
        // so the transition A/B arithmetic can see them -- same family as IMPORTANT-1.
        if (!g_reseedScratch.empty()) {
            g_reseedSumDrops += g_reseedScratch.size();
            UE_LOGI("reseed: pass scratch dropped (n=%zu reason=gate)", g_reseedScratch.size());
        }
        g_reseedScratch.clear();  // no adjudication outside steady state; episode paths own it
        return enqueuedCandidates;
    }
    // Queue merge (R3-C1): one worldGen per queue. Different gen -> the old queue is
    // dead (the drain gate would drop it) -> REPLACE. Same gen: a FULL batch REPLACES
    // (a full scratch is a superset -- an undrained still-live candidate is re-matched,
    // the census is idempotent; an undrained dead one is correctly dropped); a TAIL
    // batch APPENDS (delta -- must not be lost).
    if (worldGen != g_reseedQueueGen || isFull) {
        if (g_reseedQueueHead < g_reseedQueue.size() && worldGen != g_reseedQueueGen) {
            g_reseedSumDrops += g_reseedQueue.size() - g_reseedQueueHead;  // audit IMPORTANT-1
            UE_LOGI("reseed: queue dropped (n=%zu reason=gen-flip at pass merge)",
                    g_reseedQueue.size() - g_reseedQueueHead);
        }
        // Audit IMPORTANT-1: a REPLACE preempting an unfinished drain must fold the
        // per-queue counters into the 60 s sums BEFORE resetting them, or the summary
        // undercounts exactly during mass-adopt convergence (the directional A/B gate
        // `old - new == rejects + drops` is built on these counters).
        g_reseedSumNew     += g_reseedDrainedNew;
        g_reseedSumRejects += g_reseedDrainedRejects;
        g_reseedQueue.swap(g_reseedScratch);
        g_reseedQueueHead    = 0;
        g_reseedQueueGen     = worldGen;
        g_reseedQueueHasFull = isFull;
        g_reseedDrainTicks   = 0;
        g_reseedDrainedNew   = 0;
        g_reseedDrainedRejects = 0;
    } else {
        g_reseedQueue.insert(g_reseedQueue.end(), g_reseedScratch.begin(), g_reseedScratch.end());
    }
    g_reseedScratch.clear();
    // Bump at gated pass-complete, NOT queue-empty (R9-C3: a bracket built from a
    // partially-drained registry self-heals -- mid-bracket expresses are claim-safe).
    if (isFull || grew) BumpSeedGeneration_();  // the summary counter lives inside (mute-aware)
    return enqueuedCandidates;  // pre-adjudication count; sole consumer is DebugConsumerCount
}

}  // namespace

void SetReaperInGameplayWorld(bool inGameplay) {
    g_reaperInGameplay.store(inGameplay, std::memory_order_release);
}

void InstallReseedScanConsumer() {
    if (g_reseedRegistered) return;
    g_reseedRegistered = true;
    coop::element::scan_hub::Register(coop::element::scan_hub::Consumer{
        "prop_reseed", nullptr,
        &ue_wrap::prop::EnsurePropBaseResolved,
        &ue_wrap::prop::IsKeyedInteractable,   // CLASS-PURE (prop.cpp IsClassKeyedInteractable)
        &ReseedPassBegin_, &ReseedMatch_, &ReseedPassComplete_,
        /*settleScans*/ 0});  // demand-exempt: never forces full passes (0<0 false at every hub site)
}

void DrainReseedQueue() {
    // 60 s summary FIRST (before the empty early-return, or a quiet world would never
    // flush it) -- the acceptance's bump-cadence observable (RED when the mute drill
    // zeroes bumps; >=3/60 s expected from the ~20 s backstop fulls alone).
    {
        const auto now = std::chrono::steady_clock::now();
        if (g_reseedSumSince.time_since_epoch().count() == 0) g_reseedSumSince = now;
        if (now - g_reseedSumSince >= std::chrono::seconds(60)) {
            g_reseedSumSince = now;
            UE_LOGI("reseed: hub steady summary (60s): queues=%llu bumps=%llu new=%llu rejects=%llu drops=%llu",
                    static_cast<unsigned long long>(g_reseedSumQueues),
                    static_cast<unsigned long long>(g_reseedSumBumps),
                    static_cast<unsigned long long>(g_reseedSumNew),
                    static_cast<unsigned long long>(g_reseedSumRejects),
                    static_cast<unsigned long long>(g_reseedSumDrops));
            g_reseedSumQueues = g_reseedSumBumps = g_reseedSumNew = g_reseedSumRejects = g_reseedSumDrops = 0;
        }
    }
    if (g_reseedQueueHead >= g_reseedQueue.size()) return;  // empty -- two size_t reads
    ue_wrap::ScopedWalkTimer _wt("reseed:drain");
    // Per-tick gate re-check (DrainChunk:525's mid-drain-abort shape): a world flip or
    // a purge episode invalidates the WHOLE queue -- the episode/travel synchronous
    // walks own post-transition re-derivation.
    if (g_reseedQueueGen != ue_wrap::world_identity::Generation() || !ReseedGatePasses_()) {
        const size_t dropped = g_reseedQueue.size() - g_reseedQueueHead;
        g_reseedSumDrops += dropped;
        UE_LOGI("reseed: queue dropped (n=%zu reason=%s)", dropped,
                g_reseedQueueGen != ue_wrap::world_identity::Generation() ? "gen-flip" : "episode");
        g_reseedQueue.clear();
        g_reseedQueueHead = 0;
        return;
    }
    // [drill] interleave: force a SYNCHRONOUS census between drain ticks with the
    // queue still charged. The assert is DUP-SIDE ONLY (audit IMPORTANT-2): zero
    // duplicate-eid expresses across the run -- insert().second is the sole newness
    // authority, the sync walk inserting first makes the drain's insert fail. A prop
    // adopted BY the bare sync walk here legitimately expresses ZERO times (its
    // production callers pair with retriggerReadySlots / re-bracket; this bare call
    // does not), so "not 0" is NOT part of the assert. Latched: once per process.
    {
        static const bool sDrill = !coop::config::ReadEnv("VOTVCOOP_RESEED_INTERLEAVE_DRILL").empty();
        static bool sFired = false;
        if (sDrill && !sFired && g_reseedQueueHead > 0) {
            sFired = true;
            UE_LOGW("reseed: [drill] forcing synchronous ReSeed mid-drain (queue n=%zu head=%zu)",
                    g_reseedQueue.size(), g_reseedQueueHead);
            ReSeedKnownKeyedProps(nullptr);
        }
    }
    ++g_reseedDrainTicks;
    // Hand-axis snapshot once per drain tick (<=8 entries; membership churns per
    // quick-slot switch, which is why it is NOT evaluated per-slice at match time).
    void* handAxis[1 + coop::players::kMaxPeers];
    const size_t handAxisN =
        coop::hand_item::CollectHandAxisActors(handAxis, 1 + coop::players::kMaxPeers);
    // ~1 ms budget (QPC checked every 8 items) -- the hub-slice discipline. A fixed
    // item count would comb the mass-adopt stall (256 x ~140 us adoption = ~36 ms).
    static const long long sQpcPerMs = [] {
        LARGE_INTEGER f{};
        return ::QueryPerformanceFrequency(&f) ? f.QuadPart / 1000 : 0;
    }();
    LARGE_INTEGER t0{};
    ::QueryPerformanceCounter(&t0);
    size_t adoptedThisTick = 0;
    size_t processed = 0;
    while (g_reseedQueueHead < g_reseedQueue.size()) {
        // Budget check EVERY item (QPC ~20 ns; a per-8 check let 8 back-to-back
        // ~170 us adoption+express items overshoot the budget 2.4x).
        if (processed != 0) {
            LARGE_INTEGER now{};
            ::QueryPerformanceCounter(&now);
            if (sQpcPerMs > 0 && (now.QuadPart - t0.QuadPart) >= sQpcPerMs) break;
        }
        const ReseedItem it = g_reseedQueue[g_reseedQueueHead++];
        ++processed;
        // Cross-frame re-verify (D1 pattern -- never bare IsLive on a pointer that
        // aged across slices/ticks): array-slot reads FIRST, WorldOf only on a
        // slot+serial-live object.
        if (!R::IsLiveByIndex(it.obj, it.idx)) continue;
        if (R::SlotSerial(it.idx) != it.serial) continue;
        if (ue_wrap::world_identity::WorldOf(it.obj) != ue_wrap::world_identity::CurrentWorld()) {
            ++g_reseedDrainedRejects;  // the named directional delta: today's census would
            continue;                  // have adopted a dying-world actor here (R-1 class)
        }
        bool isHand = false;
        for (size_t h = 0; h < handAxisN; ++h) {
            if (handAxis[h] == it.obj) { isHand = true; break; }
        }
        if (isHand) continue;
        if (ue_wrap::engine::IsChildActor(it.obj)) continue;
        {
            const std::wstring nm = R::ToString(R::NameOf(it.obj));
            if (nm.rfind(L"Default__", 0) == 0) continue;
        }
        // Newness: SeedWalk_'s phase-1 block relocated verbatim -- insert().second is
        // the SOLE authority; the churn guard + freshness re-check keep a rebound/
        // recycled actor out of the express exactly as before.
        bool isNew = false;
        {
            std::lock_guard<std::mutex> lk(g_knownKeyedPropsMutex);
            if (g_knownKeyedProps.size() < kKnownKeyedPropsCap &&
                g_knownKeyedProps.insert(it.obj).second) {
                if (const coop::element::ElementId beid = GetPropElementIdForActor(it.obj);
                    beid != coop::element::kInvalidId) {
                    coop::element::Element* el = coop::element::Registry::Get().Get(beid);
                    if (!(el && R::IsLiveByIndex(it.obj, el->GetInternalIdx()))) isNew = true;
                } else {
                    isNew = true;
                }
            }
        }
        // Phase-2 (outside the mutex, today's ordering): idempotent Mark refresh for
        // keyed (client: key-index only, v122 no-passive-mint) + keyless pile mint.
        const std::wstring cls = R::ClassNameOf(it.obj);
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(it.obj);
        bool expressible = false;
        if (key.empty() || key == L"None") {
            if (ue_wrap::prop::IsChipPile(it.obj)) {
                MarkPropElement(it.obj, L"", cls, EnrollSource::kPassiveCensus);
                expressible = true;  // keyless pile rides the eid lane
            }
        } else {
            MarkPropElement(it.obj, key, cls, EnrollSource::kPassiveCensus);
            expressible = true;
        }
        if (isNew && expressible) {
            // Express PER ITEM, INSIDE the budget loop (the first acceptance run showed
            // the end-of-tick chunk Deliver escaping the QPC budget: ~800 adoptions'
            // payload builds + sends landed on ONE tick = 140/254 ms -- exactly the
            // stall class this drain exists to remove). The 1-element vector keeps
            // DeliverLateRegisteredProps the ONE kerfur-vs-generic routing owner.
            void* one[1] = {it.obj};
            coop::prop_snapshot::DeliverLateRegisteredProps(std::vector<void*>(one, one + 1));
            ++adoptedThisTick;
        }
    }
    if (adoptedThisTick > 0) {
        g_reseedDrainedNew += adoptedThisTick;
        // The "net_pump:" prefix + this exact wording are LOAD-BEARING: mp.py's joinchurn
        // gate greps "broadcasting one PropSpawn each (incremental" (tools/mp.py) and the
        // A/B digests sum this line's counts across runs. Do not reword casually.
        UE_LOGI("net_pump: steady-world re-seed adopted %zu NEW runtime-spawned keyed prop(s) "
                "(spawn-menu/toolgun/ambient/pile) -- broadcasting one PropSpawn each "
                "(incremental delta, no re-bracket; MTA CEntityAddPacket shape)", adoptedThisTick);
    }
    if (g_reseedQueueHead >= g_reseedQueue.size()) {
        // Only interesting drains get their own line (a per-pass line would be ~0.5 Hz
        // spam); the 60 s summary below is the steady-state observable.
        if (g_reseedDrainedNew > 0 || g_reseedDrainedRejects > 0 || g_reseedDrainTicks > 1) {
            UE_LOGI("reseed: queue drained (n=%zu new=%zu rejects=%zu ticks=%llu full=%d)",
                    g_reseedQueue.size(), g_reseedDrainedNew, g_reseedDrainedRejects,
                    static_cast<unsigned long long>(g_reseedDrainTicks),
                    g_reseedQueueHasFull ? 1 : 0);
        }
        ++g_reseedSumQueues;
        g_reseedSumNew     += g_reseedDrainedNew;
        g_reseedSumRejects += g_reseedDrainedRejects;
        g_reseedQueue.clear();
        g_reseedQueueHead = 0;
        g_reseedQueueHasFull = false;  // audit MINOR-4: a later tail-append queue must not inherit it
        g_reseedDrainTicks = 0;
        g_reseedDrainedNew = 0;
        g_reseedDrainedRejects = 0;
    }
}

}  // namespace coop::prop_element_tracker

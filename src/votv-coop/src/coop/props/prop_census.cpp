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

#include "coop/element/registry.h"
#include "coop/player/hand_item.h"  // hand-axis boundary: CollectHandAxisActors (SeedWalk_ skip; local hand + remote mirrors)
#include "ue_wrap/engine/engine.h"  // IsChildActor (child-actor exclusion, take-7 floating-CCTV RCA)
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <atomic>
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
    const SeedCounts c = SeedWalk_(nullptr);
    UE_LOGI("prop_element_tracker: seeded known-keyed-props set with %d live actors (%d new, %d keyless chipPile element(s), %d CDOs, %d dying skipped) -- subsequent snapshots skip GUObjectArray walk",
            c.liveFound, c.newlyTracked, c.keylessPiles, c.cdo, c.dying);
    g_seededOnce.store(true, std::memory_order_release);
}

size_t ReSeedKnownKeyedProps(std::vector<void*>* outNewActors) {
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


}  // namespace coop::prop_element_tracker

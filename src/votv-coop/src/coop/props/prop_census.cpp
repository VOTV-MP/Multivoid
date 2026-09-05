// coop/props/prop_census.cpp -- the seed and re-seed census of keyed props: the synchronous
// GUObjectArray walk, the world-coherence stamp, the purge-episode flag, and the steady re-seed
// as a scan-hub consumer with a budgeted drain. Shares the known-keyed-props set with the
// tracker through prop_element_tracker_detail.h.

#include "coop/props/prop_element_tracker.h"

#include "prop_element_tracker_detail.h"  // co-located private header (src tree, not include/)

#include "coop/config/config.h"          // ReadEnv, the drill switches
#include "coop/element/object_scan_hub.h"  // the steady re-seed is a hub consumer
#include "coop/element/registry.h"
#include "coop/player/hand_item.h"  // CollectHandAxisActors: the local hand and the remote mirrors
#include "coop/props/prop_snapshot.h"      // DeliverLateRegisteredProps (per drained chunk)
#include "ue_wrap/engine/engine.h"  // IsChildActor
#include "ue_wrap/engine/world_identity.h"  // the queue's world generation and the per-item world term
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/walk_timer.h"       // ScopedWalkTimer

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

// The shared walk body for the boot seed and an explicit re-seed (a level or world change). Two
// phases, so the mutex is not held for the whole walk: phase 1 collects live keyed interactables
// with no lock, phase 2 takes the mutex once and bulk-inserts. Both phases are idempotent (a set
// insert, and MarkPropElement no-ops on a tracked actor), so a re-seed only adds props the world
// gained. `newlyTracked` is how many this walk added: the whole set on the boot seed, the delta
// on a re-seed.
namespace {
struct SeedCounts { int liveFound = 0; int newlyTracked = 0; int cdo = 0; int dying = 0; int keylessPiles = 0; };

// The world-coherence stamp: the live gameplay world the walk ran against. The snapshot gate
// refuses to open a bracket unless that world is still the live one; during a transition the
// registry holds the dead world's props until the drain-complete re-seed, and a bracket built
// then is near-empty, which the client's adoption sweep destroys against. Game-thread write and
// read.
std::atomic<bool>     g_seededOnce{false};
std::atomic<void*>    g_seedWorld{nullptr};
std::atomic<int32_t>  g_seedWorldIdx{-1};
std::atomic<uint64_t> g_seedGeneration{0};
// The purge-episode flag: the reaper detected a mass purge and the registry is draining dead
// elements until the episode-end re-seed. The world stamp alone is not enough: the boot and
// save-load flow can leave the stamped world alive while the registry is majority-dead. The
// pump owns the detection edges; the flag is registry-coherence state.
std::atomic<bool>     g_inPurgeEpisode{false};

SeedCounts SeedWalk_(std::vector<void*>* outNewActors) {
    const int32_t n = R::NumObjects();
    SeedCounts c;
    // Hand-axis actors are player expression, not world entities: the local player's hotbar hand
    // (destroyed and respawned per quick-slot switch, carried by the HandItem lane) and every
    // remote peer's display mirror, a real prop whose adoption here would broadcast a phantom
    // PropSpawn with an echo-suppressed destroy. Hoisted once per walk.
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
        // A ChildActorComponent child (the kerfur's eye camera) is parent-owned, never an
        // independent world prop; kept out of the known set and the new list so the steady re-seed
        // never treats a toggle-fresh one as new and broadcasts it. Matches the Init and
        // MarkPropElement gates.
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
                // The churn guard: an actor already bound to a live owned element is not new, it is
                // the churned actor of a tracked element (a host re-pile re-creates the pile actor
                // in place and the pile layer rebinds the element the same tick). The known set is
                // a pointer set, blind to churn, and without this every re-pile re-entered the new
                // list and was re-broadcast at each re-seed. The binding is trusted only if this
                // actor is still the live occupant of the element's slot; a recycled address
                // pointing at a dead element's row must still express as new.
                if (const coop::element::ElementId beid = GetPropElementIdForActor(obj);
                    beid != coop::element::kInvalidId) {
                    coop::element::Element* el = coop::element::Registry::Get().Get(beid);
                    if (el && R::IsLiveByIndex(obj, el->GetInternalIdx())) continue;
                }
                ++c.newlyTracked;
                // The newly adopted actor is yielded so the steady re-seed can broadcast one
                // incremental PropSpawn for it; the eid is minted in phase 2 before this returns.
                // The only newness signal; the list may include keyless non-pile actors phase 2
                // does not express, and the express filters them.
                if (outNewActors) outNewActors->push_back(obj);
            }
        }
    }
    // Phase 2 also creates the Prop Element shadow for each seeded actor, so the unified snapshot
    // path can enumerate props by type. Liveness is re-checked at the start of phase 2: phase 1
    // held no lock, and a destroy observer can fire between the phases; a dangling pointer
    // committed into the mirror manager and the reverse map would leak its eid for the session,
    // since the unmark already ran for that actor. The duplicate-key re-key lives inside
    // MarkPropElement, the one enrollment owner, so every enroll path is covered.
    for (void* obj : live) {
        if (!R::IsLive(obj)) continue;
        const std::wstring cls = R::ClassNameOf(obj);
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(obj);
        if (key.empty() || key == L"None") {
            // A keyless chipPile's cross-peer identity is its ElementId, so an Element is minted
            // here, which puts the host's world piles into the connect snapshot (the keyless skip
            // in the drain routes them down the eid-only receiver lane) and lets the client adopt
            // the host's pile set instead of sweeping its own. Every other keyless actor (a held
            // clump in flight, a pre-Init prop) is not expressible and stays untracked, symmetric
            // with the sweep's universe test.
            if (ue_wrap::prop::IsChipPile(obj)) {
                MarkPropElement(obj, L"", cls, EnrollSource::kPassiveCensus);
                ++c.keylessPiles;
            }
            continue;
        }
        // On a client this keyed call refreshes the index only (no Element mint): the host
        // expresses keyed identity by key, and the express seams own client births.
        MarkPropElement(obj, key, cls, EnrollSource::kPassiveCensus);  // idempotent
    }
    // The gameplay world this walk expressed, stamped. Liveness-filtered (mid-transition the dying
    // world sits at a lower index) and name-filtered (a menu or preLoad world never opens the
    // snapshot gate). An inline pointer-compare walk rather than a class search that allocates a
    // name per entry: the World class resolves once, and only actual World instances are named.
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
        // Bumped on every walk, even with no gameplay world resolved: every coherence-restoring
        // event is a generation bump, and the deferred-slot flush in prop_snapshot keys on it.
        g_seedGeneration.fetch_add(1, std::memory_order_release);
    }
    return c;
}
}  // namespace

void SeedKnownKeyedProps() {
    // One latch, file-scope, so the snapshot gate can refuse to bracket before the boot seed has
    // run.
    if (g_seededOnce.load(std::memory_order_acquire)) return;
    // Every synchronous census walk is labelled here, at the shared body's door, so each caller is
    // visible in field logs.
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
    // O(1): IsLiveByIndex reads only the slot metadata at the captured index, never the possibly
    // freed world's memory. The instant a world swap's purge kills the stamped world this reads
    // false, and the next walk re-stamps the new one.
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

// The steady re-seed as a scan-hub consumer. The retired reaper branch paid a single-frame full
// census every 20 s or so (over a second at worst on a big world); here the shared sliced pass
// collects candidates and a drain of about 1 ms per tick adjudicates them with the walk's
// phase-1 and phase-2 semantics.

namespace {

// The reaper's 4 s gameplay-versus-menu verdict (SetReaperInGameplayWorld).
std::atomic<bool> g_reaperInGameplay{false};

struct ReseedItem {
    void*   obj;
    int32_t idx;     // InternalIndex captured at match time
    int32_t serial;  // SlotSerial captured at match time (D1 re-verify pair)
};

// All game thread: the hub passes, the drain and the synchronous walks share it.
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
// The 60 s summary window (the bump cadence the acceptance greps; a per-bump line would be
// spam). Session-window counters.
uint64_t g_reseedSumBumps = 0, g_reseedSumQueues = 0, g_reseedSumNew = 0,
         g_reseedSumRejects = 0, g_reseedSumDrops = 0;
std::chrono::steady_clock::time_point g_reseedSumSince{};

// The gate: the reaper's published gameplay verdict (a 4 s read; world pointers are identities,
// never dereferenced for a name), the boot seed done, the registry stamped for the current
// world, and no purge episode.
bool ReseedGatePasses_() {
    return g_reaperInGameplay.load(std::memory_order_acquire) &&
           HasSeededOnce() && IsRegistrySeededForCurrentWorld() && !InPurgeEpisode();
}

// The generation bump, on a full pass or a grown object array. The env mute is the red
// calibration for the acceptance's bump-cadence gate; never set outside a drill.
void BumpSeedGeneration_() {
    static const bool sMuted = !coop::config::ReadEnv("VOTVCOOP_RESEED_MUTE_BUMP").empty();
    if (sMuted) {
        static bool sLogged = false;
        if (!sLogged) { sLogged = true; UE_LOGW("reseed: [drill] SeedGeneration bump MUTED (RED calibration)"); }
        return;
    }
    g_seedGeneration.fetch_add(1, std::memory_order_release);
    // Counted past the mute, not at the call site: a gate observable that cannot go red observes
    // nothing.
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
        // Gate-fail scratch drops are counted and logged, so the A/B arithmetic can see them.
        if (!g_reseedScratch.empty()) {
            g_reseedSumDrops += g_reseedScratch.size();
            UE_LOGI("reseed: pass scratch dropped (n=%zu reason=gate)", g_reseedScratch.size());
        }
        g_reseedScratch.clear();  // no adjudication outside steady state; episode paths own it
        return enqueuedCandidates;
    }
    // The queue merge: one world generation per queue. A different generation means the old queue
    // is dead (the drain gate would drop it), so it is replaced; on the same generation a full
    // batch replaces (a superset: an undrained live candidate is re-matched, an undrained dead one
    // is correctly dropped) and a tail batch appends (a delta that must not be lost).
    if (worldGen != g_reseedQueueGen || isFull) {
        if (g_reseedQueueHead < g_reseedQueue.size() && worldGen != g_reseedQueueGen) {
            g_reseedSumDrops += g_reseedQueue.size() - g_reseedQueueHead;  // folded into the sums
            UE_LOGI("reseed: queue dropped (n=%zu reason=gen-flip at pass merge)",
                    g_reseedQueue.size() - g_reseedQueueHead);
        }
        // A replace preempting an unfinished drain folds the per-queue counters into the 60 s sums
        // first, or the summary undercounts exactly during a mass adoption.
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
    // Bumped at the gated pass-complete, not at queue-empty: a bracket built from a partially
    // drained registry self-heals, since mid-bracket expresses are claim-safe.
    if (isFull || grew) BumpSeedGeneration_();  // the summary counter lives inside
    return enqueuedCandidates;  // the pre-adjudication count
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
        &ue_wrap::prop::IsKeyedInteractable,   // class-pure
        &ReseedPassBegin_, &ReseedMatch_, &ReseedPassComplete_,
        /*settleScans*/ 0});  // demand-exempt: never forces full passes
}

void DrainReseedQueue() {
    // The 60 s summary first, before the empty early-return, or a quiet world would never flush it.
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
    // The gate re-checked per tick: a world flip or a purge episode invalidates the whole queue,
    // and the episode and travel walks own the re-derivation.
    if (g_reseedQueueGen != ue_wrap::world_identity::Generation() || !ReseedGatePasses_()) {
        const size_t dropped = g_reseedQueue.size() - g_reseedQueueHead;
        g_reseedSumDrops += dropped;
        UE_LOGI("reseed: queue dropped (n=%zu reason=%s)", dropped,
                g_reseedQueueGen != ue_wrap::world_identity::Generation() ? "gen-flip" : "episode");
        g_reseedQueue.clear();
        g_reseedQueueHead = 0;
        return;
    }
    // The interleave drill: a synchronous census forced between drain ticks with the queue still
    // charged. The assert is duplicate-side only, zero duplicate-eid expresses across the run: the
    // set insert is the sole newness authority, and the sync walk inserting first makes the drain's
    // insert fail. A prop adopted by the bare sync walk legitimately expresses zero times. Once per
    // process.
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
    // The hand-axis snapshot once per drain tick (at most a few entries; membership churns per
    // quick-slot switch, so it is not evaluated at match time).
    void* handAxis[1 + coop::players::kMaxPeers];
    const size_t handAxisN =
        coop::hand_item::CollectHandAxisActors(handAxis, 1 + coop::players::kMaxPeers);
    // The budget of about 1 ms, the hub-slice discipline: a fixed item count would comb a mass
    // adoption into a stall.
    static const long long sQpcPerMs = [] {
        LARGE_INTEGER f{};
        return ::QueryPerformanceFrequency(&f) ? f.QuadPart / 1000 : 0;
    }();
    LARGE_INTEGER t0{};
    ::QueryPerformanceCounter(&t0);
    size_t adoptedThisTick = 0;
    size_t processed = 0;
    while (g_reseedQueueHead < g_reseedQueue.size()) {
        // The budget checked every item: eight back-to-back adoptions with expresses overshot a
        // per-eight check by more than double.
        if (processed != 0) {
            LARGE_INTEGER now{};
            ::QueryPerformanceCounter(&now);
            if (sQpcPerMs > 0 && (now.QuadPart - t0.QuadPart) >= sQpcPerMs) break;
        }
        const ReseedItem it = g_reseedQueue[g_reseedQueueHead++];
        ++processed;
        // The cross-frame re-verify: slot reads first (index and serial), the world term only on a
        // slot-and-serial-live object; never a bare IsLive on a pointer that aged across ticks.
        if (!R::IsLiveByIndex(it.obj, it.idx)) continue;
        if (R::SlotSerial(it.idx) != it.serial) continue;
        if (ue_wrap::world_identity::WorldOf(it.obj) != ue_wrap::world_identity::CurrentWorld()) {
            ++g_reseedDrainedRejects;  // a dying-world actor
            continue;                  
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
        // Newness, the walk's phase-1 rule: the set insert is the sole authority, and the churn
        // guard plus the freshness check keep a rebound or recycled actor out of the express.
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
        // Phase 2, outside the mutex: the idempotent mark refresh for a keyed prop (index only on a
        // client) and the mint for a keyless pile.
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
            // Expressed per item, inside the budget loop: an end-of-tick chunk delivery escaped the
            // budget and landed hundreds of payload builds and sends on one tick, the stall class
            // this drain removes. The one-element vector keeps DeliverLateRegisteredProps the one
            // routing owner.
            void* one[1] = {it.obj};
            coop::prop_snapshot::DeliverLateRegisteredProps(std::vector<void*>(one, one + 1));
            ++adoptedThisTick;
        }
    }
    if (adoptedThisTick > 0) {
        g_reseedDrainedNew += adoptedThisTick;
        // The "net_pump:" prefix and this wording are load-bearing: tools/mp.py's joinchurn gate
        // greps "broadcasting one PropSpawn each (incremental", and the A/B digests sum this line's
        // counts. The broadcast half is host-only, so a client prints the second form: its
        // adoptions are tracked locally, and it authors no PropSpawn.
        if (coop::prop_snapshot::ExpressWouldBroadcast())
            UE_LOGI("net_pump: steady-world re-seed adopted %zu NEW runtime-spawned keyed prop(s) "
                    "(spawn-menu/toolgun/ambient/pile) -- broadcasting one PropSpawn each "
                    "(incremental delta, no re-bracket; MTA CEntityAddPacket shape)", adoptedThisTick);
        else
            UE_LOGI("net_pump: steady-world re-seed adopted %zu NEW runtime-spawned keyed prop(s) "
                    "-- tracked locally; a client authors no PropSpawn", adoptedThisTick);
    }
    if (g_reseedQueueHead >= g_reseedQueue.size()) {
        // Only an interesting drain gets its own line; the 60 s summary is the steady-state
        // observable.
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
        g_reseedQueueHasFull = false;  // a later tail-append queue must not inherit it
        g_reseedDrainTicks = 0;
        g_reseedDrainedNew = 0;
        g_reseedDrainedRejects = 0;
    }
}

}  // namespace coop::prop_element_tracker

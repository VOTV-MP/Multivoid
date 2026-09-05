// coop/props/join_membership_sweep.cpp -- see join_membership_sweep.h. The claim set records every
// actor the host snapshot bound (or this client announced) inside a bracket; at load-tail
// quiescence the sweep destroys the unclaimed part of the client's own save-loaded world, under a
// per-class completeness floor and a >50% valve.

#include "coop/props/join_membership_sweep.h"
#include "coop/session/world_load_episode.h"  // the load-tail quiescence probe

#include "coop/creatures/kerfur_entity.h"
#include "coop/creatures/kerfur_reconcile.h"
#include "coop/creatures/npc_sync.h"
#include "coop/dev/force_overdestroy_test.h"
#include "coop/dev/join_window_pos_trace.h"  // the keyed-prop join-window position trace
#include "coop/dev/spawn_order_probe.h"
#include "coop/element/element.h"
#include "coop/element/mirror_managers.h"  // PropMirrors, the dead-actor mirror-row census
#include "coop/element/prop.h"             // coop::element::Prop (the PropMirrors snapshot element type)
#include "coop/element/quiescence_drain.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/props/pile_spawn_bind.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_lifecycle.h"
#include "coop/props/remote_prop.h"
#include "coop/props/save_identity_bind.h"
#include "coop/props/snapshot_census.h"
#include "coop/props/trash_pile_sync.h"
#include "coop/element/mirror_defer.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop::join_membership_sweep {

namespace P = ue_wrap::profile;
namespace R = ue_wrap::reflection;
namespace E = ue_wrap::engine;

namespace {
// Claim tracking state, game thread only: armed, recorded and swept from the event_feed drain and
// the net_pump disconnect edge.
std::unordered_set<void*> g_claimedActors;
bool g_claimTrackingActive = false;

// The deferred sweep: SnapshotComplete arms it, TickClientReconcile fires it once the load tail
// has quiesced. Game thread only.
bool g_sweepPending = false;                          // armed at SnapshotComplete; cleared when the sweep runs
bool g_sweepFired   = false;                          // sticky: set when the sweep runs, until re-armed (HasLoadTailQuiesced)
std::chrono::steady_clock::time_point g_sweepArmedAt{};  // fire-log timing only
// The lost-bracket backstop: after the announce (probe latched) the host's bracket normally
// arrives within a second. If no bracket produced a sweep inside this window (SnapshotBegin lost,
// host wedged), load-tail quiescence is declared without a doom sweep so every consumer (the
// steady drain, NPC adoption, the grab guards) un-sticks.
constexpr int kBracketFlakeMs = 30000;

// The host snapshot bound `actor` (exact key, fuzzy or fresh spawn), so the sweep spares it. One
// bool read when tracking is disarmed.
void RecordClaim(void* actor) {
    if (!g_claimTrackingActive || !actor) return;
    g_claimedActors.insert(actor);
}
}  // namespace (file-local claim/sweep state)

void RecordClaimIfTracking(void* actor) {
    // The claim for the self-announce sites (Init, takeObj, held-item and convert broadcasts): a
    // prop a client announces while a bracket is open cannot be in the host's already-enumerated
    // bracket, and unclaimed it would be destroyed at the sweep while the host keeps the mirror. A
    // claim is an entity expressed on the wire this bracket, in either direction.
    RecordClaim(actor);
}

void BeginClaimTracking() {
    g_claimedActors.clear();
    g_claimTrackingActive = true;
    // A fresh bracket cancels a sweep pending from the prior one (a two-level load) and the fired
    // latch; SnapshotComplete re-arms.
    g_sweepPending = false;
    g_sweepFired = false;
    // The pile-bind candidates re-enumerate (stale entries would be dead pointers).
    coop::pile_spawn_bind::Reset();  // index only -- the deferred reconcile queues (quiescence_drain) survive the bracket
    // The prior bracket's completeness census goes; until this bracket's arrives HostCountForClass
    // is -1 and the floor is a no-op.
    coop::snapshot_census::Reset();
    // The read-only probes arm for this join.
    coop::dev::spawn_order_probe::ArmForJoin();
    coop::dev::join_window_pos_trace::ArmForJoin();
    UE_LOGI("join_membership_sweep: claim tracking ARMED (snapshot bracket open) -- "
            "unclaimed in-universe locals will be destroyed at SnapshotComplete");
}

// The one sweep, driven by TickClientReconcile once the load tail has quiesced.
static void RunDivergenceSweep_(void* localPlayer) {
    if (!g_claimTrackingActive) {
        // A SnapshotComplete without its Begin must not sweep with an empty claim set: that would
        // destroy every in-universe actor.
        UE_LOGW("join_membership_sweep: claim sweep requested but tracking is not armed -- skipping");
        return;
    }
    // The universe is what the host snapshot can express a binding for: keyed interactables and the
    // keyless chipPile lineage (eid-expressed), plus the wire-suppressed intermediate the client
    // already destroys on sight while connected. A keyless non-pile (a held clump mid-flight, a
    // pre-Init Aprop_C, an event clump whose setKey never sticks) is outside it and never swept.
    // The candidates are the client's own local Prop elements from the registry, never a
    // GUObjectArray scan: deletion by tracked membership, as MTA's CElementGroup iterates its
    // member list rather than the world. So a host-driven mirror is excluded at the source (a
    // kerfur prop mirror is never a save-load divergence), and a keyless transient is never a local
    // element. The >50% valve stays: an incomplete host bracket leaves most of the loaded world
    // unclaimed, and that is an incomplete snapshot, not a divergence. One registry snapshot, no
    // per-actor mutex.
    std::vector<coop::element::Registry::ActorIdPair> propPairs;
    coop::element::Registry::Get().SnapshotActorsByType(
        coop::element::ElementType::Prop, propPairs);
    // The keyed churn re-bind: a keyed prop whose actor GC-churned mid-join leaves its mirror row
    // holding a dead pointer while the game re-creates a same-key twin; the re-create gets a fresh
    // local element and would be doomed as unclaimed, destroying a host-known entity (and the dead
    // row then mis-resolves a recycled address). The dead-actor keyed mirror rows are collected
    // once; the doom loop re-binds a candidate onto its orphaned row instead. The chipPile analogue
    // is the position re-bind in save_identity_bind.
    std::unordered_map<std::string, coop::element::ElementId> deadKeyedRows;
    {
        std::vector<coop::element::Prop*> rows;
        coop::element::PropMirrors().Snapshot(rows);
        for (coop::element::Prop* p : rows) {
            if (!p || !p->IsMirror()) continue;
            if (p->GetName().empty()) continue;  // keyless (chip) rows: the position lane owns them
            void* ra = p->GetActor();
            if (ra && R::IsLiveByIndex(ra, p->GetInternalIdx())) continue;  // live row -> not orphaned
            deadKeyedRows.emplace(p->GetName(), p->GetId());
        }
    }
    auto narrowAscii = [](const std::wstring& w) {
        std::string s; s.reserve(w.size());
        for (wchar_t c : w) s.push_back(static_cast<char>(c & 0xFF));
        return s;
    };
    int inClass = 0;        // live, non-mirror, LOCAL Prop Elements (the membership universe)
    int claimedCount = 0;
    std::vector<void*> doomed;
    doomed.reserve(propPairs.size());
    std::vector<std::wstring> doomedClass;  // Phase 0: class per doomed actor, parallel to `doomed`
    doomedClass.reserve(propPairs.size());
    std::unordered_map<std::wstring, int> doomedByClass;
    std::unordered_map<std::wstring, int> claimedByClass;  // Phase 0: claimed count per class (floor numerator)
    std::unordered_map<std::wstring, int> keylessSkippedByClass;
    for (const auto& pr : propPairs) {
        if (pr.mirror) continue;                                    // host-driven mirror -> not a save divergence (automatic kerfur exemption)
        if (!pr.actor) continue;
        if (!R::IsLiveByIndex(pr.actor, pr.internalIdx)) continue;  // dead (no deref) -- the reaper owns it
        ++inClass;
        void* a = pr.actor;
        // The class is read before the claimed check: the completeness floor counts claims per
        // class.
        const std::wstring acls = R::ClassNameOf(a);
        if (g_claimedActors.count(a)) { ++claimedCount; ++claimedByClass[acls]; continue; }  // host-expressed / self-claimed -> converged, keep
        // A per-player state actor (the inventory container) is this player's own save state, never
        // host expressed and never swept; membership alone would doom it, and the sweep once
        // destroyed the client's inventory container, which fataled at the next GC purge.
        if (coop::prop_lifecycle::IsPerPlayerPropClass(acls)) continue;
        if (ue_wrap::prop::IsChipPile(a)) {            // expressible keyed OR keyless (eid lane)
            doomed.push_back(a);
            doomedClass.push_back(acls);
            ++doomedByClass[acls];
            continue;
        }
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(a);
        if (key.empty() || key == L"None") {
            ++keylessSkippedByClass[acls];  // defensive tripwire: a tracked keyless non-pile should not exist post-quiescence
            continue;
        }
        // This unclaimed keyed local's key names an expressed identity whose row lost its actor: it
        // is that identity's re-create. The row is rebound onto it (the same drain-local-element
        // plus rebindInPlace path every save bind takes) and it counts as claimed.
        if (!deadKeyedRows.empty()) {
            auto dr = deadKeyedRows.find(narrowAscii(key));
            if (dr != deadKeyedRows.end()) {
                coop::prop_element_tracker::UnmarkKnownKeyedProp(a);  // drain the re-create's fresh LOCAL element
                // The position trace records the re-create before the re-bind.
                coop::dev::join_window_pos_trace::NoteRecreateRebind(key, static_cast<uint32_t>(dr->second), a);
                coop::remote_prop::RegisterPropMirror(dr->second, a, key, acls, /*senderSlot*/ 0,
                                                      /*rebindInPlace*/ true);
                const ue_wrap::FVector rbLoc = ue_wrap::engine::GetActorLocation(a);
                UE_LOGW("join_membership_sweep: keyed churn RE-BIND -- unclaimed '%ls' key='%ls' "
                        "loc=(%.1f,%.1f,%.1f) is the re-create of already-expressed eid=%u (mirror row held "
                        "a dead actor) -> row rebound, actor claimed, NOT doomed (docs/piles/12 eid=2947 upstream)",
                        acls.c_str(), key.c_str(), rbLoc.X, rbLoc.Y, rbLoc.Z,
                        static_cast<unsigned>(dr->second));
                deadKeyedRows.erase(dr);
                ++claimedCount;
                ++claimedByClass[acls];
                continue;
            }
        }
        // A swept trashBitsPile is unwatched from the counter channel before it dies: the sweep
        // runs after join_progress::Complete in the same drain, and the next tick's death-watch
        // would otherwise see a near-camera vanish and broadcast a keyed PropDestroy for a pile the
        // host still has.
        if (ue_wrap::prop::IsTrashBitsPile(a)) {
            coop::trash_pile_sync::NotifyWireDestroy(key);
        }
        doomed.push_back(a);
        doomedClass.push_back(acls);
        ++doomedByClass[acls];
    }
    // The index half: a client's save-loaded keyed props have no element row (the key index is
    // their tracked membership), so the row walk cannot see them. They are adjudicated here under
    // the same rules: claimed survive, per-player skipped, keyed-churn re-binds spared, the rest
    // doomed under the same per-class accounting. An actor with an element row belongs to the row
    // walk, so the two halves partition and nothing is counted twice.
    {
        std::vector<coop::prop_element_tracker::KeyIndexEntry> keyedIdx;
        coop::prop_element_tracker::CollectKeyIndexEntries(keyedIdx);
        int idxUniverse = 0, idxClaimed = 0, idxEvicted = 0;
        for (const auto& ke : keyedIdx) {
            if (!ke.actor) continue;
            if (!R::IsLiveByIndex(ke.actor, ke.internalIdx)) {
                // A dead entry is evicted now: an element-less keyed entry has no registry row, so
                // the reaper never sees it; this sweep, the lookup's lazy evict and
                // DrainDeadKeyIndexEntries are its only collectors.
                coop::prop_element_tracker::EvictKeyIndexEntryIfStale(ke.actor, ke.key);
                continue;
            }
            if (coop::element::Registry::Get().EidForActor(ke.actor) !=
                coop::element::kInvalidId) continue;                    // element-bound -> row walk's half
            ++inClass; ++idxUniverse;
            const std::wstring acls = R::ClassNameOf(ke.actor);
            if (g_claimedActors.count(ke.actor)) {
                ++claimedCount; ++claimedByClass[acls]; ++idxClaimed;   // floor numerator keeps its semantics
                continue;
            }
            if (coop::prop_lifecycle::IsPerPlayerPropClass(acls)) continue;
            // A cached (actor, idx) pair can survive a slot recycle; only the live actor's current
            // key proves the entry still names it. A mismatch (an impostor or an un-reindexed
            // rekey) evicts the entry, never dooms; the live actor re-enters at the next census
            // walk. Paid only for would-be-doomed entries.
            const std::wstring liveKey = ue_wrap::prop::GetInteractableKeyString(ke.actor);
            if (liveKey != ke.key) {
                coop::prop_element_tracker::EvictKeyIndexEntryIfStale(ke.actor, ke.key);
                ++idxEvicted;
                continue;
            }
            // The keyed churn re-bind, as in the row walk.
            if (!deadKeyedRows.empty()) {
                auto dr = deadKeyedRows.find(narrowAscii(liveKey));
                if (dr != deadKeyedRows.end()) {
                    coop::dev::join_window_pos_trace::NoteRecreateRebind(
                        liveKey, static_cast<uint32_t>(dr->second), ke.actor);
                    coop::remote_prop::RegisterPropMirror(dr->second, ke.actor, liveKey, acls,
                                                          /*senderSlot*/ 0, /*rebindInPlace*/ true);
                    UE_LOGW("join_membership_sweep: keyed churn RE-BIND (index-half) -- unclaimed '%ls' "
                            "key='%ls' is the re-create of expressed eid=%u -> row rebound, claimed, NOT doomed",
                            acls.c_str(), liveKey.c_str(), static_cast<unsigned>(dr->second));
                    deadKeyedRows.erase(dr);
                    ++claimedCount;
                    ++claimedByClass[acls];
                    continue;
                }
            }
            if (ue_wrap::prop::IsTrashBitsPile(ke.actor)) {
                coop::trash_pile_sync::NotifyWireDestroy(liveKey);      // unwatch before the death, as above
            }
            doomed.push_back(ke.actor);
            doomedClass.push_back(acls);
            ++doomedByClass[acls];
        }
        if (idxUniverse > 0 || idxEvicted > 0) {
            UE_LOGI("join_membership_sweep: keyed index-half -- %d element-less keyed in universe "
                    "(%d claimed, %d stale entries evicted)", idxUniverse, idxClaimed, idxEvicted);
        }
    }
    // A row still in deadKeyedRows is a wire identity whose actor churn-died with no same-key
    // re-create. Under the join barrier wire expressions land only in a settled world, so this
    // should not happen; it is named per key so a log read localises it.
    for (const auto& [dkey, deid] : deadKeyedRows) {
        UE_LOGW("join_membership_sweep: dead keyed mirror row SURVIVED the re-bind pass -- eid=%u key='%s' "
                "has no churn re-create; this identity stays invisible here until the host re-expresses it "
                "(unexpected under the join barrier -- report)",
                static_cast<unsigned>(deid), dkey.c_str());
    }
    if (!keylessSkippedByClass.empty()) {
        size_t skippedTotal = 0;
        for (const auto& [k, v] : keylessSkippedByClass) skippedTotal += v;
        std::wstring topCls;
        int topCnt = 0;
        for (const auto& [k, v] : keylessSkippedByClass) {
            if (v > topCnt) { topCnt = v; topCls = k; }
        }
        UE_LOGW("join_membership_sweep: sweep SKIPPED %zu keyless unclaimed actor(s) across %zu class(es) "
                "(top: %d x '%ls') -- expected ~0 post-quiescence; a non-zero keyed-later count would "
                "mean the quiescence gate fired too early (regression tripwire)",
                skippedTotal, keylessSkippedByClass.size(), topCnt, topCls.c_str());
    }
    // The per-class completeness floor. The >50% valve is global, so a whole-class wipe slips under
    // it when the class is a minority of the world (every pile gone at 31% of all props). The floor
    // uses a positive signal, the host's independent GUObjectArray census (snapshot_census): a
    // doomed actor whose class the host reported more of than this bracket claimed is kept, since
    // the snapshot for that class is incomplete. Exact, not a percentage, so a legitimate clear
    // (host has 0, no census entry) still dooms. Applied before the valve, so the valve sees the
    // genuine remainder.
    if (coop::snapshot_census::HasCensus() && !doomed.empty() &&
        !coop::dev::force_overdestroy_test::FloorDisabledForTest()) {
        std::vector<void*> keptDoomed;
        std::vector<std::wstring> keptDoomedClass;
        keptDoomed.reserve(doomed.size());
        keptDoomedClass.reserve(doomed.size());
        std::unordered_map<std::wstring, int> floorKeptByClass;
        for (size_t i = 0; i < doomed.size(); ++i) {
            const std::wstring& c = doomedClass[i];
            const int hostHas = coop::snapshot_census::HostCountForClass(c);
            const auto cit = claimedByClass.find(c);
            const int claimedOfC = (cit == claimedByClass.end()) ? 0 : cit->second;
            if (hostHas > 0 && claimedOfC < hostHas) {
                ++floorKeptByClass[c];   // incomplete snapshot for class c -> KEEP, do not doom
                continue;
            }
            keptDoomed.push_back(doomed[i]);
            keptDoomedClass.push_back(c);
        }
        if (keptDoomed.size() != doomed.size()) {
            doomedByClass.clear();   // rebuild the histogram from the surviving doomed set
            for (const auto& c : keptDoomedClass) ++doomedByClass[c];
            size_t keptTotal = 0;
            std::wstring topCls;
            int topCnt = 0;
            for (const auto& [c, v] : floorKeptByClass) {
                keptTotal += static_cast<size_t>(v);
                if (v > topCnt) { topCnt = v; topCls = c; }
                const int hostHas = coop::snapshot_census::HostCountForClass(c);
                const auto cit = claimedByClass.find(c);
                const int claimedOfC = (cit == claimedByClass.end()) ? 0 : cit->second;
                UE_LOGW("join_membership_sweep: completeness FLOOR kept %d unclaimed '%ls' -- host census %d, "
                        "claimed only %d this bracket (INCOMPLETE snapshot, NOT a divergence; docs/piles/10 guard)",
                        v, c.c_str(), hostHas, claimedOfC);
            }
            UE_LOGW("join_membership_sweep: completeness floor KEPT %zu of %zu doomed actor(s) across %zu class(es) "
                    "(top: %d x '%ls') -- the host under-expressed these classes; the unclaimed locals SURVIVE",
                    keptTotal, doomed.size(), floorKeptByClass.size(), topCnt, topCls.c_str());
            doomed.swap(keptDoomed);
            doomedClass.swap(keptDoomedClass);
        }
    }

    // The valve: the client loaded the host's save, so a legitimate divergence is a small delta. A
    // sweep that would destroy more than half the universe is an incomplete snapshot (the host
    // re-seeded mid-connect and sent a tiny first bracket, racing the chunked drain), and
    // destroying on it wipes the just-loaded world. Abort instead: the claimed stay converged, the
    // unclaimed survive until a fuller bracket re-expresses them, and the host's PropDestroy stream
    // still removes real deletions.
    if (inClass > 0 && static_cast<int>(doomed.size()) * 2 > inClass) {
        UE_LOGW("join_membership_sweep: claim sweep ABORTED -- would destroy %zu of %d in-universe "
                "actor(s) (>50%%); the host snapshot is INCOMPLETE (partial/racing bracket), not a "
                "divergence. Keeping the loaded world (%d claimed stay converged).",
                doomed.size(), inClass, claimedCount);
        // The identity reconcile already ran at the fire edge, before this sweep. The deferred
        // queues survive the bracket; only the spawn-time index resets.
        g_claimedActors.clear();
        g_claimTrackingActive = false;
        coop::pile_spawn_bind::Reset();
        return;
    }

    // The destroys, with OnDestroy-parity teardown and echo-suppressed so the K2_DestroyActor
    // observer does not broadcast them; deferred=false, since this runs from the event_feed drain,
    // not inside a BP graph.
    for (void* a : doomed) {
        // One line per doomed actor with its class, key and position; the histogram below says what
        // died, this says which and where. Cold path, once per join.
        {
            const std::wstring dk = ue_wrap::prop::GetInteractableKeyString(a);
            const ue_wrap::FVector dl = ue_wrap::engine::GetActorLocation(a);
            UE_LOGI("join_membership_sweep:   dooming '%ls' key='%ls' loc=(%.1f,%.1f,%.1f)",
                    R::ClassNameOf(a).c_str(), dk.c_str(), dl.X, dl.Y, dl.Z);
        }
        coop::remote_prop::ClearAnyDriveFor(a);
        ue_wrap::engine::ReleaseMainPlayerGrabIfHolding(localPlayer, a);
        coop::prop_lifecycle::DestroyLocalProp(a, /*deferred=*/false);
    }
    UE_LOGI("join_membership_sweep: claim sweep -- %d in-universe actors live, "
            "%d claimed (expressed on the wire this bracket), %d unclaimed locals destroyed "
            "(client adopts host world)",
            inClass, claimedCount, static_cast<int>(doomed.size()));
    // The doomed-class histogram: a wrongly-universed class shows up here, not as a delayed crash.
    {
        std::vector<std::pair<std::wstring, int>> hist(doomedByClass.begin(), doomedByClass.end());
        std::sort(hist.begin(), hist.end(),
                  [](const auto& l, const auto& r) { return l.second > r.second; });
        int shown = 0;
        for (const auto& [hcls, cnt] : hist) {
            if (++shown > 10) {
                UE_LOGI("join_membership_sweep:   doomed ... +%zu more classes",
                        hist.size() - 10);
                break;
            }
            UE_LOGI("join_membership_sweep:   doomed %d x '%ls'", cnt, hcls.c_str());
        }
    }

    // The orphan census, read-only: the native level chipPiles no arriving proxy claimed within 1
    // cm. The bracket is drained before the sweep fires (Begin, every PropSpawn and Complete are
    // FIFO on one lane; Complete only arms), so a leftover is real host drift: the host moved or
    // collected the pile since the save the client loaded. The registry doom above misses these (a
    // level native enters the Prop registry lazily), and a player can grab one through the real
    // interaction system with no GrabIntent sent. Each orphan is banded by its distance to the
    // nearest live pile proxy, so a removal threshold comes from measured drift. This diverges from
    // MTA, which removes only by id (Packet_EntityRemove): a chipPile is keyless and level-placed,
    // so position is the only key. The summary prints at zero orphans too, or a clean join looks
    // like a census that never ran. LogCensus re-enumerates with fresh indices: a mass purge runs
    // right at the sweep and churns the GUObjectArray, so stored indices false-negative on every
    // survivor. One array walk per join.
    coop::pile_spawn_bind::LogCensus();
    // The kerfur off-to-active retire is RunReconcile's step 3; when no bracket armed (a lost
    // SnapshotBegin) quiescence_drain::OnTick covers it every client tick.

    g_claimedActors.clear();
    g_claimTrackingActive = false;
    coop::pile_spawn_bind::Reset();  // the candidate pointers would dangle past the GC below; the deferred reconcile queues survive
    // The engine's own purge follows the mass destroy, as after a level transition: otherwise the
    // pending-kill actors and the bracket's transients sit until the periodic purge, a client RSS
    // plateau over 10 GB that grazed the process commit cap. Under the join cover, the hitch is
    // invisible.
    if (!ue_wrap::engine::ForceGarbageCollection()) {
        UE_LOGW("join_membership_sweep: post-sweep CollectGarbage unresolved -- relying on the engine's periodic purge");
    }
}

void ArmDivergenceSweep() {
    if (!g_claimTrackingActive) {
        // A SnapshotComplete without its Begin does not arm a sweep with no claim set behind it.
        UE_LOGW("join_membership_sweep: divergence sweep arm requested but tracking not armed -- skipping");
        return;
    }
    // The sweep is deferred; claim tracking stays armed so a host PropSpawn or a client
    // self-announce during the quiesce window still claims its actor. A re-arm happens only on a
    // genuine world transition, and the membership bound plus the valve keep that re-fire safe.
    g_sweepPending = true;
    g_sweepFired = false;
    g_sweepArmedAt = std::chrono::steady_clock::now();
    // A fresh probe session: with the join barrier the world was settled at the announce, so this
    // normally latches after the minimum stability window (about 2 s); it exists for a load tail
    // resuming between the announce and SnapshotComplete (a late straggler wave, a purge under the
    // bracket). The probe owns stability, purge awareness and the deadlines.
    coop::world_load_episode::ArmQuiesceProbe("post-snapshot sweep gate");
    UE_LOGI("join_membership_sweep: divergence sweep ARMED -- deferring to the load-tail "
            "quiescence latch (world_load_episode probe session)");
}

void TickClientReconcile() {
    // The steady-state identity reconcile runs every tick, self-gated (a quiescence bool, a
    // pending-work bool, a 250 ms debounce); it walks the array only when a save pile moved after a
    // twin was armed. Below it is the join one-shot, zero cost when disarmed.
    coop::element::quiescence_drain::OnTick();
    // The one probe owner, also driven from net_pump for the announce gate; double-driving is
    // throttled inside.
    const bool quiesced = coop::world_load_episode::TickQuiesceProbe();
    if (!g_sweepPending) {
        // The lost-bracket backstop (kBracketFlakeMs): quiescence declared without a doom sweep, no
        // claims exist on this path. Client only: the probe arms only on client paths.
        if (!g_sweepFired && !g_claimTrackingActive && quiesced &&
            coop::world_load_episode::MsSinceQuiesced() > kBracketFlakeMs) {
            g_sweepFired = true;
            UE_LOGW("join_membership_sweep: no snapshot bracket within %d s of the world-ready "
                    "announce (SnapshotBegin lost / host wedged?) -- declaring load-tail quiescence "
                    "WITHOUT a doom sweep so the deferred reconcile queues drain via the steady tick",
                    kBracketFlakeMs / 1000);
            // The flake also shortens the reconcile window to the bound the header promises.
            coop::world_load_episode::NoteBracketFlake();
        }
        return;  // zero cost when disarmed (the steady state)
    }
    UE_ASSERT_GAME_THREAD("join_membership_sweep::TickClientReconcile");  // no-mutex: all sweep state is GT-only
    // A deadline latch arrives here like a stable one (degraded, and logged loud by the probe).
    if (!quiesced) return;

    // The local player is re-resolved here (a pointer stashed at arm time could go stale); the
    // sweep releases its grab if a doomed actor is held. One FindObjectByClass, cold.
    void* localPlayer = R::FindObjectByClass(P::name::MainPlayerClass);
    const auto msSinceArm = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - g_sweepArmedAt).count();
    UE_LOGI("join_membership_sweep: divergence sweep FIRING (probe latched; %lldms after arm)",
            static_cast<long long>(msSinceArm));
    g_sweepPending = false;
    g_sweepFired = true;  // load tail drained -> npc_adoption may now fresh-spawn no-twin save NPCs
    coop::save_identity_bind::ForceSaveChurnForTest();  // dev: a synthetic unbind so the re-bind lane runs with N > 0
    // The deferred reconcile runs before the doom adjudication with claim tracking still armed, so
    // a reconcile that converge-binds a re-create claims it and the sweep spares it. Doom judges
    // last.
    coop::element::quiescence_drain::RunReconcile();
    // A save-transfer join goes through two level loads, and a keyed prop re-created by the second
    // (or by GC churn) reuses a freed GUObjectArray slot: NumObjects stays flat and the steady
    // re-seed's grew-gate never indexes it, so it is invisible to the membership here and survives
    // as a duplicate (the joining client's starting suitcase, twice). The key-index membership is
    // refreshed at this one cold per-join point, so the re-create is either re-bind-spared or
    // doomed, leaving exactly one. One array walk per join.
    coop::prop_element_tracker::ReSeedKnownKeyedProps(nullptr);
    RunDivergenceSweep_(localPlayer);
    // The read-only probes and the bind summary report at quiescence.
    coop::dev::spawn_order_probe::EmitVerdictAtQuiescence();
    coop::dev::join_window_pos_trace::EmitVerdictAtQuiescence();
    coop::save_identity_bind::EmitBindSummary();
    // The sweep just destroyed the join-window ghosts and duplicates, so every still-hidden
    // survivor is revealed and the deferred-hide window closes; the destroyed are liveness-skipped
    // inside mirror_defer.
    coop::mirror_defer::RevealAllSurvivorsAtQuiescence();
}

bool IsInDivergenceUniverseUnclaimed(void* actor) {
    // The universe membership test without the armed precondition: an unclaimed, in-universe keyed
    // Aprop the host has not expressed. Shared by IsPendingSweepCandidate and the pre-quiescence
    // grab guard in trash_collect_sync, so there is one definition of the lineage.
    if (!actor) return false;
    if (g_claimedActors.count(actor)) return false;  // host-expressed / self-claimed legit drop -> not a ghost
    void* cls = R::ClassOf(actor);
    if (!ue_wrap::prop::IsClassKeyedInteractable(cls)) return false;  // out of the divergence universe
    if (!R::IsLive(actor)) return false;
    if (ue_wrap::prop::IsChipPile(actor)) return false;  // pile has its own collect/share + grab-hook destroy path
    if (coop::prop_lifecycle::IsPerPlayerPropClass(R::ClassNameOf(actor))) return false;  // per-player: never swept
    // A kerfur prop mirror is host-driven state (its host-range eid is bound when the convert or
    // adoption materialises it), never a save-loaded local: exempt, like the chipPile and the
    // per-player classes. The sweep itself never sees a mirror; this serves the grab guard, which
    // is handed an arbitrary actor.
    if (coop::kerfur_entity::GetKerfurMirrorEidForActor(actor) != coop::element::kInvalidId) return false;
    return true;  // unclaimed in-universe keyed Aprop = a divergence candidate awaiting adjudication
}

bool IsPendingSweepCandidate(void* actor) {
    // A sweep is armed and this actor is one of its candidates.
    return g_sweepPending && IsInDivergenceUniverseUnclaimed(actor);
}

bool HasLoadTailQuiesced() { return g_sweepFired; }

void OnClientWorldReadyResetSweep() {
    if (g_sweepPending)
        UE_LOGI("join_membership_sweep: client world-ready -- cancelling a pending divergence sweep "
                "from the prior world (the new snapshot bracket will re-arm it)");
    g_sweepPending = false;
    g_sweepFired = false;
}

void ResetClaimTracking() {
    if (g_claimTrackingActive) {
        UE_LOGI("join_membership_sweep: claim tracking reset mid-snapshot (disconnect) -- "
                "%zu claims dropped, no sweep", g_claimedActors.size());
    }
    g_claimedActors.clear();
    g_claimTrackingActive = false;
    // A mid-snapshot drop also cancels a deferred sweep, or the tick driver would fire it against a
    // torn-down world.
    g_sweepPending = false;
    g_sweepFired = false;
    coop::pile_spawn_bind::Reset();  // session teardown: drop the spawn-time index (dangling-pointer hygiene)
    coop::element::quiescence_drain::Reset();  // session teardown: drop the deferred reconcile queues (the ONLY site that clears them)
    coop::kerfur_reconcile::Reset();  // scope A: drop any unconsumed save-time kerfur retire across sessions
    coop::mirror_defer::Reset();  // instant-world: reveal any still-hidden mirror + disarm the deferred-hide window
    coop::world_load_episode::Reset();  // clear the world-load episode across sessions
}

// OnSpawn gates the level-pile twin destroy on an open bracket.
bool IsClaimTrackingActive() { return g_claimTrackingActive; }

// OnSpawn hands the claim set to pile_spawn_bind's twin destroy and adopt, so a claimed native is
// skipped.
const std::unordered_set<void*>& ClaimedActors() { return g_claimedActors; }

}  // namespace coop::join_membership_sweep

// coop/props/join_membership_sweep.cpp -- see join_membership_sweep.h.
//
// EXTRACTED from remote_prop_spawn.cpp 2026-06-30 (anti-smear: that file was over the 1500 LOC hard cap and
// held TWO concepts -- the wire PropSpawn RECEIVER (OnSpawn, stays there) AND this membership-claim +
// divergence-sweep). The moved code is byte-for-byte identical; only the namespace changed
// (coop::remote_prop_spawn -> coop::join_membership_sweep) and the OnSpawn seam became the public
// RecordClaimIfTracking / IsClaimTrackingActive entry points. [[feedback-folder-per-domain-concept-rule]]

#include "coop/props/join_membership_sweep.h"

#include "coop/creatures/kerfur_entity.h"
#include "coop/creatures/kerfur_reconcile.h"
#include "coop/creatures/npc_sync.h"
#include "coop/dev/force_overdestroy_test.h"
#include "coop/dev/spawn_order_probe.h"
#include "coop/element/element.h"
#include "coop/element/mirror_managers.h"  // PropMirrors (keyed churn re-bind: dead-actor mirror-row census)
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
#include "coop/session/mirror_defer.h"
#include "ue_wrap/engine.h"
#include "ue_wrap/hot_path_guard.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/sdk_profile.h"

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
// ---- P2 claim tracking state (2026-06-10) -------------------------------
// Game-thread only: armed/recorded/swept exclusively from the event_feed
// drain (SnapshotBegin / PropSpawn / SnapshotComplete dispatch inline on the
// GT) + the net_pump disconnect edge. No mutex needed. See the design block
// in remote_prop_spawn.h.
std::unordered_set<void*> g_claimedActors;
bool g_claimTrackingActive = false;

// ---- Deferred divergence sweep (quiescence-gated; the kerfur-savetransfer
// ghost fix 2026-06-15). The sweep no longer runs inline at SnapshotComplete --
// see ArmDivergenceSweep/TickClientReconcile + the design note in the header.
// All game-thread-only (the client tick + the event_feed drain), no mutex.
bool g_sweepPending = false;                          // armed at SnapshotComplete; cleared when the sweep runs
bool g_sweepFired   = false;                          // sticky: set when the sweep runs, until re-armed (HasLoadTailQuiesced)
// (g_sweepReconciled, the 2026-06-17 reconcile-once latch, was RETIRED by R1+R3 on
// 2026-06-18: R1 stopped the steady re-seed re-bracketing -- the dominant re-arm --
// and R3's membership + the >50% valve make any remaining re-fire bounded + safe. See
// the note in ArmDivergenceSweep. RULE 2: no latch, no parallel band-aid.)
std::chrono::steady_clock::time_point g_sweepArmedAt{};
std::chrono::steady_clock::time_point g_sweepLastProgressAt{};  // reset on any progress (active purge OR moving
                                                                // population) -> base of the NO-PROGRESS deadline
std::chrono::steady_clock::time_point g_sweepLastScan{};  // {} (epoch 0) => not yet scanned this arm
int  g_sweepLastUnsettledCount = -1;
int  g_sweepStableScans        = 0;
constexpr int kSweepScanIntervalMs = 200;    // 5 Hz quiescence probe while pending (matches npc_adoption)
constexpr int kSweepQuiesceScans   = 10;     // load-tail population (keyless props, chipPiles, AND allowlisted
                                             // NPCs) stable across 10 scans (~2 s) = the async loadObjects pass
                                             // has drained. Longer than the old 600 ms: the kerfur NPCs load with
                                             // multi-hundred-ms gaps after the props, so a short window false-
                                             // signals mid-load -> the adoption fresh-spawns a duplicate of a twin
                                             // still to come (the 2026-06-15 kerfur-dupe).
// Two-tier deadline (2026-06-25, the docs/piles/10 purge-aware gate). The sweep must adjudicate the FULLY
// reloaded world, but a join-time world-reload mass-purge drains async over ~50 s (14:11 hands-on: arm ->
// re-seed-done ~50 s) -- a fixed SINCE-ARM deadline would fire INSIDE the purge trough before the re-seed,
// never exercising the floor (the exact self-sabotage this gate avoids). So:
//   - kSweepDeadlineMs: a NO-PROGRESS timer (base = g_sweepLastProgressAt, reset whenever a purge is draining
//     OR the load-tail population is still moving). It fires only after this long with NOTHING happening --
//     a genuine stall, never a legitimate long drain. Same 45 s value, new semantics.
//   - kSweepHardCapMs: an ABSOLUTE ceiling from arm. Bounds a pathological stuck-purge-flag (no-progress
//     keeps resetting) so the reconcile can never defer forever. Curtain is already down at Complete, so this
//     only delays the INVISIBLE reconcile, not player control -> a generous 120 s is safe.
constexpr int kSweepDeadlineMs     = 45000;   // NO-PROGRESS deadline (since last progress, not since arm)
constexpr int kSweepHardCapMs      = 120000;  // absolute ceiling since arm (stuck-purge backstop)

// Record that the host snapshot bound `actor` (exact-key, fuzzy, or fresh
// spawn) -- the actor is accounted-for by the host's world and must survive
// the sweep. No-op when tracking is disarmed (live PropSpawns outside a join
// cost one bool read).
void RecordClaim(void* actor) {
    if (!g_claimTrackingActive || !actor) return;
    g_claimedActors.insert(actor);
}
}  // namespace (file-local claim/sweep state)

void RecordClaimIfTracking(void* actor) {
    // Fork B 2c (2026-06-10): public claim primitive for the SELF-announce
    // sites (Init POST / takeObj POST / held-item / convert broadcasts). A
    // client announcing a prop while a bracket is open creates a live
    // in-universe actor the host's already-enumerated bracket cannot express
    // -- without a claim the sweep would destroy the announcer's own prop at
    // SnapshotComplete while the host keeps the mirror. Claims = "entities
    // expressed on the wire this bracket, in EITHER direction".
    RecordClaim(actor);
}

void BeginClaimTracking() {
    g_claimedActors.clear();
    g_claimTrackingActive = true;
    // A fresh bracket supersedes any sweep still pending from a prior bracket
    // (the two-level-load case): cancel it so it can't fire against this new
    // claim set; SnapshotComplete re-arms it. Clear the sticky fired latch too
    // (the new world's load tail has not quiesced yet).
    g_sweepPending = false;
    g_sweepFired = false;
    g_sweepStableScans = 0;
    g_sweepLastUnsettledCount = -1;
    // A fresh bracket re-enumerates pile-bind candidates (a re-bracket runs
    // against the post-sweep world; stale entries would be dead pointers).
    coop::pile_spawn_bind::Reset();  // index only -- the deferred reconcile queues (quiescence_drain) survive the bracket
    // Phase 0: drop any completeness census from a prior bracket; SnapshotComplete delivers this
    // bracket's. Until then HostCountForClass returns -1 (the floor is a no-op -> >50% valve only).
    coop::snapshot_census::Reset();
    // Phase 1 step 1A probe: arm the read-only keyless load-spawn coverage recorder for this join.
    coop::dev::spawn_order_probe::ArmForJoin();
    UE_LOGI("join_membership_sweep: claim tracking ARMED (snapshot bracket open) -- "
            "unclaimed in-universe locals will be destroyed at SnapshotComplete");
}

// The one real sweep. Driven (deferred) by TickClientReconcile once the load
// tail has quiesced -- NOT inline at SnapshotComplete (see ArmDivergenceSweep).
// Internal linkage: the only callers are ArmDivergenceSweep's tick driver.
static void RunDivergenceSweep_(void* localPlayer) {
    if (!g_claimTrackingActive) {
        // A SnapshotComplete without its Begin (wire anomaly / disconnect race)
        // must NOT sweep with an empty claim set -- that would destroy every
        // in-universe actor including legitimately claimed ones.
        UE_LOGW("join_membership_sweep: claim sweep requested but tracking is not armed -- skipping");
        return;
    }
    // Fork B 2e (2026-06-10): SWEEP(client) == EXPRESS(host) + CLIENT-
    // FORBIDDEN. The sweep may destroy exactly what the host snapshot can
    // express a binding for -- keyed IsClassKeyedInteractable actors + the
    // keyless chipPile lineage (eid-expressed since HALF 1) -- plus the
    // wire-suppressed intermediate the client already destroys-on-sight
    // while connected (mushroom7: keyed, in-universe, never expressed ->
    // swept; parity with the connected-state destroy + wire-ingress drop).
    // Everything keyless that is NOT chipPile-lineage (a held clump
    // mid-flight, a pre-Init Aprop_C, event clumps whose setKey never
    // sticks) is OUT of universe and never swept. The pre-2e 4-class
    // RNG-divergent scope could neither destroy the orphan props a host
    // re-bracket no longer expresses (intact cubicles, moved-wall doubles)
    // nor protect a flying keyless clump mirror -- both fixed by the
    // universe test. (IsRngDivergentClass + ResolveRngDivergentBases
    // retired with it, RULE 2.)
    //
    // (The "watched-pile interaction" caveat once noted here is GONE 2026-06-17 with the pile
    // death-watch retirement -- there are no watched piles for the sweep to spare anymore; a pile
    // re-grab is handled by the InpActEvt_use observer, independent of this sweep + its claim set.)
    //
    // R3 (2026-06-18, MTA CElementGroup membership). The doom candidates are the
    // client's OWN LOCAL Prop Elements -- its save-loaded world -- enumerated from the
    // tracked Prop registry, NOT a GUObjectArray scan of every keyed-interactable in
    // existence. This is MTA's deletion-by-tracked-membership (Server/.../
    // CElementGroup.cpp:28 iterates the explicit member list, never the world): we only
    // ever adjudicate what THIS client loaded. Two old guards collapse for free:
    //   - host-driven wire MIRRORS (pr.mirror) are excluded at the SOURCE. A kerfur prop
    //     mirror -- or any host-expressed prop -- is never a save-load divergence, so the
    //     old per-actor kerfur-mirror exemption is now automatic (RULE 2: it goes).
    //   - keyless NON-pile transients (a held clump mid-flight, a pre-Init Aprop) are
    //     never MarkPropElement'd -> never LOCAL Prop Elements -> never enumerated here,
    //     so the old scan's keyless-skip is structurally unreachable (the defensive skip
    //     below stays only as a ~0 tripwire).
    // The >50%% valve STAYS (the agent's "membership collapses it" was wrong -- fewer
    // host claims means MORE unclaimed, not fewer): membership bounds the set to "what
    // you loaded", but an INCOMPLETE host bracket still leaves most of it unclaimed ->
    // dooming most of the loaded world. That is an incomplete snapshot, not a divergence
    // -- the valve aborts it (below). Per-player state actors ARE local Prop Elements, so
    // their exemption stays too. ONE registry snapshot (no per-actor mutex; the eid/idx/
    // mirror flag are captured under the Registry lock), ZERO GUObjectArray walks.
    std::vector<coop::element::Registry::ActorIdPair> propPairs;
    coop::element::Registry::Get().SnapshotActorsByType(
        coop::element::ElementType::Prop, propPairs);
    // KEYED CHURN RE-BIND (2026-07-03, docs/piles/12 -- the eid=2947 upstream). A keyed prop whose actor GC-
    // churned mid-join leaves its MIRROR row holding a dead pointer while the game re-creates a same-key twin;
    // the re-create gets a fresh LOCAL element and lands in the doom set below as "unclaimed" -- but its KEY
    // matches an already-expressed identity, so dooming it destroys a host-known entity (13:33:43 "wire
    // destroy ... unwatched"; the dead row then mis-resolves a recycled address = the wedge). Collect the
    // dead-actor keyed mirror rows ONCE; the doom loop re-binds a candidate onto its orphaned row instead of
    // dooming it (the chipPile analog is the position re-bind in save_identity_bind -- this is the keyed lane).
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
        // ClassNameOf moved AHEAD of the claimed check (Phase 0): the completeness floor needs the
        // claimed count PER CLASS, so the claimed branch must tag its class too.
        const std::wstring acls = R::ClassNameOf(a);
        if (g_claimedActors.count(a)) { ++claimedCount; ++claimedByClass[acls]; continue; }  // host-expressed / self-claimed -> converged, keep
        // PER-PLAYER state actors (inventory container etc.) are this player's own
        // per-save state -- never host-expressed AND never swept. The 2026-06-10 smoke
        // swept the client's inventory container and fataled at the next GC purge. STAYS:
        // a per-player prop IS a local Prop Element, so membership alone would doom it.
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
        // KEYED CHURN RE-BIND (see the deadKeyedRows build above): this unclaimed keyed local's KEY names an
        // already-expressed identity whose mirror row lost its actor to churn -> it IS that identity's
        // re-create. Re-bind the row onto it (the same drain-local-element + rebindInPlace path every save
        // bind takes) and count it CLAIMED -- never doom a host-known entity for having churned.
        if (!deadKeyedRows.empty()) {
            auto dr = deadKeyedRows.find(narrowAscii(key));
            if (dr != deadKeyedRows.end()) {
                coop::prop_element_tracker::UnmarkKnownKeyedProp(a);  // drain the re-create's fresh LOCAL element
                coop::remote_prop::RegisterPropMirror(dr->second, a, key, acls, /*senderSlot*/ 0,
                                                      /*rebindInPlace*/ true);
                UE_LOGW("join_membership_sweep: keyed churn RE-BIND -- unclaimed '%ls' key='%ls' is the "
                        "re-create of already-expressed eid=%u (mirror row held a dead actor) -> row rebound, "
                        "actor claimed, NOT doomed (docs/piles/12 eid=2947 upstream)",
                        acls.c_str(), key.c_str(), static_cast<unsigned>(dr->second));
                deadKeyedRows.erase(dr);
                ++claimedCount;
                ++claimedByClass[acls];
                continue;
            }
        }
        // v57 audit CRIT-2: a SWEPT trashBitsPile must be unwatched from the
        // counter channel BEFORE it dies -- the sweep runs AFTER join_progress
        // ::Complete() (Idle) in the same drain, so the next Tick's death-watch
        // would see a near-camera vanish with inTransition=false and broadcast
        // a keyed PropDestroy for a pile the HOST legitimately still has.
        if (ue_wrap::prop::IsTrashBitsPile(a)) {
            coop::trash_pile_sync::NotifyWireDestroy(key);
        }
        doomed.push_back(a);
        doomedClass.push_back(acls);
        ++doomedByClass[acls];
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
    // PHASE 0 PER-CLASS COMPLETENESS FLOOR (2026-06-25, docs/COOP_STABLE_ID_SIDECAR.md S4 -- the
    // docs/piles/10 over-destroy guard). The >50% valve below is GLOBAL, so a whole-class wipe slips
    // under it whenever that class is a minority of the world (11:16: 100% of 870 piles = 31% of all
    // props -> no abort -> ALL piles vanished). This floor is PER-CLASS and uses a POSITIVE signal:
    // the host's INDEPENDENT GUObjectArray census (snapshot_census, NOT the registry the expression
    // path used). For each doomed actor, if the host reported a live count for its class AND we
    // claimed FEWER than that, the snapshot for the class is INCOMPLETE -> KEEP (the missing
    // expressions are in flight or failed; dooming wipes genuine objects). EXACT, not a percentage:
    // it keeps "host expressed 0 of 870" yet still dooms a legitimate clear (host genuinely has 0 ->
    // no census entry / count 0 -> claimed >= count -> doomed as a real deletion). A class with no
    // census entry is unaffected (the >50% valve still guards it). Applied BEFORE the valve so the
    // valve sees the genuine-divergence remainder.
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

    // SAFETY VALVE (2026-06-15, post-live-save regression). A legitimate divergence
    // is a SMALL delta: the client loaded the host's save, so the host cannot have
    // changed more than a fraction of the world between that save and this join. A
    // sweep that wants to destroy MORE THAN HALF the in-universe props is therefore
    // NOT divergence -- it is an incomplete snapshot (the host re-seeded mid-connect
    // and sent a tiny first bracket, racing the chunked drain), and destroying on it
    // wipes the just-loaded world (the 2026-06-15 hands-on: 2979 of 3229 destroyed).
    // ABORT instead -- the claimed props stay converged; the unclaimed survive (a
    // later fuller bracket re-expresses them, and the host's PropDestroy stream still
    // removes genuine deletions). A few stale ghosts beat an empty world. The
    // live-capture path skips the sweep entirely (event_feed); this guards every
    // OTHER path (stale fallback / fresh boot) against the same class of bug.
    if (inClass > 0 && static_cast<int>(doomed.size()) * 2 > inClass) {
        UE_LOGW("join_membership_sweep: claim sweep ABORTED -- would destroy %zu of %d in-universe "
                "actor(s) (>50%%); the host snapshot is INCOMPLETE (partial/racing bracket), not a "
                "divergence. Keeping the loaded world (%d claimed stay converged).",
                doomed.size(), inClass, claimedCount);
        // (sync-refactor 2026-06-27) The membership DOOM-destroy is aborted (incomplete snapshot), but the
        // IDENTITY reconcile must STILL run -- it is per-eid, armed-bounded (re-bind/retire by stable
        // position/eid), NEVER a mass-destroy, so it is safe even on an incomplete world. Skipping it here
        // was why variant-1 / twin-retire / b3 never ran on a save-transfer join with a mass-purge (the
        // 15:25 verify: valve aborted -> reconcile skipped -> purge-churned natives left unbound). Run it
        // (The deferred twins/corrections/destroys SURVIVE this bracket now -- they drain at the steady-state
        // tick; only the spawn-time index is reset here. Anti-smear split 2026-06-30.)
        coop::element::quiescence_drain::RunReconcile(/*joinSweep=*/false);
        g_claimedActors.clear();
        g_claimTrackingActive = false;
        coop::pile_spawn_bind::Reset();
        return;
    }

    // Phase 3: destroy with OnDestroy-parity teardown. Echo-suppressed
    // (MarkIncomingDestroy) so our K2_DestroyActor PRE observer does not
    // broadcast these local-only teardowns. deferred=false: we run from the
    // event_feed drain on the game thread, not inside a BP graph.
    for (void* a : doomed) {
        coop::remote_prop::ClearAnyDriveFor(a);
        ue_wrap::engine::ReleaseMainPlayerGrabIfHolding(localPlayer, a);
        coop::prop_lifecycle::DestroyLocalProp(a, /*deferred=*/false);
    }
    UE_LOGI("join_membership_sweep: claim sweep -- %d in-universe actors live, "
            "%d claimed (expressed on the wire this bracket), %d unclaimed locals destroyed "
            "(client adopts host world)",
            inClass, claimedCount, static_cast<int>(doomed.size()));
    // Doomed-class histogram (audit ask): the sweep must NAME what it kills
    // -- a wrongly-universed class shows up here, not as a delayed crash.
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
    // R1 + R3 retired the reconcile-once latch that lived here (g_sweepReconciled):
    // R1 stopped the steady-world re-seed from re-bracketing (the dominant ~10x/join
    // re-arm = the churn the latch band-aided), and R3's membership enumeration + the
    // >50%% valve make any REMAINING re-fire (only a genuine world transition re-brackets
    // now) bounded and safe -- a re-sweep can no longer thrash the world or doom mirrors
    // (mirrors are excluded at the source). So there is nothing left to latch against.

    // ---- L1 ORPHAN CENSUS (2026-06-23, READ-ONLY -- insertion #2). We are now post-quiescence
    // (g_sweepFired set above), post-burst, and past the >50%% valve. The leftover g_pileBindIndex
    // entries are native level-chipPiles that NO arriving proxy claimed within 1cm. The PropSpawn
    // bracket is PROVABLY drained before this sweep fires (in-lane FIFO Begin->[every PropSpawn]->
    // Complete on Lane::Bulk; SnapshotComplete only ARMS the sweep -- agent-verified 2026-06-23),
    // so a leftover is a real host-DRIFT orphan: the host MOVED the pile (its proxy is elsewhere)
    // or COLLECTED it (no proxy) since the save the client loaded. These are EXACTLY the orphans
    // the sweep's Registry doom above MISSES -- a level-native enters the Prop Registry lazily, so
    // SnapshotActorsByType(Prop) never enumerates it. This is the L1 hole: a human aims at one of
    // these surviving natives, grabs it through the REAL interaction system (it is a real
    // actorChipPile_C, not our proxy), so no GrabIntent is sent and the host never sees the grab.
    //
    // READ-ONLY for now (absence-removal is Phase 2): band each orphan by the distance to the
    // nearest live PILE-form proxy + log the histogram, so the removal thresholds come from REAL
    // host-drift data (measure before cut). DIVERGES from MTA, which removes purely by ID/
    // membership (Client/.../CPacketHandler.cpp Packet_EntityRemove deletes only the exact IDs the
    // server names, never by position): our chipPiles are KEYLESS + deterministically level-placed,
    // so the host pile and the client native share NO cross-peer id -> position is the only key.
    // MTA's client starts EMPTY (it never holds a local save), so it never has this orphan class;
    // the position+valve approach is the justified adaptation (RULE 2026-05-28 divergence note --
    // the >50%% valve is the partial-snapshot guard MTA gets for free from its empty-start + JOINED
    // gate). Cold path (once per join), bounded (a few dozen leftovers x a proxy-set walk each).
    // ALWAYS log when the index was built this bracket -- even 0 orphans. A CLEAN join drains the index
    // to EMPTY (every twin matched within 1cm), and gating on non-empty made a 0-orphan join SILENT --
    // indistinguishable from a census that never ran (the exact ambiguity the 2026-06-23 clean same-machine
    // smoke hit: 869 built, all matched, no [PILE-CENSUS] line -> looked broken). The summary line is the
    // proof the census ran + the count; N=0 on a clean join is the expected, INFORMATIVE result.
    // FRESH walk at the sweep (GC-ROBUST) -- do NOT re-use g_pileBindIndex's build-time internal indices.
    // The 2026-06-23 calibration proved why: a mass-purge runs right at the sweep (prop_element_tracker
    // reaps 256 dead Prop Elements/call, draining the join-tail backlog -- log-confirmed at the SAME second
    // as the sweep, repeating every ~4s), churning the GUObjectArray so every stored internalIdx goes STALE
    // -> IsLiveByIndex(actor, idx) false-negatives on every survivor -> "0 live of 17" while the drift had
    // seeded 8 orphans. So re-enumerate live native chipPiles with FRESH indices: the burst's 1cm twin-
    // destroy already removed every MATCHED native, so the live survivors ARE the orphan set directly.
    // One GUObjectArray walk, once per join (cold path), pointer-compare class filter before any read.
    // v86 Path 1c: retry the save-time twin-destroys that MISSED at the world-ready burst (the moved
    // pile's native@old had not async-loaded yet). We are now post-quiescence (this whole sweep is gated
    // by TickClientReconcile on kSweepQuiesceScans stable scans) + past the >50%% valve above, so the late
    // natives are present (the census below SEES them). Keyed by save-time position (not blind proximity),
    // with its own >50% abort-valve. Runs BEFORE the census so the census reflects the removals.
    // The join-window reconcile (twin-retire -> re-bind -> kerfur-retire -> deferred-destroy -> b3 pos-correction
    // -> census) is the ONE order owner coop::element::quiescence_drain::RunReconcile, driven HERE at the join-window
    // quiescence sweep AND by the steady-state coop::element::quiescence_drain::OnTick (the D1 structural fix: the
    // mechanisms were join-window-one-shot, so a save-pile grabbed/moved AFTER this sweep armed a twin nothing
    // consumed = the 15:01:49 ghost). joinSweep=true logs the one-shot orphan census. See coop/element/quiescence_drain.h.
    coop::element::quiescence_drain::RunReconcile(/*joinSweep=*/true);
    // NOTE: the kerfur off->active retire sweep (scope A) IS driven here now -- it is step 3 of RunReconcile
    // (anti-smear 2026-06-30; it used to be a separate driver in kerfur_convert::PollKerfurConversions = a 3rd
    // parallel order owner, now removed). The bracket-not-armed case (SnapshotBegin-lost flake leaves
    // g_sweepPending false) is covered by quiescence_drain::OnTick, which runs every client tick before the
    // g_sweepPending gate and ORs kerfur_reconcile::HasPendingRetire into its HasPendingWork. [[feedback-one-owner-order-axis]]

    g_claimedActors.clear();
    g_claimTrackingActive = false;
    coop::pile_spawn_bind::Reset();  // bracket over: drop candidate pointers (would dangle past the GC below). The
                                     // deferred reconcile queues (quiescence_drain) survive -> the steady tick drains them.
    // Pair the mass destruction with the engine's own purge (end-of-frame
    // CollectGarbage, the post-level-transition pattern). Without it the
    // sweep's pending-kill actors + the ~3k-spawn bracket's transients sit
    // until UE's 61 s periodic purge -- smoke-measured as a 10.6 GB client
    // RSS plateau that grazed the 12 GB process commit cap. Runs under the
    // join cover; the purge hitch is invisible.
    if (!ue_wrap::engine::ForceGarbageCollection()) {
        UE_LOGW("join_membership_sweep: post-sweep CollectGarbage unresolved -- relying on the engine's periodic purge");
    }
}

// Load-tail quiescence probe for the deferred sweep + the NPC adoption (both gate on
// HasLoadTailQuiesced). Counts two populations whose sum settles exactly when the
// async loadObjects pass finishes materializing the world:
//   (a) ALLOWLISTED NPCs (the kerfur load tail). Counted live, non-CDO, tracked OR
//       untracked -- adoption/binding does NOT change the count (the actor stays live +
//       allowlisted), so only loadObjects spawning a new twin perturbs it. This is the
//       half the old prop-only probe MISSED: the kerfur NPCs respawn SECONDS after the
//       props' keys mint (2026-06-15 hands-on), so a prop-only quiescence fired before
//       the twins existed and the adoption fresh-spawned a duplicate next to the still-
//       loading twin. Genuinely-absent twins (a host kerfur turned on after capture)
//       have no local actor, so they do not perturb the count -> never block quiescence.
//   (b) Keyless, UNCLAIMED, in-universe, non-per-player, non-chipPile props that read
//       Key=None -- the prop load tail (a straggler that has not minted its key yet,
//       exactly the set the sweep's keyless-skip would spare).
// While either tail is still loading the sum CHANGES; when it stops changing for
// kSweepQuiesceScans the load has drained. ONE GUObjectArray walk (pure pointer-compare
// class filters before any key/name read), throttled to 5 Hz, only while a sweep pends.
static int CountLoadTailUnsettled_() {
    const int32_t n = R::NumObjects();
    int unsettled = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        // (a) allowlisted-NPC load tail (lineage test first; the NameOf/IsLive only run
        // for the handful that pass it). A false return (allowlist not yet resolved)
        // just degrades to prop-only quiescence -- never worse than the old behavior.
        if (coop::npc_sync::IsAllowlistedClass(cls)) {
            if (!R::IsLive(obj)) continue;
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // CDO
            ++unsettled;
            continue;
        }
        // (b) keyless-prop load tail (the original probe population).
        if (!ue_wrap::prop::IsClassKeyedInteractable(cls)) continue;
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // CDO (alloc-free)
        // (b') chipPile field (docs/piles/10 purge-aware gate, 2026-06-25). COUNT live chipPiles into the
        // population (was a `continue` skip): a join-time world-reload purge DROPS the field (871 -> ...) and
        // the async reload CLIMBS it back (... -> 870), so counting it makes the gate refuse to quiesce
        // through EITHER half of the reload -- including the <=4 s window where the purge has physically
        // started but InPurgeEpisode is not yet flagged (the reaper is on a 4 s throttle). This is the
        // PHYSICAL-population half of the fix; it does not depend on the flag's detection latency. Counted
        // EVEN IF claimed (a claimed-then-purged pile still perturbs the field), so it sits ABOVE the
        // g_claimedActors skip below. Shared with NPC adoption (HasLoadTailQuiesced): making adoption also
        // wait for pile stability is conservative + correct (piles + kerfur NPCs load in the same async pass).
        if (ue_wrap::prop::IsChipPile(obj)) { ++unsettled; continue; }
        if (g_claimedActors.count(obj)) continue;                       // claimed: never a sweep candidate
        if (coop::prop_lifecycle::IsPerPlayerPropClass(R::ClassNameOf(obj))) continue;
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(obj);
        if (key.empty() || key == L"None") ++unsettled;
    }
    return unsettled;
}

void ArmDivergenceSweep() {
    if (!g_claimTrackingActive) {
        // SnapshotComplete without its Begin (wire anomaly / disconnect race) --
        // do not arm a sweep with no claim set behind it.
        UE_LOGW("join_membership_sweep: divergence sweep arm requested but tracking not armed -- skipping");
        return;
    }
    // (The 2026-06-17 reconcile-once latch that gated here -- g_sweepReconciled -- is
    // RETIRED by R1+R3. It band-aided the join-churn: the host's steady-world re-seed
    // re-bracketed ~10x/join, each re-arming this sweep, which re-doomed unclaimed locals
    // -- thrashing piles + repeatedly dooming kerfur mirrors. R1 fixed that at the SOURCE
    // (steady re-seed now broadcasts bracket-free incremental PropSpawns -- no re-bracket,
    // no re-arm). The only re-entry left is a genuine world transition (cave/level travel),
    // where re-reconciling against the NEW world is CORRECT, and R3's membership enumeration
    // + the >50%% valve keep that re-fire bounded + non-churning (mirrors excluded at the
    // source; can't wipe the world). So we always proceed to arm. RULE 2: the latch is gone.)
    // Defer the one real sweep. Claim tracking stays armed (NOT disarmed here) so
    // any host PropSpawn / client self-announce during the quiesce window still
    // claims its actor via RecordClaim and is spared.
    g_sweepPending = true;
    g_sweepFired = false;
    g_sweepArmedAt = std::chrono::steady_clock::now();
    g_sweepLastProgressAt = g_sweepArmedAt;  // no-progress deadline base; no generation capture -- a clean
                                             // no-purge join (boot seed holds, population already stable) must
                                             // be allowed to quiesce immediately.
    g_sweepLastScan = {};            // force a quiescence scan on the next tick
    g_sweepLastUnsettledCount = -1;
    g_sweepStableScans = 0;
    UE_LOGI("join_membership_sweep: divergence sweep ARMED -- deferring to load-tail quiescence "
            "(keyless-prop + allowlisted-NPC population stable x%d scans @%dms, or %dms hard deadline)",
            kSweepQuiesceScans, kSweepScanIntervalMs, kSweepDeadlineMs);
}

void TickClientReconcile() {
    // Steady-state identity reconcile (D1 structural fix, sync-refactor 2026-06-27): runs EVERY tick, even
    // when the join one-shot is disarmed -- it self-gates cheaply (a quiescence bool + a pending-work bool +
    // a 250 ms debounce) and only walks the array when a save-pile grabbed/moved after the join sweep armed a
    // twin. Below this line is the join-window one-shot trigger (disarmed = zero cost). See coop/sync.
    coop::element::quiescence_drain::OnTick();
    if (!g_sweepPending) return;  // zero cost when disarmed (the steady state)
    UE_ASSERT_GAME_THREAD("join_membership_sweep::TickClientReconcile");  // no-mutex: all sweep state is GT-only
    const auto now = std::chrono::steady_clock::now();
    const auto msSince = [now](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count();
    };
    // Throttle to ~5 Hz (skip the throttle on the very first scan after arming,
    // when g_sweepLastScan is the default-constructed epoch).
    if (g_sweepLastScan.time_since_epoch().count() != 0 &&
        msSince(g_sweepLastScan) < kSweepScanIntervalMs) return;
    g_sweepLastScan = now;

    // Two-tier deadline (docs/piles/10 purge-aware gate): the NO-PROGRESS timer (since the last progress edge)
    // OR the ABSOLUTE ceiling (since arm). The absolute ceiling fires even through a stuck purge so the
    // reconcile can never defer forever; the no-progress timer never pre-empts a legitimately-draining purge
    // (which keeps resetting g_sweepLastProgressAt below).
    const bool deadlineHit = msSince(g_sweepArmedAt) >= kSweepHardCapMs ||
                             msSince(g_sweepLastProgressAt) >= kSweepDeadlineMs;
    if (!deadlineHit) {
        // REGISTRY MID-PURGE (or never seeded) -> the loaded world is INCOMPLETE; never adjudicate against it.
        // A draining purge IS progress (reset the no-progress base) so a ~50 s legitimate reload does not trip
        // the deadline; the population-stability run restarts once the world re-seeds (!InPurgeEpisode is the
        // clean "registry re-seeded" edge -- the episode-end re-seed is synchronous, net_pump.cpp:538-539).
        if (!coop::prop_element_tracker::HasSeededOnce() ||
            coop::prop_element_tracker::InPurgeEpisode()) {
            g_sweepLastProgressAt = now;
            g_sweepLastUnsettledCount = -1;
            g_sweepStableScans = 0;
            return;  // keep waiting (the absolute ceiling above still bounds a permanently-stuck purge)
        }
        const int unsettled = CountLoadTailUnsettled_();
        if (unsettled != g_sweepLastUnsettledCount) {
            g_sweepLastUnsettledCount = unsettled;
            g_sweepLastProgressAt = now;  // population still moving = progress -> hold off the no-progress deadline
            g_sweepStableScans = 0;
            return;  // population still changing -> load tail (props, piles, or NPCs) not drained, keep waiting
        }
        if (++g_sweepStableScans < kSweepQuiesceScans) return;  // stable, but not for long enough yet
    }

    // Gate satisfied (quiesced or deadline) -- run the one real sweep. Re-resolve
    // the live local player here (a pointer stashed at arm time could go stale; the
    // sweep needs it to release the grav-hand if a doomed actor is being held --
    // exactly the kerfur-ghost-grab case). Cold path, one FindObjectByClass.
    void* localPlayer = R::FindObjectByClass(P::name::MainPlayerClass);
    const char* fireReason =
        !deadlineHit                                       ? "load tail quiesced"
        : (msSince(g_sweepArmedAt) >= kSweepHardCapMs)     ? "ABSOLUTE ceiling (stuck purge backstop)"
                                                           : "no-progress deadline";
    UE_LOGI("join_membership_sweep: divergence sweep FIRING (%s; %lldms after arm, %lldms since last progress)",
            fireReason, static_cast<long long>(msSince(g_sweepArmedAt)),
            static_cast<long long>(msSince(g_sweepLastProgressAt)));
    g_sweepPending = false;
    g_sweepFired = true;  // load tail drained -> npc_adoption may now fresh-spawn no-twin save NPCs
    coop::save_identity_bind::ForceSaveChurnForTest();  // [dev] force_save_churn: synthetic unbind so variant-1 runs N>0 (verify probe)
    RunDivergenceSweep_(localPlayer);
    // Phase 1 step 1A probe: load tail has quiesced -> emit the keyless-spawn coverage verdict (read-only).
    coop::dev::spawn_order_probe::EmitVerdictAtQuiescence();
    // Phase 1 step 2b bind: load tail has quiesced -> emit the eid-range bind summary (bound count, case i/ii).
    coop::save_identity_bind::EmitBindSummary();
    // instant-world quiescence BACKSTOP: the sweep just destroyed the join-window ghosts/dups, so reveal
    // every still-hidden survivor (the held tail + anything spawned after the curtain-lift) and close the
    // deferred-hide window. Ghosts destroyed above are liveness-skipped inside mirror_defer. Worst case this
    // is exactly today's end-state -- the backup is untouched; this only un-hides what it resolved.
    coop::mirror_defer::RevealAllSurvivorsAtQuiescence();
}

bool IsInDivergenceUniverseUnclaimed(void* actor) {
    // The divergence-universe membership test WITHOUT the g_sweepPending precondition: an UNCLAIMED,
    // in-universe, keyed Aprop the host has not expressed -- a save-loaded local awaiting adjudication
    // against the host snapshot. Used by IsPendingSweepCandidate (a sweep is armed) AND by the
    // pre-quiescence join-window grab guard in trash_collect_sync (the sweep is not yet armed) so both
    // share ONE membership definition (RULE 2 -- no second copy of this lineage logic).
    if (!actor) return false;
    if (g_claimedActors.count(actor)) return false;  // host-expressed / self-claimed legit drop -> not a ghost
    void* cls = R::ClassOf(actor);
    if (!ue_wrap::prop::IsClassKeyedInteractable(cls)) return false;  // out of the divergence universe
    if (!R::IsLive(actor)) return false;
    if (ue_wrap::prop::IsChipPile(actor)) return false;  // pile has its own collect/share + grab-hook destroy path
    if (coop::prop_lifecycle::IsPerPlayerPropClass(R::ClassNameOf(actor))) return false;  // per-player: never swept
    // A kerfur prop MIRROR is host-driven state (its host-range eid is bound when the convert/adoption
    // materializes it), NEVER a save-loaded local awaiting adjudication -- so it is not a divergence
    // candidate for any caller of THIS predicate (IsPendingSweepCandidate + the trash_collect pre-
    // quiescence grab guards). (The divergence SWEEP itself no longer needs this: since R3 it enumerates
    // only LOCAL Prop Elements via SnapshotActorsByType and excludes host-driven mirrors at the source
    // with pr.mirror -- so this exemption now serves ONLY the grab-guard predicate, which is handed an
    // arbitrary actor and must still recognize a kerfur mirror.) Exempt it like the chipPile / per-player
    // ones. (2026-06-17 kerfur join fix; the reconcile-once gate it companioned was retired by R1+R3.)
    if (coop::kerfur_entity::GetKerfurMirrorEidForActor(actor) != coop::element::kInvalidId) return false;
    return true;  // unclaimed in-universe keyed Aprop = a divergence candidate awaiting adjudication
}

bool IsPendingSweepCandidate(void* actor) {
    // A divergence sweep is armed AND this actor is one of its candidates.
    return g_sweepPending && IsInDivergenceUniverseUnclaimed(actor);
}

bool HasLoadTailQuiesced() { return g_sweepFired; }

void OnClientWorldReadyResetSweep() {
    if (g_sweepPending)
        UE_LOGI("join_membership_sweep: client world-ready -- cancelling a pending divergence sweep "
                "from the prior world (the new snapshot bracket will re-arm it)");
    g_sweepPending = false;
    g_sweepFired = false;
    g_sweepStableScans = 0;
    g_sweepLastUnsettledCount = -1;
}

void ResetClaimTracking() {
    if (g_claimTrackingActive) {
        UE_LOGI("join_membership_sweep: claim tracking reset mid-snapshot (disconnect) -- "
                "%zu claims dropped, no sweep", g_claimedActors.size());
    }
    g_claimedActors.clear();
    g_claimTrackingActive = false;
    // A mid-snapshot drop must also cancel any deferred sweep armed this session
    // (the tick driver would otherwise fire it against a torn-down world).
    g_sweepPending = false;
    g_sweepFired = false;
    g_sweepStableScans = 0;
    g_sweepLastUnsettledCount = -1;
    coop::pile_spawn_bind::Reset();  // session teardown: drop the spawn-time index (dangling-pointer hygiene)
    coop::element::quiescence_drain::Reset();  // session teardown: drop the deferred reconcile queues (the ONLY site that clears them)
    coop::kerfur_reconcile::Reset();  // scope A: drop any unconsumed save-time kerfur retire across sessions
    coop::mirror_defer::Reset();  // instant-world: reveal any still-hidden mirror + disarm the deferred-hide window
}

// OnSpawn gates a level-pile twin-destroy on an open bracket; expose the file-local flag for that one seam.
bool IsClaimTrackingActive() { return g_claimTrackingActive; }

// OnSpawn passes the claim set (read-only) to pile_spawn_bind's twin-destroy / adopt so a claimed native is
// skipped. Exposed by reference (the set is file-local above).
const std::unordered_set<void*>& ClaimedActors() { return g_claimedActors; }

}  // namespace coop::join_membership_sweep

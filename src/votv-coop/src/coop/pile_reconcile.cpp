// coop/pile_reconcile.cpp -- join-bracket keyless-pile reconcile (see header).
//
// EXTRACTED from coop/remote_prop_spawn.cpp 2026-06-23. Behavior preserved
// byte-for-byte; the only shape change is that TryDestroyTwin takes the match
// position as a PARAMETER (the in-line code matched the proxy's current pose;
// v86 Path 1c passes the pile's save-time position instead -- the caller chooses).

#include "coop/pile_reconcile.h"

#include "coop/ini_config.h"  // IsIniKeyTrue -- the [PILE-DELTA] probe flag (votv-coop.ini [dev], not bats/env)
#include "coop/prop_element_tracker.h"  // UnmarkKnownKeyedProp / GetPropElementIdForActor
#include "coop/trash_proxy.h"  // NearestPileProxy (the census)
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>   // getenv -- the read-only [PILE-DELTA] probe gate (L1 orphan histogram)
#include <unordered_map>
#include <vector>

namespace coop::pile_reconcile {
namespace {

namespace R = ue_wrap::reflection;

// ---- Keyless-pile position-bind index (v56 follow-up, 2026-06-10) --------
// With the save-transfer join the client's world is LOADED FROM THE HOST'S
// OWN SAVE: its chipPiles are the same piles, settled at the same positions.
// Binding the host's keyless eid expression to the client's OWN local pile
// (instead of sweep-destroying all ~870 and fresh-spawning mirrors) is now
// SOUND. Pre-save-transfer worlds had per-peer RNG pile layouts -- the very
// reason the eidOnly lane historically skipped local matching.
//
// Bracket-scoped, built lazily ONCE per bracket on the first keyless-pile
// expression (one GUObjectArray walk -- NOT one walk per pile; the ~870-
// expression burst would otherwise rescan 870x, the exact storm the
// dedupeFellBack self-heal guards against). Game-thread only (the
// event_feed drain), like the claim set.
struct PileBindCandidate {
    void*   actor;
    int32_t idx;  // InternalIndex captured at build time -- bind-time liveness is
                  // IsLiveByIndex (no deref of a possibly-GC-freed pointer; the
                  // index lives for the whole multi-second bracket)
    float   x, y, z;
    uint8_t chipType;
};
std::vector<PileBindCandidate> g_pileBindIndex;
bool g_pileBindIndexBuilt = false;
int  g_pileBindCount = 0;  // per-bracket bind counter (throttles the log)

// v86 Path 1c -- save-time twin-destroys that MISSED at the world-ready snapshot burst
// (the moved pile's native@save-time-key had not async-loaded/indexed yet -- world-ready
// fires before the native-pile load-tail drains). Keyed by host eid; retried at the post-
// quiescence sweep (SweepReconcileSaveTimeTwins) where the late native is present.
struct PendingTwin {
    float   x, y, z;     // the save-time key (the native@old position to destroy)
    uint8_t chipType;
};
std::unordered_map<uint32_t, PendingTwin> g_pendingSaveTimeTwin;
int  g_pileIndexBuiltCount = 0;  // size of g_pileBindIndex at build (the L1 orphan-census valve denominator:
                                 // leftovers / built = the host-drift fraction; a huge fraction = wire loss,
                                 // not divergence -> the census/removal must refuse it, like the >50%% sweep valve)

// [PILE-DELTA] / per-orphan [PILE-CENSUS] probe gate (L1 orphan histogram), read ONCE + cached. Ships dark
// (off => zero cost). When on, logs the per-orphan nearest-proxy/native deltas so we can band the host-drift
// orphans (0-5cm near-miss vs >30cm true drift). HANDS-ON FLAG: lives in votv-coop.ini [dev]
// `pile_delta_probe=1` (the established probe pattern -- perf_probe/leak_probe -- the user toggles in the ini,
// NOT in the launch bats). The env is kept ONLY as the autonomous mp.py-harness override.
// [[feedback-test-flags-in-ini-not-bats-or-env]]
bool DeltaProbeOn() {
    static const bool on = coop::ini_config::IsIniKeyTrue("pile_delta_probe") || [] {
        const char* v = std::getenv("VOTVCOOP_PILE_DELTA_PROBE");
        return v && v[0] && v[0] != '0';
    }();
    return on;
}

// Build the bracket index lazily (idempotent). `claimed` = remote_prop_spawn's
// g_claimedActors (skip a native already bound earlier this bracket).
void EnsureIndex(const std::unordered_set<void*>& claimed) {
    if (g_pileBindIndexBuilt) return;
    g_pileBindIndexBuilt = true;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        if (!ue_wrap::prop::IsChipPile(obj)) continue;  // lineage test, pure pointer walks
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // CDO
        if (claimed.count(obj)) continue;  // already bound earlier this bracket
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(obj);
        // chipType read once at build time: save-loaded piles carry it from the
        // save (both peers loaded the SAME save, so host==client). If a pile
        // class ever set it lazily post-load, the equality gate would miss --
        // a harmless fallback to fresh-spawn+sweep, never a wrong bind.
        g_pileBindIndex.push_back(
            {obj, R::InternalIndexOf(obj), loc.X, loc.Y, loc.Z,
             ue_wrap::prop::GetChipType(obj)});
    }
    g_pileIndexBuiltCount = static_cast<int>(g_pileBindIndex.size());  // census valve denominator (pre-consume size)
    UE_LOGI("pile_reconcile: pile-bind index built -- %zu local chipPile candidate(s)",
            g_pileBindIndex.size());
}

}  // namespace

void Reset() {
    g_pileBindIndex.clear();
    g_pileBindIndex.shrink_to_fit();
    g_pileBindIndexBuilt = false;
    g_pileBindCount = 0;
    g_pileIndexBuiltCount = 0;
    g_pendingSaveTimeTwin.clear();
}

void TryDestroyTwin(const coop::net::PropSpawnPayload& payload,
                    const ue_wrap::FVector& matchPos,
                    bool isSaveTimeKey,
                    const std::unordered_set<void*>& claimed) {
    EnsureIndex(claimed);
    constexpr float kDestroyR2Cm = 1.0f;  // 1 cm^2 -- the probe confirmed a bit-exact (1cm) twin
    int matchCount = 0, matchIdx = -1;
    for (int i = 0; i < static_cast<int>(g_pileBindIndex.size()); ++i) {
        const auto& c = g_pileBindIndex[i];
        const float dx = c.x - matchPos.X, dy = c.y - matchPos.Y, dz = c.z - matchPos.Z;
        if (dx * dx + dy * dy + dz * dz > kDestroyR2Cm) continue;
        if (c.chipType != payload.chipType) continue;          // same trash variant only
        if (!R::IsLiveByIndex(c.actor, c.idx)) continue;       // bracket-long raw ptr: no deref first
        ++matchCount;
        matchIdx = i;
    }
    if (matchCount == 1) {
        void* native = g_pileBindIndex[matchIdx].actor;
        g_pileBindIndex[matchIdx] = g_pileBindIndex.back();   // O(1) remove (consume the twin)
        g_pileBindIndex.pop_back();
        // Drop its client-minted eid from the tracker FIRST so the K2_DestroyActor PRE observer
        // stays silent (keyless + no eid) -- no stray PropDestroy on the superseded client eid
        // (the same fresh-mirror invariant the adopt-bind path enforces, audit 2026-06-10).
        coop::prop_element_tracker::UnmarkKnownKeyedProp(native);
        ue_wrap::engine::DestroyActor(native);
        if (g_pileBindCount < 8 || (g_pileBindCount % 200) == 0)
            UE_LOGI("[PILE] DESTROY native level-pile twin eid=%u at (%.1f,%.1f,%.1f) chipType=%u -- "
                    "proxy is the sole mirror now (dup fixed; %zu native(s) left in index)",
                    payload.elementId, matchPos.X, matchPos.Y, matchPos.Z,
                    static_cast<unsigned>(payload.chipType), g_pileBindIndex.size());
        ++g_pileBindCount;
    } else if (matchCount > 1) {
        UE_LOGW("[PILE] DESTROY SKIP eid=%u at (%.1f,%.1f,%.1f) -- %d native chipPile twins within "
                "1cm (ambiguous cluster) -> keeping all (never destroy the wrong one); dup may persist",
                payload.elementId, matchPos.X, matchPos.Y, matchPos.Z, matchCount);
    }
    // matchCount == 0: no twin within 1cm of matchPos in the CURRENT index.
    else if (matchCount == 0) {
        // v86 Path 1c: if matchPos was a SAVE-TIME key (a stamped pile that SHOULD have a save-loaded
        // twin), the miss is almost always TIMING -- this runs in the world-ready snapshot burst, BEFORE
        // the client's async native-pile load-tail has drained, so the native@save-time-key has not
        // loaded/indexed yet (it appears ~10s later, at the post-quiescence sweep). Record it for a retry
        // there (SweepReconcileSaveTimeTwins), where the late native is present. (A non-save-time miss is a
        // genuine DERIVED pile with no twin -- the common gameplay case -- so do NOT record it.)
        if (isSaveTimeKey && payload.elementId != 0)
            g_pendingSaveTimeTwin[payload.elementId] =
                PendingTwin{matchPos.X, matchPos.Y, matchPos.Z, payload.chipType};
    }
    // [PILE-DELTA] dark probe: during the join bracket every proxy is a LEVEL pile, so a no-match here
    // is a host-DRIFT candidate (the native is not at the proxy's pose). Log its nearest-native delta so
    // the harness can band the orphans (read-only; no destroy, no index mutation).
    if (matchCount == 0 && DeltaProbeOn() && !g_pileBindIndex.empty()) {
        float bestD2 = 3.4e38f; int bestI = -1;
        for (int i = 0; i < static_cast<int>(g_pileBindIndex.size()); ++i) {
            const auto& c = g_pileBindIndex[i];
            if (!R::IsLiveByIndex(c.actor, c.idx)) continue;       // no deref of a GC'd ptr
            const float dx = c.x - matchPos.X, dy = c.y - matchPos.Y, dz = c.z - matchPos.Z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < bestD2) { bestD2 = d2; bestI = i; }
        }
        if (bestI >= 0) {
            const auto& c = g_pileBindIndex[bestI];
            // A native still IN the index is UNCLAIMED (a 1cm match pops it), so a nearby chipType-
            // matching entry is likely this pile's drifted twin; a >30cm nearest = a real orphan
            // (host removed/moved the pile far). nativeEid confirms FACT 1 (expect kInvalidId).
            const uint32_t nativeEid = static_cast<uint32_t>(
                coop::prop_element_tracker::GetPropElementIdForActor(c.actor));
            UE_LOGI("[PILE-DELTA] eid=%u matchPos=(%.1f,%.1f,%.1f) chipType=%u nearestNative_d=%.1fcm "
                    "nearestChipTypeMatch=%d nativeEid=%u",
                    payload.elementId, matchPos.X, matchPos.Y, matchPos.Z, static_cast<unsigned>(payload.chipType),
                    std::sqrt(bestD2), (c.chipType == payload.chipType) ? 1 : 0, nativeEid);
        } else {
            UE_LOGI("[PILE-DELTA] eid=%u matchPos=(%.1f,%.1f,%.1f) chipType=%u nearestNative_d=NONE "
                    "(no live native in the index)",
                    payload.elementId, matchPos.X, matchPos.Y, matchPos.Z, static_cast<unsigned>(payload.chipType));
        }
    }
}

void* FindAndConsumeAdoptCandidate(const coop::net::PropSpawnPayload& payload,
                                   const std::wstring& classW,
                                   const std::unordered_set<void*>& claimed,
                                   float* outD2, int* outBindSeq) {
    EnsureIndex(claimed);
    constexpr float kPileBindRadiusCm = 30.f;
    int best = -1;
    float bestD2 = kPileBindRadiusCm * kPileBindRadiusCm;
    for (int i = 0; i < static_cast<int>(g_pileBindIndex.size()); ++i) {
        const auto& c = g_pileBindIndex[i];
        const float dx = c.x - payload.locX;
        const float dy = c.y - payload.locY;
        const float dz = c.z - payload.locZ;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > bestD2) continue;
        if (c.chipType != payload.chipType) continue;
        if (claimed.count(c.actor)) continue;  // keyed lane claimed it meanwhile
        if (!R::IsLiveByIndex(c.actor, c.idx)) continue;  // bracket-long raw ptr: no deref before this
        if (!R::NameEquals(R::NameOf(R::ClassOf(c.actor)), classW.c_str())) continue;
        best = i;
        bestD2 = d2;
    }
    if (best >= 0) {
        void* pile = g_pileBindIndex[best].actor;
        g_pileBindIndex[best] = g_pileBindIndex.back();
        g_pileBindIndex.pop_back();
        if (outD2) *outD2 = bestD2;
        if (outBindSeq) *outBindSeq = ++g_pileBindCount;
        return pile;
    }
    return nullptr;
}

void LogCensus() {
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
    if (!g_pileBindIndexBuilt) return;
    int live = 0, le5 = 0, mid = 0, gt30 = 0, none = 0;
    int totalLive = 0, proxyMatched = 0;   // DIAGNOSTIC: distinguish "0 orphans because all natives DIED"
                                           // (totalLive==0) from "0 because every survivor is 1cm-proxy-matched"
                                           // (totalLive==proxyMatched). The 2026-06-23 same-machine drift hit
                                           // census=0 -- this says which: consumed (incl. wrong-consumption) vs no-divergence.
    const bool verbose = DeltaProbeOn();
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        if (!ue_wrap::prop::IsChipPile(o)) continue;                   // real actorChipPile_C only (NOT our proxy)
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;   // CDO
        ++totalLive;
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(o);
        float d = -1.f;
        void* prox = coop::trash_proxy::NearestPileProxy(loc, &d);
        const bool hasProx = (prox != nullptr && d >= 0.f);
        if (hasProx && d <= 1.0f) { ++proxyMatched; continue; }   // a 1cm-matched twin -> not an orphan
        ++live;
        // The >50%% valve early-returns on an incomplete snapshot before this census is reached, so the
        // proxy set is complete here -> "no proxy near" means the host genuinely has no pile there.
        if (!hasProx)        ++none;   // no host pile anywhere near      -> COLLECTED orphan
        else if (d <= 5.f)   ++le5;    // a proxy sits ~here              -> host pile settled slightly off = near-miss DUP
        else if (d <= 30.f)  ++mid;    // ambiguous (settle vs neighbour) -> watch this band
        else                 ++gt30;   // nearest host pile is far        -> MOVED / true orphan
        if (verbose) {
            const unsigned ct = static_cast<unsigned>(ue_wrap::prop::GetChipType(o));
            if (hasProx)
                UE_LOGI("[PILE-CENSUS] orphan native @(%.1f,%.1f,%.1f) chipType=%u nearestProxy_d=%.1fcm",
                        loc.X, loc.Y, loc.Z, ct, d);
            else
                UE_LOGI("[PILE-CENSUS] orphan native @(%.1f,%.1f,%.1f) chipType=%u nearestProxy_d=NONE",
                        loc.X, loc.Y, loc.Z, ct);
        }
    }
    UE_LOGI("[PILE-CENSUS] %d live orphan native(s) (of %d built, FRESH walk; totalLiveNatives=%d proxyMatched<=1cm=%d): "
            "le5=%d (near-miss dup) 5_30=%d (ambiguous) gt30=%d (true orphan/moved) noProxy=%d (collected) -- "
            "[totalLive==0 => all natives DIED/consumed; totalLive>0 & orphans=0 => survivors all proxy-matched]",
            live, g_pileIndexBuiltCount, totalLive, proxyMatched, le5, mid, gt30, none);
}

int SweepReconcileSaveTimeTwins() {
    if (g_pendingSaveTimeTwin.empty()) return 0;
    const size_t pendingN = g_pendingSaveTimeTwin.size();

    // FRESH GC-robust walk of live native chipPiles (the world-ready index is stale: built BEFORE
    // these natives async-loaded; the same reason LogCensus re-walks). Pointer-compare class filter
    // before any read; no stored internal indices (a sweep-time mass-purge stales them).
    struct LiveNative { void* actor; int32_t idx; float x, y, z; uint8_t chipType; bool consumed; };
    std::vector<LiveNative> natives;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        if (!ue_wrap::prop::IsChipPile(o)) continue;                   // real actorChipPile_C only (NOT our proxy)
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;   // CDO
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(o);
        natives.push_back({o, R::InternalIndexOf(o), loc.X, loc.Y, loc.Z, ue_wrap::prop::GetChipType(o), false});
    }

    // Match each pending save-time key to the now-loaded native within 1cm + same chipType. Claim-track
    // (consumed) so two keys never claim one native. 1cm is exact (the save round-trip is bit-for-bit;
    // the key IS the native's loaded position -- proven by the 2026-06-23 hands-on: census orphan @old
    // == matchKey @old to 0.1cm).
    constexpr float kR2Cm = 1.0f;
    std::vector<void*> toRemove;
    toRemove.reserve(pendingN);
    for (const auto& [eid, p] : g_pendingSaveTimeTwin) {
        for (auto& nv : natives) {
            if (nv.consumed) continue;
            if (nv.chipType != p.chipType) continue;
            const float dx = nv.x - p.x, dy = nv.y - p.y, dz = nv.z - p.z;
            if (dx * dx + dy * dy + dz * dz > kR2Cm) continue;
            if (!R::IsLiveByIndex(nv.actor, nv.idx)) continue;
            nv.consumed = true;
            toRemove.push_back(nv.actor);
            break;
        }
    }

    // SAFETY VALVE (mirrors RunDivergenceSweep_'s >50% valve): removing MORE THAN HALF the live native
    // piles is not a handful of moved-in-window twins -- it is a racing/incomplete bracket where the
    // load-tail is STILL draining (so most "matches" would be wrong). Refuse it; the natives stay.
    if (!natives.empty() && static_cast<int>(toRemove.size()) * 2 > static_cast<int>(natives.size())) {
        UE_LOGW("[PILE-1C] sweep-reconcile ABORTED -- %zu save-time twin removals of %zu live native(s) "
                "(>50%%); racing/incomplete bracket, not divergence -- keeping all natives",
                toRemove.size(), natives.size());
        g_pendingSaveTimeTwin.clear();
        return 0;
    }

    for (void* native : toRemove) {
        // Drop the client-minted eid FIRST so the K2_DestroyActor PRE observer stays silent (no stray
        // PropDestroy on the superseded client eid) -- the same fresh-mirror invariant the world-ready
        // twin-destroy enforces.
        coop::prop_element_tracker::UnmarkKnownKeyedProp(native);
        ue_wrap::engine::DestroyActor(native);
    }
    UE_LOGI("[PILE-1C] sweep-reconcile -- %zu of %zu pending save-time twin(s) removed at post-quiescence "
            "(the late-loaded native@old destroyed; the proxy@new is the sole mirror -- join-window dup fixed)",
            toRemove.size(), pendingN);
    g_pendingSaveTimeTwin.clear();
    return static_cast<int>(toRemove.size());
}

}  // namespace coop::pile_reconcile

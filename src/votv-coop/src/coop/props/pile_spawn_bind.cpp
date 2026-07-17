// coop/props/pile_spawn_bind.cpp -- the pile spawn-time native-bind mechanism (see header).
//
// EXTRACTED from coop/props/pile_reconcile.cpp 2026-06-30 (anti-smear refactor): this is
// pile_reconcile's group A -- the bracket-scoped GUObjectArray pile-bind index + the two
// spawn-time bind paths (TryDestroyTwin / FindAndConsumeAdoptCandidate) + the [PILE-DELTA]
// dark probe. Behavior preserved byte-for-byte; the ONLY change is that the save-time-key
// MISS now ARMS the order owner (coop::element::quiescence_drain::ArmPendingSaveTimeTwin)
// across modules instead of writing a local pending map (which moved to quiescence_drain).
// [[feedback-one-owner-order-axis]]

#include "coop/props/pile_spawn_bind.h"

#include "coop/element/quiescence_drain.h"  // ArmPendingSaveTimeTwin (the spawn mechanism CAPTURES into the order owner)
#include "coop/config/config.h"  // IsIniKeyTrue -- the [PILE-DELTA] probe flag (votv-coop.ini [dev], not bats/env)
#include "coop/props/prop_element_tracker.h"  // IsBoundMirrorNative / GetPropElementIdForActor
#include "coop/props/save_time_retire_util.h"  // UnmarkAndDestroy + kExactMatchR2Cm (shared kernel)
#include "coop/props/trash_proxy.h"  // NearestPileProxy (the L1 orphan census)
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>   // getenv -- the read-only [PILE-DELTA] probe gate (L1 orphan histogram)
#include <vector>

namespace coop::pile_spawn_bind {
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
int  g_pileIndexBuiltCount = 0;  // size of g_pileBindIndex at build (the L1 orphan-census valve denominator:
                                 // leftovers / built = the host-drift fraction; a huge fraction = wire loss,
                                 // not divergence -> the census/removal must refuse it, like the >50%% sweep valve)

// [PILE-DELTA]/[PILE-CENSUS] probe gate (L1 orphan histogram), read ONCE + cached. Ships dark (off => zero
// cost). When on, logs the per-orphan nearest-proxy/native deltas so we can band the host-drift orphans
// (0-5cm near-miss vs >30cm true drift). HANDS-ON FLAG: votv-coop.ini [dev] `pile_delta_probe=1` (the
// established probe pattern; the user toggles the ini, NOT the launch bats). The env is the autonomous
// mp.py-harness override only. [[feedback-test-flags-in-ini-not-bats-or-env]] Used by TryDestroyTwin's
// delta-log AND LogCensus's verbose mode -- ONE concept, ONE gate, file-local to this module.
bool DeltaProbeOn() {
    static const bool on = coop::config::IsIniKeyTrue("pile_delta_probe") || [] {
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
        // (X) native-authoritative guard (b): a save_identity_bind BOUND native IS the authoritative
        // host-range mirror -- it must NEVER be a reconcile-destroy candidate. Excluding it from the index
        // here covers BOTH the world-ready TryDestroyTwin AND the adopt path (FindAndConsumeAdoptCandidate),
        // so a co-located UNBOUND pile's proxy can never 1cm-destroy a bound native (the end-to-end killer).
        if (coop::prop_element_tracker::IsBoundMirrorNative(obj)) continue;
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
    UE_LOGI("pile_spawn_bind: pile-bind index built -- %zu local chipPile candidate(s)",
            g_pileBindIndex.size());
}

}  // namespace

void Reset() {
    g_pileBindIndex.clear();
    g_pileBindIndex.shrink_to_fit();
    g_pileBindIndexBuilt = false;
    g_pileBindCount = 0;
    g_pileIndexBuiltCount = 0;
}

void TryDestroyTwin(const coop::net::PropSpawnPayload& payload,
                    const ue_wrap::FVector& matchPos,
                    bool isSaveTimeKey,
                    const std::unordered_set<void*>& claimed) {
    EnsureIndex(claimed);
    // Inline match (NOT save_time_retire_util::FindExactMatch): this path does an O(1) swap-pop CONSUME
    // from g_pileBindIndex on a match (lines below), which the non-mutating index-return kernel cannot
    // model; keep it inline. The 1cm + ambiguous(>1)->skip policy is the same as the shared kernel.
    constexpr float kDestroyR2Cm = coop::save_time_retire_util::kExactMatchR2Cm;  // 1 cm^2 -- bit-exact twin
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
        // (X) MED-2: the EnsureIndex bound-mirror skip runs at index-BUILD time; a native that binds AFTER
        // the (latched) build is still in the index. Re-check at the consume site so a bound native can never
        // be destroyed even if it bound late (cheap -- one map lookup on the single matched candidate).
        if (coop::prop_element_tracker::IsBoundMirrorNative(native)) return;
        // Drop its client-minted eid from the tracker FIRST so the K2_DestroyActor PRE observer
        // stays silent (keyless + no eid) -- no stray PropDestroy on the superseded client eid
        // (the same fresh-mirror invariant the adopt-bind path enforces, audit 2026-06-10).
        coop::save_time_retire_util::UnmarkAndDestroy(native);
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
        // loaded/indexed yet (it appears ~10s later, at the post-quiescence sweep). ARM it on the ORDER
        // OWNER for a retry there (quiescence_drain::SweepReconcileSaveTimeTwins), where the late native is
        // present. (A non-save-time miss is a genuine DERIVED pile with no twin -- the common gameplay case
        // -- so do NOT record it.) The spawn mechanism only CAPTURES; the order owner drains.
        if (isSaveTimeKey && payload.elementId != 0)
            coop::element::quiescence_drain::ArmPendingSaveTimeTwin(payload.elementId, matchPos, payload.chipType);
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

}  // namespace coop::pile_spawn_bind

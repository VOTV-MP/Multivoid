// coop/props/pile_spawn_bind.cpp -- the pile spawn-time native-bind mechanism (see header).
//
// It holds the bracket-scoped GUObjectArray pile-bind index, the spawn-time bind
// (BindOwnSavePile) and the [PILE-DELTA] dark probe. A save-time-key MISS arms the order owner
// across modules
// (coop::element::quiescence_drain::ArmPendingSaveTimeTwin) rather than keeping a pending map
// of its own, so one module owns the order axis.

#include "coop/props/pile_spawn_bind.h"

#include "coop/element/quiescence_drain.h"  // ArmPendingSaveTimeTwin (the spawn mechanism CAPTURES into the order owner)
#include "coop/config/config.h"  // ResolveFlag -- the [PILE-DELTA] probe flag (multivoid.ini [dev], not bats/env)
#include "coop/element/registry.h"  // Element::SetSaveNative on the bound native
#include "coop/props/join_membership_sweep.h"  // RecordClaimIfTracking (a bound native is expressed this bracket)
#include "coop/props/prop_element_tracker.h"  // IsBoundMirrorNative / GetPropElementIdForActor
#include "coop/props/prop_wire_parity.h"  // RestoreCollisionIfNeeded
#include "coop/props/remote_prop.h"  // RegisterPropMirror
#include "coop/props/save_time_retire_util.h"  // kExactMatchR2Cm (the shared match radius)
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

// ---- Keyless-pile position-bind index -----------------------------------
// The save-transfer join loads the client's world FROM THE HOST'S OWN SAVE,
// so its chipPiles are the same piles settled at the same positions. That is
// what makes it sound to bind the host's keyless eid expression to the
// client's OWN local pile instead of sweep-destroying all ~870 and
// fresh-spawning mirrors: without it the layouts are per-peer and only an
// eid can match.
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
    bool    isClump;  // the entity's other resting form: a row binds a native of its own form only
};
std::vector<PileBindCandidate> g_pileBindIndex;
bool g_pileBindIndexBuilt = false;
int  g_pileBindCount = 0;  // per-bracket bind counter (throttles the log)

// This client's own clumps that a bind parked (tick and physics off). They are the CLIENT's actors
// and outlive the session in its world, so the disconnect gives both back. Session-scoped, not
// bracket-scoped: Reset leaves it alone.
struct ParkedClump { void* actor; int32_t idx; };
std::vector<ParkedClump> g_parkedClumps;
int  g_pileIndexBuiltCount = 0;  // size of g_pileBindIndex at build (the L1 orphan-census valve denominator:
                                 // leftovers / built = the host-drift fraction; a huge fraction = wire loss,
                                 // not divergence -> the census/removal must refuse it, like the >50%% sweep valve)

// [PILE-DELTA]/[PILE-CENSUS] probe gate (L1 orphan histogram), read ONCE + cached. Ships dark (off => zero
// cost). When on, logs the per-orphan nearest-native deltas so we can band the host-drift orphans
// (0-5cm near-miss vs >30cm true drift). HANDS-ON FLAG: multivoid.ini [dev] `pile_delta_probe=1` (the
// established probe pattern: the ini is the toggle, not the launch bats). The env var is the
// mp.py-harness override only. Used by the bind's miss delta-log AND LogCensus's verbose
// mode -- ONE concept, ONE gate, file-local to this module.
bool DeltaProbeOn() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::pile_delta_probe) || [] {
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
        const bool isClump = ue_wrap::prop::IsGarbageClump(obj);         // lineage tests, pure pointer walks
        if (!isClump && !ue_wrap::prop::IsChipPile(obj)) continue;
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // CDO
        if (claimed.count(obj)) continue;  // already bound earlier this bracket
        // Native-authoritative guard: a native already bound as a host-range mirror must never be
        // a candidate for a second eid, so a co-located UNBOUND pile's expression can never bind
        // over it.
        if (coop::prop_element_tracker::IsBoundMirrorNative(obj)) continue;
        ue_wrap::FVector loc{};
        if (!ue_wrap::engine::TryGetActorLocation(obj, loc)) continue;   // no read, no bind candidate
        // chipType read once at build time: save-loaded piles carry it from the
        // save (both peers loaded the SAME save, so host==client). If a pile
        // class ever set it lazily post-load, the equality gate would miss --
        // a harmless fallback to fresh-spawn+sweep, never a wrong bind.
        g_pileBindIndex.push_back(
            {obj, R::InternalIndexOf(obj), loc.X, loc.Y, loc.Z,
             ue_wrap::prop::GetChipType(obj), isClump});
    }
    g_pileIndexBuiltCount = static_cast<int>(g_pileBindIndex.size());  // census valve denominator (pre-consume size)
    int clumpN = 0;
    for (const auto& c : g_pileBindIndex) if (c.isClump) ++clumpN;
    UE_LOGI("pile_spawn_bind: pile-bind index built -- %zu local chipPile candidate(s), %d garbage clump(s)",
            g_pileBindIndex.size() - static_cast<size_t>(clumpN), clumpN);
}

// [PILE-DELTA] dark probe on a bind MISS: during the join bracket every expression names a LEVEL
// pile, so a no-match is a host-DRIFT candidate (the native is not at the expression's save-time
// pose). Log its nearest-native delta so the harness can band the orphans (read-only; no bind, no
// index mutation). A native still IN the index is unbound -- a 1 cm match pops it -- so a nearby
// chipType-matching entry is likely this pile's drifted twin, and a >30 cm nearest is a real
// orphan (the host removed or moved the pile far).
void LogNearestDelta(const coop::net::PropSpawnPayload& payload, const ue_wrap::FVector& matchPos) {
    if (!DeltaProbeOn() || g_pileBindIndex.empty()) return;
    float bestD2 = 3.4e38f; int bestI = -1;
    for (int i = 0; i < static_cast<int>(g_pileBindIndex.size()); ++i) {
        const auto& c = g_pileBindIndex[i];
        if (!R::IsLiveByIndex(c.actor, c.idx)) continue;       // no deref of a GC'd ptr
        const float dx = c.x - matchPos.X, dy = c.y - matchPos.Y, dz = c.z - matchPos.Z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; bestI = i; }
    }
    if (bestI < 0) {
        UE_LOGI("[PILE-DELTA] eid=%u matchPos=(%.1f,%.1f,%.1f) chipType=%u nearestNative_d=NONE "
                "(no live native in the index)",
                payload.elementId, matchPos.X, matchPos.Y, matchPos.Z, static_cast<unsigned>(payload.chipType));
        return;
    }
    const auto& c = g_pileBindIndex[bestI];
    const uint32_t nativeEid = static_cast<uint32_t>(
        coop::prop_element_tracker::GetPropElementIdForActor(c.actor));
    UE_LOGI("[PILE-DELTA] eid=%u matchPos=(%.1f,%.1f,%.1f) chipType=%u nearestNative_d=%.1fcm "
            "nearestChipTypeMatch=%d nativeEid=%u",
            payload.elementId, matchPos.X, matchPos.Y, matchPos.Z, static_cast<unsigned>(payload.chipType),
            std::sqrt(bestD2), (c.chipType == payload.chipType) ? 1 : 0, nativeEid);
}

}  // namespace

void Reset() {
    g_pileBindIndex.clear();
    g_pileBindIndex.shrink_to_fit();
    g_pileBindIndexBuilt = false;
    g_pileBindCount = 0;
    g_pileIndexBuiltCount = 0;
}

void* BindOwnSavePile(const coop::net::PropSpawnPayload& payload,
                      const std::wstring& classW,
                      const ue_wrap::FVector& matchPos,
                      bool isSaveTimeKey,
                      int senderSlot,
                      const std::unordered_set<void*>& claimed) {
    EnsureIndex(claimed);
    // Inline match (NOT save_time_retire_util::FindExactMatch): this path does an O(1) swap-pop CONSUME
    // from g_pileBindIndex on a match (lines below), which the non-mutating index-return kernel cannot
    // model; keep it inline. The 1cm + ambiguous(>1)->skip policy is the same as the shared kernel.
    constexpr float kBindR2Cm = coop::save_time_retire_util::kExactMatchR2Cm;  // 1 cm^2 -- bit-exact twin
    const bool wantClump = ue_wrap::prop::IsClumpClassName(classW);
    int matchCount = 0, matchIdx = -1;
    for (int i = 0; i < static_cast<int>(g_pileBindIndex.size()); ++i) {
        const auto& c = g_pileBindIndex[i];
        if (c.isClump != wantClump) continue;                  // a native of the row's own form only
        const float dx = c.x - matchPos.X, dy = c.y - matchPos.Y, dz = c.z - matchPos.Z;
        if (dx * dx + dy * dy + dz * dz > kBindR2Cm) continue;
        if (c.chipType != payload.chipType) continue;          // same trash variant only
        if (!R::IsLiveByIndex(c.actor, c.idx)) continue;       // bracket-long raw ptr: no deref first
        ++matchCount;
        matchIdx = i;
    }
    // Every way of leaving here without a binding arms the order owner, when the expression
    // carries a save-time key. The caller cannot tell the misses apart and says so in its own
    // comment, and an unarmed one leaves the eid with no actor for the rest of the session: the
    // host has a pile the client can neither see nor aim at.
    auto armAndFail = [&](const char* why) -> void* {
        if (isSaveTimeKey && payload.elementId != 0)
            coop::element::quiescence_drain::ArmPendingSaveTimeTwin(payload.elementId, matchPos, payload.chipType,
                                                                    wantClump);
        else
            UE_LOGW("[PILE] BIND eid=%u at (%.1f,%.1f,%.1f) -- %s, and the expression carries no "
                    "save-time key to retry from", payload.elementId, matchPos.X, matchPos.Y, matchPos.Z, why);
        return nullptr;
    };
    if (matchCount > 1) {
        UE_LOGW("[PILE] BIND SKIP eid=%u at (%.1f,%.1f,%.1f) -- %d native twins of the row's form within "
                "1cm (ambiguous cluster) -> binding none (never bind the wrong one)",
                payload.elementId, matchPos.X, matchPos.Y, matchPos.Z, matchCount);
        return armAndFail("an ambiguous cluster");
    }
    if (matchCount == 0) {
        // A save-time key names a pile that SHOULD have a save-loaded twin, so the miss is almost
        // always TIMING: this runs in the world-ready snapshot burst, before the client's async
        // native-pile load tail has drained, so the native at that key has not loaded or indexed
        // yet and appears about ten seconds later, at the post-quiescence sweep, where the order
        // owner binds it. A non-save-time miss is a genuine DERIVED pile with no twin -- the
        // common gameplay case -- and the caller materialises a mirror for it. The spawn mechanism
        // only CAPTURES; the order owner drains.
        LogNearestDelta(payload, matchPos);
        if (!isSaveTimeKey) return nullptr;   // the caller materialises; nothing to wait for
        return armAndFail("no native at the save-time key yet");
    }
    void* native = g_pileBindIndex[matchIdx].actor;
    // The EnsureIndex bound-mirror skip runs at index-BUILD time; a native that binds AFTER the
    // (latched) build is still in the index. Re-check BEFORE consuming it, so a candidate this
    // expression cannot use is still there for the expression that can.
    if (coop::prop_element_tracker::IsBoundMirrorNative(native))
        return armAndFail("the matched native is already another eid's mirror");
    g_pileBindIndex[matchIdx] = g_pileBindIndex.back();   // O(1) remove (consume the candidate)
    g_pileBindIndex.pop_back();
    // No physics reconcile: the shared helper resolves an Aprop_C mesh offset, and a chip pile is
    // not an Aprop_C, so the call was a measured no-op. The pile's own construction sets its mesh
    // Static, and both peers loaded the same save, so a bound pile is already at rest where the
    // host has it. Converge only on real divergence: save-aligned piles are sub-millimetre
    // identical, so the common case writes and wakes nothing. A native whose location cannot be read
    // is not known to be aligned, so the host's transform converges it.
    ue_wrap::FVector cur{};
    const bool curRead = ue_wrap::engine::TryGetActorLocation(native, cur);
    const float ddx = cur.X - payload.locX, ddy = cur.Y - payload.locY, ddz = cur.Z - payload.locZ;
    constexpr float kAlignedCm = 2.0f;
    const bool aligned = curRead && (ddx * ddx + ddy * ddy + ddz * ddz) <= (kAlignedCm * kAlignedCm);
    if (!aligned) {
        ue_wrap::engine::SetActorLocation(native, ue_wrap::FVector{payload.locX, payload.locY, payload.locZ});
        ue_wrap::engine::SetActorRotation(native,
            ue_wrap::FRotator{payload.rotPitch, payload.rotYaw, payload.rotRoll});
    }
    coop::prop_wire_parity::RestoreCollisionIfNeeded(L"pile-bind", classW, native);
    AdoptOwnNative(native, payload.elementId, senderSlot, aligned ? "burst, aligned" : "burst, converged");
    return native;
}

void AdoptOwnNative(void* native, uint32_t eid, int senderSlot, const char* why) {
    if (!native || eid == 0u) return;
    // A bound PILE is a Static, inert actor. A bound CLUMP is a ticking, simulating body, and from
    // this bind the host authors its pose and its turn back into a pile: left as loaded it is a
    // second author of both (it rolls and is pushed on this peer alone). The same two parkings a
    // spawned clump mirror gets (trash_mirror::Materialize); its collision stays, because a clump
    // at rest is aimed at and stood on like the host's.
    if (ue_wrap::prop::IsGarbageClump(native)) {
        ue_wrap::engine::SetActorTickEnabled(native, false);      // a clump's tick wakes its body every second
        ue_wrap::engine::SetActorSimulatePhysics(native, false);  // kinematic: the host drives it
        g_parkedClumps.push_back({native, R::InternalIndexOf(native)});
    }
    // The claim: this actor was expressed on the wire this bracket, so the membership sweep must
    // not destroy it. Without it an entire host-expressed class claims zero and the sweep's
    // completeness floor reads that as the host under-expressing. Outside a bracket it is a no-op.
    coop::join_membership_sweep::RecordClaimIfTracking(native);
    // Retire the client-local identity: the save-loaded pile was census-walked with a client-minted
    // eid, and from this bind its sole cross-peer identity is the host eid. Left in place, a local
    // morph would make the destroy observer broadcast a stray PropDestroy under the superseded eid.
    coop::prop_element_tracker::UnmarkKnownKeyedProp(native);
    coop::remote_prop::RegisterPropMirror(eid, native, L"", ue_wrap::reflection::ClassNameOf(native), senderSlot);
    // Save-native is what the rest of the machinery tests (IsBoundMirrorNative): the grab route,
    // the morph hand-off, the divergence-sweep exemption and the retire all read it.
    if (auto* el = coop::element::Registry::Get().Get(eid)) el->SetSaveNative(true);
    ++g_pileBindCount;
    if (g_pileBindCount <= 8 || (g_pileBindCount % 200) == 0) {
        ue_wrap::FVector at{};
        const bool atRead = ue_wrap::engine::TryGetActorLocation(native, at);
        UE_LOGI("[PILE] BIND #%d eid=%u -> OWN save-loaded native %p at (%.1f,%.1f,%.1f)%s [%s]",
                g_pileBindCount, eid, native, at.X, at.Y, at.Z, atRead ? "" : " (unread)", why);
    }
}

void OnDisconnect() {
    // With no session nobody authors these bodies but this client: a clump left kinematic with its
    // tick off would hang where the host last put it and refuse the game's own pickup, which wants
    // a simulating body. One that turned back into a pile in the meantime is dead by index.
    int restored = 0;
    for (const ParkedClump& c : g_parkedClumps) {
        if (!R::IsLiveByIndex(c.actor, c.idx)) continue;
        coop::remote_prop::ClearAnyDriveFor(c.actor);   // before ForceRelease, which destroys a driven clump
        ue_wrap::engine::SetActorTickEnabled(c.actor, true);
        ue_wrap::engine::SetActorSimulatePhysics(c.actor, true);
        ++restored;
    }
    if (!g_parkedClumps.empty())
        UE_LOGI("[PILE] pile_spawn_bind: OnDisconnect gave %d of %zu parked own clump(s) back to the game "
                "(tick and physics on)", restored, g_parkedClumps.size());
    g_parkedClumps.clear();
    Reset();
}

void LogCensus() {
    // ALWAYS log when the index was built this bracket, even at zero orphans: the summary line is
    // the proof the census ran, and a clean join binds every indexed candidate, which would
    // otherwise be silent and indistinguishable from a census that never ran.
    //
    // A FRESH walk, not the build-time index: a mass purge runs right at the sweep (the tracker
    // reaps dead Prop Elements in batches, draining the join-tail backlog every few seconds),
    // churning the GUObjectArray so every stored internal index goes stale and IsLiveByIndex
    // false-negatives on every survivor. One walk, once per join, with the class filter first.
    if (!g_pileBindIndexBuilt) return;
    struct Seen { float x, y, z; bool bound; };
    std::vector<Seen> seen;
    int totalLive = 0, bound = 0, unread = 0;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        if (!ue_wrap::prop::IsTrashActor(o)) continue;                 // what the index holds: piles and clumps at rest
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;   // CDO
        ++totalLive;
        const bool isBound = coop::prop_element_tracker::IsBoundMirrorNative(o);
        if (isBound) ++bound;
        ue_wrap::FVector loc{};
        if (!ue_wrap::engine::TryGetActorLocation(o, loc)) { ++unread; continue; }   // counted, in no band
        seen.push_back({loc.X, loc.Y, loc.Z, isBound});
    }
    const int orphan = totalLive - bound;
    // The bands, computed over the positions this walk already read -- no second engine call per
    // pair. An orphan sitting on top of a bound native is a near-miss the 1 cm gate refused; one
    // far from every bound native is a pile the host genuinely does not have.
    int le5 = 0, mid = 0, gt30 = 0, none = 0;
    const bool verbose = DeltaProbeOn();
    for (const auto& a : seen) {
        if (a.bound) continue;
        float best = -1.f;
        for (const auto& b : seen) {
            if (!b.bound) continue;
            const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (best < 0.f || d2 < best) best = d2;
        }
        const float d = (best >= 0.f) ? std::sqrt(best) : -1.f;
        if (d < 0.f)        ++none;   // no bound native anywhere -> nothing of the host's is near
        else if (d <= 5.f)  ++le5;    // a bound native sits ~here     -> near-miss the gate refused
        else if (d <= 30.f) ++mid;    // ambiguous (settle vs neighbour)
        else                ++gt30;   // the nearest host pile is far  -> moved or a true orphan
        if (verbose)
            UE_LOGI("[PILE-CENSUS] orphan native @(%.1f,%.1f,%.1f) nearestBound_d=%.1fcm", a.x, a.y, a.z, d);
    }
    UE_LOGI("[PILE-CENSUS] %d live native trash actor(s), piles and clumps (of %d indexed at the burst): %d BOUND to a host "
            "eid, %d orphan -- le5=%d (near-miss) 5_30=%d (ambiguous) gt30=%d (moved/true orphan) "
            "noBound=%d (nothing of the host's nearby) unread=%d (no position, in no band)",
            totalLive, g_pileIndexBuiltCount, bound, orphan, le5, mid, gt30, none, unread);
}

}  // namespace coop::pile_spawn_bind

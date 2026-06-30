// coop/element/quiescence_drain.cpp -- the join-window ORDER owner (see header).
//
// CONSOLIDATED 2026-06-30 (anti-smear refactor, [[feedback-one-owner-order-axis]]) from:
//   - coop/element/identity_reconcile.cpp  (the SEQUENCE + the steady-state trigger), and
//   - coop/props/pile_reconcile.cpp groups B+C  (the deferred QUEUES the sequence drained:
//     save-time twins, b3 pos-corrections, destroy-before-load; + HasPendingWork + Reset).
// Both were two halves of ONE concept (the drain-edge reconcile order axis) split across two
// folders + two namespaces, with a GENERIC kerfur destroy queue living in a "pile"-named file.
// The kerfur off->active retire's SEQUENCING also folds in here: it was a THIRD parallel order
// owner driven from kerfur_convert::PollKerfurConversions; now the ONE sequence calls the kerfur
// retire MECHANISM (kerfur_reconcile, which STAYS its own module, like save_identity_bind).

#include "coop/element/quiescence_drain.h"

#include "coop/creatures/kerfur_reconcile.h"   // SweepReconcileSaveTimeKerfurs + HasPendingRetire (mechanism the sequence calls)
#include "coop/element/registry.h"             // Registry::Get (b3 pos-correction resolve)
#include "coop/props/pile_spawn_bind.h"        // SweepReconcileSaveTimeTwins counterpart -> LogCensus (the pile-bind census)
#include "coop/props/prop_element_tracker.h"   // IsBoundMirrorNative / InPurgeEpisode
#include "coop/props/remote_prop.h"            // TryApplyDestroy + KeyToWString (deferred destroy-before-load re-apply)
#include "coop/props/remote_prop_spawn.h"      // HasLoadTailQuiesced (the quiescence gate)
#include "coop/props/join_membership_sweep.h"  // anti-smear 2026-06-30: claim+sweep extracted out of remote_prop_spawn
#include "coop/props/save_identity_bind.h"     // BindUnboundReCreates (identity layer the sequence calls)
#include "coop/props/save_time_retire_util.h"  // FindExactMatch + UnmarkAndDestroy (shared kernel)
#include "ue_wrap/engine.h"
#include "ue_wrap/log.h"
#include "ue_wrap/prop.h"
#include "ue_wrap/reflection.h"
#include "ue_wrap/types.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>   // memcmp -- deferred-destroy dedup by key bytes
#include <unordered_map>
#include <vector>

namespace coop::element::quiescence_drain {
namespace {

namespace R = ue_wrap::reflection;

// ---- The deferred queues (armed by event handlers; drained by the sequence) ----

// v86 Path 1c -- save-time twin-destroys that MISSED at the world-ready snapshot burst (the moved pile's
// native@save-time-key had not async-loaded/indexed yet). Keyed by host eid; retried at SweepReconcileSaveTimeTwins
// where the late native is present.
struct PendingTwin {
    float   x, y, z;     // the save-time key (the native@old position to destroy)
    uint8_t chipType;
};
std::unordered_map<uint32_t, PendingTwin> g_pendingSaveTimeTwin;

// b3 (v90, PropSnapPos) -- a join-window position correction for a save-authoritative chipPile the host moved
// while our reliable channel wasn't ready. Armed on receipt, applied at the quiescence sweep (the bind has
// registered the native + the load tail is settled). Keyed by host eid; the latest correction wins.
struct PendingPosCorrection { float x, y, z, pitch, yaw, roll; };
std::unordered_map<uint32_t, PendingPosCorrection> g_pendingPosCorrection;

// DESTROY-BEFORE-LOAD (2026-06-30): PropDestroys that arrived before this peer loaded the doomed prop. Armed
// by remote_prop::OnDestroy (the event handler CAPTURES), applied at the quiescence sweep AFTER the rebind (the
// order owner SEQUENCES) -- never dropped. [[feedback-one-owner-order-axis]]
std::vector<coop::net::PropDestroyPayload> g_pendingDestroy;

// ---- Trigger timing (the steady-state reconcile) ----

// Debounce for the steady-state reconcile: when work is pending we want to retire the stale twin promptly
// (so a grabbed save-pile's ghost@old does not linger), but not re-walk the GUObjectArray every tick. 250 ms
// is well under human-visible for a ghost to vanish and bounds the walk rate. Game-thread only -> a plain
// static is safe.
constexpr auto kSteadyReconcileDebounce = std::chrono::milliseconds(250);
std::chrono::steady_clock::time_point g_lastSteadyReconcile{};

// Post-purge re-bind window (the variant-1 trigger). A mass-purge (engine GC during a save-transfer join)
// reaps the bound save-natives; the game then RE-CREATES them UNBOUND. The join one-shot sweep often fires
// DURING the purge (before the re-creates land) or aborts on the incomplete snapshot, so variant-1 must also
// run AFTER the purge drains. While InPurgeEpisode is active we just remember the time; for this window AFTER
// it clears we run the reconcile on the debounce so variant-1 re-binds the re-created natives.
constexpr auto kPostPurgeWindow = std::chrono::seconds(6);
std::chrono::steady_clock::time_point g_lastPurgeAt{};

// ---- Internal drain steps (called only by RunReconcile, in sequence) ----

int SweepReconcileSaveTimeTwins() {
    if (g_pendingSaveTimeTwin.empty()) return 0;
    const size_t pendingN = g_pendingSaveTimeTwin.size();

    // FRESH GC-robust walk of live native chipPiles (the world-ready index is stale: built BEFORE these
    // natives async-loaded; the same reason LogCensus re-walks). Pointer-compare class filter before any
    // read; no stored internal indices (a sweep-time mass-purge stales them).
    struct LiveNative { void* actor; int32_t idx; float x, y, z; uint8_t chipType; };
    std::vector<LiveNative> natives;
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        if (!ue_wrap::prop::IsChipPile(o)) continue;                   // real actorChipPile_C only (NOT our proxy)
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;   // CDO
        if (coop::prop_element_tracker::IsBoundMirrorNative(o)) continue;  // (X) bound native is the mirror -- never a save-time twin
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(o);
        natives.push_back({o, R::InternalIndexOf(o), loc.X, loc.Y, loc.Z, ue_wrap::prop::GetChipType(o)});
    }

    // Match each pending save-time key to the now-loaded native within 1cm + same chipType via the shared
    // mirror-identity kernel (claim-track so two keys never claim one native; ambiguous(>1)->skip never
    // destroys the wrong one). 1cm is exact (the save round-trip is bit-for-bit; the key IS the native's
    // loaded position).
    std::vector<bool> consumedFlags(natives.size(), false);
    std::vector<void*> toRemove;
    toRemove.reserve(pendingN);
    for (const auto& [eid, p] : g_pendingSaveTimeTwin) {
        (void)eid;
        const ue_wrap::FVector key{p.x, p.y, p.z};
        const int idx = coop::save_time_retire_util::FindExactMatch(
            natives, consumedFlags, key,
            [&p](const LiveNative& nv) { return nv.chipType == p.chipType; });
        if (idx >= 0) { consumedFlags[idx] = true; toRemove.push_back(natives[idx].actor); }
    }

    // SAFETY VALVE (mirrors RunDivergenceSweep_'s >50% valve): removing MORE THAN HALF the live native piles
    // is not a handful of moved-in-window twins -- it is a racing/incomplete bracket where the load-tail is
    // STILL draining (so most "matches" would be wrong). Refuse it; the natives stay.
    if (!natives.empty() && static_cast<int>(toRemove.size()) * 2 > static_cast<int>(natives.size())) {
        UE_LOGW("[PILE-1C] sweep-reconcile ABORTED -- %zu save-time twin removals of %zu live native(s) "
                "(>50%%); racing/incomplete bracket, not divergence -- keeping all natives",
                toRemove.size(), natives.size());
        g_pendingSaveTimeTwin.clear();
        return 0;
    }

    for (void* native : toRemove)
        // Drop the client-minted eid FIRST so the K2_DestroyActor PRE observer stays silent (no stray
        // PropDestroy on the superseded client eid) -- the same fresh-mirror invariant the world-ready
        // twin-destroy enforces.
        coop::save_time_retire_util::UnmarkAndDestroy(native);
    UE_LOGI("[PILE-1C] sweep-reconcile -- %zu of %zu pending save-time twin(s) removed at post-quiescence "
            "(the late-loaded native@old destroyed; the proxy@new is the sole mirror -- join-window dup fixed)",
            toRemove.size(), pendingN);
    g_pendingSaveTimeTwin.clear();
    return static_cast<int>(toRemove.size());
}

void ApplyPendingDestroys() {
    if (g_pendingDestroy.empty()) return;
    for (auto it = g_pendingDestroy.begin(); it != g_pendingDestroy.end(); ) {
        if (coop::remote_prop::TryApplyDestroy(*it)) {
            UE_LOGI("[DESTROY-DEFER] CLIENT applied deferred destroy eid=%u -- target finally loaded -> destroyed "
                    "(out-of-order destroy reconciled; no dup)", it->elementId);
            it = g_pendingDestroy.erase(it);
        } else {
            ++it;  // target still not loaded -- retry next drain
        }
    }
}

}  // namespace

// ---- Queue ARM entry points (event handlers CAPTURE here; they never apply) ----

void ArmPendingSaveTimeTwin(coop::element::ElementId eid, const ue_wrap::FVector& savePos, uint8_t chipType) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    // docs/piles/09: record the save-time key for the post-quiescence sweep. No bracket index needed -- the
    // sweep does a FRESH GUObjectArray walk for the late-loaded native@old and matches this key via the shared
    // kernel. Idempotent per eid (the latest grab/land/spawn-miss wins).
    g_pendingSaveTimeTwin[static_cast<uint32_t>(eid)] = PendingTwin{savePos.X, savePos.Y, savePos.Z, chipType};
    UE_LOGI("[PILE-09] CLIENT armed pending save-time twin eid=%u key=(%.1f,%.1f,%.1f) chipType=%u "
            "(in-window grabbed/moved or world-ready-miss pile -> sweep retires the stale native@old at quiescence)",
            static_cast<unsigned>(eid), savePos.X, savePos.Y, savePos.Z, static_cast<unsigned>(chipType));
}

void ArmPendingPosCorrection(coop::element::ElementId eid,
                             const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot) {
    g_pendingPosCorrection[static_cast<uint32_t>(eid)] =
        PendingPosCorrection{loc.X, loc.Y, loc.Z, rot.Pitch, rot.Yaw, rot.Roll};
    UE_LOGI("[PILE-B3] CLIENT armed pos-correction eid=%u host=(%.1f,%.1f,%.1f) -- a save-authoritative pile "
            "the host moved in-window (convert dropped); snap the bound native at quiescence",
            static_cast<unsigned>(eid), loc.X, loc.Y, loc.Z);
}

void ApplyPendingPosCorrections() {
    if (g_pendingPosCorrection.empty()) return;  // game-thread only (the event_feed drain / the sweep), by contract
    for (auto it = g_pendingPosCorrection.begin(); it != g_pendingPosCorrection.end(); ) {
        const uint32_t eid = it->first;
        const PendingPosCorrection& c = it->second;
        coop::element::Element* el = coop::element::Registry::Get().Get(eid);
        void* actor = el ? el->GetActor() : nullptr;
        if (!actor || !R::IsLive(actor)) { ++it; continue; }  // native not bound/loaded yet -- retry next drain
        const ue_wrap::FVector  loc{c.x, c.y, c.z};
        const ue_wrap::FRotator rot{c.pitch, c.yaw, c.roll};
        ue_wrap::engine::SetActorLocation(actor, loc);
        ue_wrap::engine::SetActorRotation(actor, rot);
        const ue_wrap::FVector got = ue_wrap::engine::GetActorLocation(actor);
        const float dx = got.X - c.x, dy = got.Y - c.y, dz = got.Z - c.z;
        UE_LOGI("[PILE-B3] CLIENT pos-correction APPLIED eid=%u applied=(%.1f,%.1f,%.1f) host=(%.1f,%.1f,%.1f) "
                "drift=%.2fcm -- join-window moved pile snapped to host pos (no interaction needed)",
                static_cast<unsigned>(eid), got.X, got.Y, got.Z, c.x, c.y, c.z,
                std::sqrt(dx * dx + dy * dy + dz * dz));
        it = g_pendingPosCorrection.erase(it);
    }
}

void ArmPendingDestroy(const coop::net::PropDestroyPayload& payload) {
    for (const coop::net::PropDestroyPayload& p : g_pendingDestroy)
        if (p.elementId == payload.elementId &&
            std::memcmp(&p.key, &payload.key, sizeof(payload.key)) == 0)
            return;  // already queued
    g_pendingDestroy.push_back(payload);
    UE_LOGI("[DESTROY-DEFER] CLIENT armed deferred destroy key='%ls' eid=%u -- arrived before the target loaded; "
            "the drain-edge applies it post-bind at quiescence (destroy-before-load order fix)",
            coop::remote_prop::KeyToWString(payload.key).c_str(), payload.elementId);
}

// ---- HasPendingWork / the sequence / the triggers / Reset ----

bool HasPendingWork() {
    return !g_pendingSaveTimeTwin.empty() || !g_pendingPosCorrection.empty() || !g_pendingDestroy.empty() ||
           coop::kerfur_reconcile::HasPendingRetire();
}

void RunReconcile(bool joinSweep) {
    // Order matters (preserved from the join one-shot tail this was extracted from):
    // twin-destroy + kerfur-retire BEFORE / around the position re-bind, the deferred destroy AFTER the bind
    // (so its target resolves), and b3 pos-correction LAST (it resolves the now-bound eid). Each step is
    // internally bounded to armed/pending work, so a steady-state pass with nothing pending is cheap.
    SweepReconcileSaveTimeTwins();                               // 1: retire stale native chipPile@old (pile twin)
    coop::save_identity_bind::BindUnboundReCreates();            // 2: re-bind unbound natives (identity layer; chip=pos, kerfur=key)
    coop::kerfur_reconcile::SweepReconcileSaveTimeKerfurs();     // 3: retire stale kerfur off-prop (eid-keyed; the folded-in 3rd owner)
    ApplyPendingDestroys();                                      // 4: destroy-before-load -- apply destroys that raced the bind, post-bind
    ApplyPendingPosCorrections();                               // 5: b3 -- snap window-moved piles
    if (joinSweep) coop::pile_spawn_bind::LogCensus();           // 6: one-shot L1 orphan census (join only)
}

void OnTick() {
    // Steady-state trigger (the D1 structural fix). Only meaningful after the join load tail has drained --
    // before that, the join one-shot owns reconcile and a mid-load native set is not yet trustworthy. Then:
    // only when there is armed reconcile work, and only on the debounce edge, do the (bounded) walk.
    const auto now = std::chrono::steady_clock::now();
    // Track the purge BEFORE the quiescence gate: the join-window mass-purge happens DURING the load tail
    // (before the sweep fires = before HasLoadTailQuiesced), so recording it must NOT be gated on quiescence
    // (the 15:44 bug: the gate ran first, so g_lastPurgeAt was never set and the post-purge window never
    // opened). We only ACT (run the reconcile) post-quiescence, but we always REMEMBER the purge.
    const bool purging = coop::prop_element_tracker::InPurgeEpisode();
    if (purging) g_lastPurgeAt = now;
    if (!coop::join_membership_sweep::HasLoadTailQuiesced()) return;
    const bool postPurge = g_lastPurgeAt.time_since_epoch().count() != 0 && !purging &&
                           (now - g_lastPurgeAt) < kPostPurgeWindow;
    // Run when there's armed twin/b3/destroy/kerfur work (D1) OR we're in the post-purge window (variant-1
    // re-binds the re-created natives). Otherwise nothing to do -- no GUObjectArray walk (perf rule).
    if (!HasPendingWork() && !postPurge) return;
    if (now - g_lastSteadyReconcile < kSteadyReconcileDebounce) return;
    g_lastSteadyReconcile = now;
    UE_LOGI("quiescence_drain: steady-state reconcile (%s past quiescence)",
            postPurge ? "post-purge window -- re-binding GC-churned natives (variant-1)"
                      : "pending twin/b3/destroy/kerfur work -- armed after the join sweep");
    RunReconcile(/*joinSweep=*/false);
}

void Reset() {
    // Session teardown ONLY. The deferred queues deliberately SURVIVE bracket close (they drain at quiescence
    // / steady-state); the per-bracket index lives in pile_spawn_bind and is Reset separately. A queue item
    // unresolved by session teardown = a target that never loaded this session -> safe to drop.
    g_pendingSaveTimeTwin.clear();
    if (!g_pendingPosCorrection.empty())
        UE_LOGI("[PILE-B3] session teardown dropping %zu undrained pos-correction(s) -- target never bound this "
                "session (benign at teardown)", g_pendingPosCorrection.size());
    g_pendingPosCorrection.clear();
    if (!g_pendingDestroy.empty())
        UE_LOGI("[DESTROY-DEFER] session teardown dropping %zu unresolved deferred destroy(s) -- target never "
                "loaded here (host-removed before our copy materialized; benign)", g_pendingDestroy.size());
    g_pendingDestroy.clear();
}

}  // namespace coop::element::quiescence_drain

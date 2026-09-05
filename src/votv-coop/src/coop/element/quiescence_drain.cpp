// coop/element/quiescence_drain.cpp -- the join-window order owner (see the header): the
// deferred queues the join window arms (save-time twins, host-vacate twins, position
// corrections, destroy-before-load) and the fixed-order drain that applies them at load-tail
// quiescence and on the steady-state trigger. The kerfur retire and the identity re-bind are
// mechanisms in their own modules that the sequence calls.

#include "coop/element/quiescence_drain.h"

#include "coop/creatures/kerfur_reconcile.h"   // SweepReconcileSaveTimeKerfurs, HasPendingRetire
#include "coop/element/registry.h"             // Registry
#include "coop/props/prop_element_tracker.h"   // IsBoundMirrorNative, InPurgeEpisode
#include "coop/props/remote_prop.h"            // TryApplyDestroy, KeyToWString, IsActorUnderAnyDrive
#include "coop/props/join_membership_sweep.h"  // HasLoadTailQuiesced
#include "coop/props/save_identity_bind.h"     // BindUnboundReCreates
#include "coop/player/players_registry.h"      // Local
#include "coop/props/save_time_retire_util.h"  // FindExactMatch, UnmarkAndDestroy
#include "coop/config/config.h"           // ResolveFlag
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>   // memcmp
#include <unordered_map>
#include <vector>

namespace coop::element::quiescence_drain {
namespace {

namespace R = ue_wrap::reflection;

// The deferred queues: armed by event handlers, drained by the sequence.

// Save-time twin destroys that missed at the world-ready snapshot burst because the moved
// pile's native at the save-time key had not loaded yet. Keyed by host eid; retried by the
// sweep once the late native is present.
struct PendingTwin {
    float   x, y, z;     // the save-time key (the native@old position to destroy)
    uint8_t chipType;
    int     unresolvedPasses = 0;  // held or unmatched passes; dropped at kMaxTwinPasses
    bool    hostVacate = false;    // armed from a host PropSnapPos: retired on the host's word
};
std::unordered_map<uint32_t, PendingTwin> g_pendingSaveTimeTwin;
// A host-vacate twin matches the stale native by position alone: whatever chipPile lingers at
// the vacated spot is the stale copy, whatever its type.
constexpr uint8_t kAnyChipType = 0xFF;

// The dup probe (the pile_dup_probe row): a read-only per-twin diagnostic that says why a twin's
// exact match missed, distinguishing a clean miss (no unbound native at the old position), a
// co-located ambiguity, and a native bound to the wrong eid, which the unbound-only candidate
// set excludes and no retire path can otherwise see.
bool DupProbeOn() {
    static const bool s_on = coop::config::ResolveFlag(::coop::config_registry::rows::pile_dup_probe);
    return s_on;
}
// A twin retires when its eid is confirmed moved: the element's bound native lives farther than
// this from the twin's save position (a real host move is metres; a same-spot re-bind is about
// zero). (50 cm) squared.
constexpr float kTwinMovedThresholdCm2 = 2500.f;
// An unconfirmed or unmatched twin is kept at most this many passes (about 10 s at the 250 ms
// debounce), long enough for a moved pile's new native to arrive and bind; then it is dropped,
// leaving the old copy rather than pinning the reconcile forever.
constexpr int kMaxTwinPasses = 40;

// A join-window position correction (PropSnapPos) for a save-authoritative chipPile the host
// moved while our reliable channel was not ready. Armed on receipt, applied at the quiescence
// sweep once the bind has registered the native; keyed by host eid, the latest wins. Bounded: a
// correction whose eid never binds must not retry forever, since a pinned HasPendingWork runs
// the full-array reconcile at 4 Hz in perpetuity. The identity re-bind is what makes the eid
// bindable; the cap is the terminalisation.
struct PendingPosCorrection { float x, y, z, pitch, yaw, roll; int unresolvedPasses = 0; };
std::unordered_map<uint32_t, PendingPosCorrection> g_pendingPosCorrection;
constexpr int kMaxPosCorrectionPasses = 40;  // about 10 s at the 250 ms debounce

// Destroy-before-load: PropDestroys that arrived before this peer loaded the doomed prop.
// Armed by the destroy handler only during the load tail, applied at the quiescence sweep after
// the re-bind. Bounded: the defer is a load-tail bridge, and an entry that cannot resolve within
// kMaxDeferApplies post-quiescence passes is dropped, since by then the target never loaded
// here; a forever retry pinned HasPendingWork and ran the reconcile at 4 Hz.
struct PendingDestroy {
    coop::net::PropDestroyPayload payload;
    int failedApplies = 0;  // post-quiescence apply attempts that still found no target
};
std::vector<PendingDestroy> g_pendingDestroy;
constexpr int kMaxDeferApplies = 8;  // about 2 s at the 250 ms debounce

// Trigger timing for the steady-state reconcile.

// The debounce: a stale twin should go promptly (a grabbed save-pile's ghost must not linger),
// but the GUObjectArray is not re-walked every tick. 250 ms is under human-visible and bounds
// the walk rate. Game thread only, so a plain static.
constexpr auto kSteadyReconcileDebounce = std::chrono::milliseconds(250);
std::chrono::steady_clock::time_point g_lastSteadyReconcile{};

// The post-purge re-bind window. A mass purge (engine GC during a save-transfer join) reaps the
// bound save natives and the game re-creates them unbound; the join one-shot often fires during
// the purge, so for this window after the purge clears the reconcile runs on the debounce and
// re-binds the re-created natives.
constexpr auto kPostPurgeWindow = std::chrono::seconds(6);
std::chrono::steady_clock::time_point g_lastPurgeAt{};

// The drain steps, called only by RunReconcile, in sequence.

int SweepReconcileSaveTimeTwins() {
    if (g_pendingSaveTimeTwin.empty()) return 0;
    const size_t pendingN = g_pendingSaveTimeTwin.size();

    // A fresh walk of the live unbound native chipPiles, the candidate stale twins: the world-ready
    // index predates these async loads, and stored internal indices go stale across a purge. A
    // bound native is the mirror, never a twin.
    struct LiveNative { void* actor; int32_t idx; float x, y, z; uint8_t chipType; };
    std::vector<LiveNative> natives;
    // The probe's parallel census of bound chipPile natives and their eids: a native at the old
    // position bound to another eid is the wrong-eid residual, excluded from the candidates, and
    // this census is the only way to observe it. Empty when the probe is off.
    struct BoundNative { void* actor; float x, y, z; uint32_t eid; };
    std::vector<BoundNative> boundChip;
    const bool probe = DupProbeOn();
    const int32_t n = R::NumObjects();
    for (int32_t i = 0; i < n; ++i) {
        void* o = R::ObjectAt(i);
        if (!o || !R::IsLive(o)) continue;
        if (!ue_wrap::prop::IsChipPile(o)) continue;                   // real actorChipPile_C only (NOT our proxy)
        if (R::NameStartsWith(R::NameOf(o), L"Default__")) continue;   // CDO
        if (coop::prop_element_tracker::IsBoundMirrorNative(o)) {
            if (probe) {
                const ue_wrap::FVector bl = ue_wrap::engine::GetActorLocation(o);
                // Registry::EidForActor covers mirrors and locals; a locals-only reverse would
                // label every bound mirror as wrong-eid.
                boundChip.push_back({o, bl.X, bl.Y, bl.Z,
                    static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(o))});
            }
            continue;  // bound native is the mirror, not a twin
        }
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(o);
        natives.push_back({o, R::InternalIndexOf(o), loc.X, loc.Y, loc.Z, ue_wrap::prop::GetChipType(o)});
    }

    // The per-eid decision. Matched and confirmed: retire, no cap (positive evidence: the element
    // is bound to a live actor, the host's word for a vacate twin, bound far for an event twin).
    // Matched and unconfirmed: hold, bounded by kMaxTwinPasses, since the candidate may be the
    // element's own purge re-create, which the identity re-bind in step 2 of this pass claims. A
    // miss: a premise-dead drop or a bounded hold. Never retire without per-eid evidence: a
    // retire-on-count arm once destroyed an element's only re-create one step before the re-bind
    // could claim it.
    std::vector<bool> consumedFlags(natives.size(), false);
    std::vector<uint32_t> resolvedEids;  // erase from the pending map (retired OR dropped-after-N)
    int confirmedRetired = 0, held = 0;
    for (auto& [eid, p] : g_pendingSaveTimeTwin) {
        const ue_wrap::FVector key{p.x, p.y, p.z};
        const int idx = coop::save_time_retire_util::FindExactMatch(
            natives, consumedFlags, key,
            [&p](const LiveNative& nv) { return p.chipType == kAnyChipType || nv.chipType == p.chipType; });
        // The element's current binding, checked with IsLiveByIndex, never raw IsLive: an
        // element-held pointer may be freed memory after a purge, and IsLive on freed memory
        // misreads.
        void* bound   = nullptr;
        float boundD2 = -1.f;
        if (coop::element::Element* el =
                coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(eid))) {
            if (void* a = el->GetActor(); a && R::IsLiveByIndex(a, el->GetInternalIdx())) {
                bound = a;
                const ue_wrap::FVector bl = ue_wrap::engine::GetActorLocation(a);
                const float ddx = bl.X - p.x, ddy = bl.Y - p.y, ddz = bl.Z - p.z;
                boundD2 = ddx * ddx + ddy * ddy + ddz * ddz;
            }
        }
        // A host-vacate twin retires only while the element is bound to a live actor. Unbound, the
        // native at the old position may be the element's own purge re-create, its only expression
        // (re-creates spawn at the game's save position, never at the new one), and retiring it
        // would orphan the eid: hold, bounded, until the re-bind claims it and the correction snaps
        // it. An event twin still needs the positive move evidence.
        const bool confirmed = p.hostVacate ? (bound != nullptr)
                                            : (bound && boundD2 > kTwinMovedThresholdCm2);
        // A dead premise: an event twin whose eid is bound at the twin key can never confirm (the
        // host put the pile back, or the move never took). Dropped now rather than burning
        // kMaxTwinPasses of walk and log.
        if (!p.hostVacate && bound && boundD2 >= 0.f && boundD2 <= kTwinMovedThresholdCm2) {
            if (idx >= 0) {
                // An unbound native also sits at the key while the element is bound there:
                // co-located ambiguity, left alone, since a retire needs positive per-eid evidence.
                UE_LOGI("[PILE-1C] twin eid=%u dropped -- E is bound AT the twin key (premise dead) but an "
                        "unbound native also sits there (co-located ambiguity; not retiring)", eid);
            }
            resolvedEids.push_back(eid);
            continue;
        }
        if (idx >= 0) {
            if (confirmed) {
                consumedFlags[idx] = true;
                coop::save_time_retire_util::UnmarkAndDestroy(natives[idx].actor);  // per-eid evidence -> no cap
                resolvedEids.push_back(eid);
                ++confirmedRetired;
            } else {
                // Hold, the candidate not consumed: it is likely the element's own re-create, which
                // step 2 of this pass claims. Bounded.
                if (++p.unresolvedPasses >= kMaxTwinPasses) resolvedEids.push_back(eid);
                ++held;
            }
        } else {
            // The probe: why the exact match missed (clean, ambiguous, or bound to the wrong eid).
            if (probe) {
                auto near = [&](float x, float y, float z, float r) {
                    const float dx = x - p.x, dy = y - p.y, dz = z - p.z; return dx*dx+dy*dy+dz*dz <= r*r;
                };
                int u1 = 0, u30 = 0;
                for (const LiveNative& nv : natives) { if (near(nv.x,nv.y,nv.z,1.f)) ++u1; else if (near(nv.x,nv.y,nv.z,30.f)) ++u30; }
                int bWrong = 0, bSame = 0; uint32_t wrongEid = 0; float wrongD = 0.f;
                for (const BoundNative& bn : boundChip) {
                    if (!near(bn.x,bn.y,bn.z,30.f)) continue;
                    const float dx = bn.x-p.x, dy = bn.y-p.y, dz = bn.z-p.z; const float d = std::sqrt(dx*dx+dy*dy+dz*dz);
                    if (bn.eid == eid) ++bSame;
                    else { ++bWrong; if (wrongEid == 0) { wrongEid = bn.eid; wrongD = d; } }
                }
                const float eDist = (bound && boundD2 >= 0.f) ? std::sqrt(boundD2) : -1.f;
                void* const eActor = bound;  // IsLiveByIndex-validated above (raw IsLive misread freed memory)
                UE_LOGI("[DUP-PROBE] eid=%u %s @old=(%.1f,%.1f,%.1f) FindExactMatch=MISS pass=%d | UNBOUND@old: "
                        "%d<1cm %d<30cm | BOUND@old<30cm: %d wrong-eid (first eid=%u d=%.1fcm) + %d same-eid | "
                        "E.bound=%p d_from_old=%.1fcm  --> %s",
                        eid, p.hostVacate ? "[host-vacate]" : "[event-twin]", p.x, p.y, p.z, p.unresolvedPasses,
                        u1, u30, bWrong, wrongEid, wrongD, bSame, eActor, eDist,
                        bWrong ? "BOUND-TO-WRONG-EID residual (the predicted GC tail)"
                               : (u1 ? "unbound-present-but-unmatched(?)"
                                     : (u30 ? "unbound near but >1cm off (fuzzy)" : "CLEAN (no orphan @old)")));
            }
            if (++p.unresolvedPasses >= kMaxTwinPasses)
                resolvedEids.push_back(eid);  // no native@old ever materialized here -> stop retrying (FPS-pin guard)
        }
    }

    for (uint32_t e : resolvedEids) g_pendingSaveTimeTwin.erase(e);
    UE_LOGI("[PILE-1C] sweep-reconcile -- %zu pending twin(s): %d confirmed-moved retired (per-eid, no cap), "
            "%d HELD (unconfirmed -- kept bounded for a later confirmed pass), %zu still pending",
            pendingN, confirmedRetired, held, g_pendingSaveTimeTwin.size());
    return confirmedRetired;
}

void ApplyPendingDestroys() {
    if (g_pendingDestroy.empty()) return;
    // Post-quiescence only (OnTick gates on HasLoadTailQuiesced), so a target that will ever load
    // has loaded, and BindUnboundReCreates ran earlier in this pass. An entry that still cannot
    // resolve after a small grace never will, and is dropped: a deferred destroy kept forever pins
    // HasPendingWork and the full-array reconcile.
    for (auto it = g_pendingDestroy.begin(); it != g_pendingDestroy.end(); ) {
        if (coop::remote_prop::TryApplyDestroy(it->payload)) {
            UE_LOGI("[DESTROY-DEFER] CLIENT applied deferred destroy eid=%u -- target finally loaded -> destroyed "
                    "(out-of-order destroy reconciled; no dup)", it->payload.elementId);
            it = g_pendingDestroy.erase(it);
        } else if (++it->failedApplies >= kMaxDeferApplies) {
            UE_LOGI("[DESTROY-DEFER] CLIENT dropping unresolvable deferred destroy eid=%u after %d post-quiescence "
                    "attempt(s) -- target never loaded here (host-only / already-gone); a load-tail bridge does "
                    "not outlive the load tail (FPS pin guard)", it->payload.elementId, it->failedApplies);
            it = g_pendingDestroy.erase(it);
        } else {
            ++it;  // target still not loaded -- retry next drain (within the grace)
        }
    }
}

}  // namespace

// The arm entry points: event handlers capture here and never apply.

void ArmPendingSaveTimeTwin(coop::element::ElementId eid, const ue_wrap::FVector& savePos, uint8_t chipType) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    // The host's word supersedes the inference in both arm orders: ArmHostVacateTwin overwrites an
    // event twin, and an event arm landing after the PropSnapPos must not downgrade the vacate twin
    // back to a guess.
    if (auto it = g_pendingSaveTimeTwin.find(static_cast<uint32_t>(eid));
        it != g_pendingSaveTimeTwin.end() && it->second.hostVacate)
        return;
    // Record the save-time key for the post-quiescence sweep; no bracket index is needed, since the
    // sweep walks the GUObjectArray fresh and matches the key through the shared kernel. Idempotent
    // per eid; the latest grab, land or spawn miss wins.
    g_pendingSaveTimeTwin[static_cast<uint32_t>(eid)] = PendingTwin{savePos.X, savePos.Y, savePos.Z, chipType};
    UE_LOGI("[PILE-09] CLIENT armed pending save-time twin eid=%u key=(%.1f,%.1f,%.1f) chipType=%u "
            "(in-window grabbed/moved or world-ready-miss pile -> sweep retires the stale native@old at quiescence)",
            static_cast<unsigned>(eid), savePos.X, savePos.Y, savePos.Z, static_cast<unsigned>(chipType));
}

void ArmHostVacateTwin(coop::element::ElementId eid, const ue_wrap::FVector& oldPos) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    // The host's PropSnapPos said the element moved off oldPos, so that position is vacated on the
    // host's authority: a wildcard-type twin, retired without the position-confirm guess.
    // Overwrites any event-armed twin for the eid. Idempotent.
    g_pendingSaveTimeTwin[static_cast<uint32_t>(eid)] =
        PendingTwin{oldPos.X, oldPos.Y, oldPos.Z, kAnyChipType, 0, /*hostVacate=*/true};
    UE_LOGI("[PILE-B3] CLIENT armed HOST-VACATE twin eid=%u @old=(%.1f,%.1f,%.1f) -- host authoritatively moved E "
            "@new; the sweep retires whatever save-loaded native@old lingers here (docs/piles/12 owner)",
            static_cast<unsigned>(eid), oldPos.X, oldPos.Y, oldPos.Z);
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
    // The local player, resolved once for the settled-skip below. A cold path.
    void* localPlayer = coop::players::Registry::Get().Local();
    for (auto it = g_pendingPosCorrection.begin(); it != g_pendingPosCorrection.end(); ) {
        const uint32_t eid = it->first;
        PendingPosCorrection& c = it->second;
        coop::element::Element* el = coop::element::Registry::Get().Get(eid);
        void* actor = el ? el->GetActor() : nullptr;
        // IsLiveByIndex, never raw IsLive: a purge frees the bound actor's memory while the row
        // lingers, and raw IsLive on the freed pointer can read true and snap freed memory.
        if (!actor || !el || !R::IsLiveByIndex(actor, el->GetInternalIdx())) {
            // Not bound yet: retry next drain, bounded. An eid that never becomes bindable must not
            // pin HasPendingWork; the identity re-bind is the primary fix, this the backstop,
            // dropped with a warning so a recurrence is visible.
            if (++c.unresolvedPasses >= kMaxPosCorrectionPasses) {
                UE_LOGW("[PILE-B3] CLIENT dropping pos-correction eid=%u host=(%.1f,%.1f,%.1f) after %d "
                        "post-quiescence passes -- eid never bound a live native (re-bind found no candidate); "
                        "accepting the divergence rather than pinning the 4 Hz drain (FPS-pin guard)",
                        eid, c.x, c.y, c.z, c.unresolvedPasses);
                it = g_pendingPosCorrection.erase(it);
                continue;
            }
            ++it;
            continue;
        }
        // Settled re-validation: the actor is live, but a grab or convert may have landed between
        // the arm and this apply, in which case the eid now renders as a carried clump or a held
        // prop, and SetActorLocation would fling the carried body to a ghost position. A picked-up
        // rock is already destroyed, so the liveness check drops it; this is the pile and clump
        // guard. Deferred under the shared bound, so a transient grab still gets its correction
        // after release; dropped with a warning if it stays held past the cap.
        if ((localPlayer && ue_wrap::engine::IsMainPlayerGrabbing(localPlayer, actor)) ||
            coop::remote_prop::IsActorUnderAnyDrive(actor)) {
            if (++c.unresolvedPasses >= kMaxPosCorrectionPasses) {
                UE_LOGW("[PILE-B3] CLIENT dropping pos-correction eid=%u -- actor stayed HELD/driven for %d "
                        "passes (a grab/convert owns its position now); accepting the divergence over fighting "
                        "the carry (snapping a carried clump would fling it to a ghost pos)",
                        eid, c.unresolvedPasses);
                it = g_pendingPosCorrection.erase(it);
                continue;
            }
            ++it;
            continue;
        }
        const ue_wrap::FVector  loc{c.x, c.y, c.z};
        const ue_wrap::FRotator rot{c.pitch, c.yaw, c.roll};
        // A save-loaded chipPile native rests at Static mobility, where SetActorLocation silently
        // does nothing (the call returns true and the actor never moves), so the root is forced
        // Movable first. Kerfurs never needed this, since an NPC is Movable.
        ue_wrap::engine::SetActorRootMovable(actor);
        ue_wrap::engine::SetActorLocation(actor, loc);
        ue_wrap::engine::SetActorRotation(actor, rot);
        const ue_wrap::FVector got = ue_wrap::engine::GetActorLocation(actor);
        const float dx = got.X - c.x, dy = got.Y - c.y, dz = got.Z - c.z;
        UE_LOGI("[PILE-B3] CLIENT pos-correction APPLIED eid=%u applied=(%.1f,%.1f,%.1f) host=(%.1f,%.1f,%.1f) "
                "drift=%.2fcm -- join-window moved pile snapped to host pos (forced-Movable then teleport; "
                "drift~0 confirms the snap took -- no interaction needed)",
                static_cast<unsigned>(eid), got.X, got.Y, got.Z, c.x, c.y, c.z,
                std::sqrt(dx * dx + dy * dy + dz * dz));
        it = g_pendingPosCorrection.erase(it);
    }
}

void EnsurePosCorrection(coop::element::ElementId eid,
                         const ue_wrap::FVector& loc, const ue_wrap::FRotator& rot) {
    // Arm if absent: when the identity re-bind claims a purge re-create at the save position for an
    // eid whose host position is elsewhere, the actor must still be snapped. Usually the original
    // PropSnapPos correction is still pending; when it already applied before the churn, re-arm
    // from the identity map's host position. An armed correction keeps its fresher, host-sent
    // rotation.
    const auto key = static_cast<uint32_t>(eid);
    if (g_pendingPosCorrection.count(key) > 0) return;
    ArmPendingPosCorrection(eid, loc, rot);
}

void CancelPendingSaveTimeTwin(coop::element::ElementId eid) {
    // The re-bind just bound the native at the twin's key to the element itself, so the stale-copy
    // premise is dead (exactly one native there, and it is the element; the co-located case never
    // re-binds). Without the cancel the twin would burn kMaxTwinPasses of walk and log, since the
    // native it would retire is now bound and excluded from the candidates.
    if (g_pendingSaveTimeTwin.erase(static_cast<uint32_t>(eid)) > 0)
        UE_LOGI("[PILE-1C] twin eid=%u CANCELLED -- the identity re-bind claimed the native at the twin key as "
                "E's own re-create (no stale copy exists)", static_cast<unsigned>(eid));
}

void ArmPendingDestroy(const coop::net::PropDestroyPayload& payload) {
    for (const PendingDestroy& p : g_pendingDestroy)
        if (p.payload.elementId == payload.elementId &&
            std::memcmp(&p.payload.key, &payload.key, sizeof(payload.key)) == 0)
            return;  // already queued
    g_pendingDestroy.push_back({payload, 0});
    UE_LOGI("[DESTROY-DEFER] CLIENT armed deferred destroy key='%ls' eid=%u -- arrived before the target loaded; "
            "the drain-edge applies it post-bind at quiescence (destroy-before-load order fix)",
            coop::remote_prop::KeyToWString(payload.key).c_str(), payload.elementId);
}

// HasPendingWork, the sequence, the triggers, Reset.

// The ghost-sweep arm, set by the events that can strand an identity-less native chipPile on
// the client (a re-bind displacing a live native; a use-press landing on an unbound native) and
// consumed by RunReconcile, whose step 2 retires every such ghost in one pass. A bool, not a
// queue: the pass re-derives the ghost set itself.
static bool g_ghostSweepArmed = false;

void ArmGhostSweep() {
    if (!g_ghostSweepArmed)
        UE_LOGI("quiescence_drain: GHOST-SWEEP armed -- a native chipPile may have been stranded "
                "identity-less (displaced by a rebind / hit by an E-press unbound); the next "
                "reconcile pass adjudicates ALL of them at once");
    g_ghostSweepArmed = true;
}

bool HasPendingWork() {
    return !g_pendingSaveTimeTwin.empty() || !g_pendingPosCorrection.empty() || !g_pendingDestroy.empty() ||
           coop::kerfur_reconcile::HasPendingRetire() || g_ghostSweepArmed;
}

void RunReconcile() {
    // Order matters: the twin destroy and the kerfur retire around the re-bind, the deferred
    // destroy after the bind so its target resolves, and the position correction last, since it
    // resolves the now-bound eid. Each step is bounded to armed work, so an idle pass is cheap. At
    // the join quiescence edge the sequence runs before the membership doom sweep, so a re-run
    // converge-claims the re-creates that raced their wire expression, and doom judges last; the
    // orphan census lives at the sweep's tail in join_membership_sweep.cpp for the same reason.
    SweepReconcileSaveTimeTwins();                               // 1: retire the stale native chipPile at the old position
    bool ghostDrained = true;
    coop::save_identity_bind::BindUnboundReCreates(&ghostDrained);  // 2: re-bind unbound natives, then retire the identity-less ghosts
    // The arm is consumed only when the tail drained: a capped pass or a valve abort keeps the
    // sweep armed, so the next pass finishes the ghost set instead of stranding it.
    if (ghostDrained) g_ghostSweepArmed = false;
    else UE_LOGI("quiescence_drain: GHOST-SWEEP kept armed (retire tail capped/valved this pass)");
    coop::kerfur_reconcile::SweepReconcileSaveTimeKerfurs();     // 3: retire the stale kerfur off-prop
    ApplyPendingDestroys();                                      // 4: destroys that raced the bind, applied post-bind
    ApplyPendingPosCorrections();                               // 5: snap the window-moved piles
}

void OnTick() {
    // The steady-state trigger: only after the join load tail has drained (before that the join
    // one-shot owns the reconcile and a mid-load native set is not trustworthy), only with armed
    // work, and only on the debounce edge.
    const auto now = std::chrono::steady_clock::now();
    // The purge is recorded before the quiescence gate: the join-window mass purge happens during
    // the load tail, so a gated record would never open the post-purge window. Acting stays
    // post-quiescence.
    const bool purging = coop::prop_element_tracker::InPurgeEpisode();
    if (purging) g_lastPurgeAt = now;
    if (!coop::join_membership_sweep::HasLoadTailQuiesced()) return;
    const bool postPurge = g_lastPurgeAt.time_since_epoch().count() != 0 && !purging &&
                           (now - g_lastPurgeAt) < kPostPurgeWindow;
    // Run with armed work or inside the post-purge window; otherwise no GUObjectArray walk.
    if (!HasPendingWork() && !postPurge) return;
    if (now - g_lastSteadyReconcile < kSteadyReconcileDebounce) return;
    g_lastSteadyReconcile = now;
    UE_LOGI("quiescence_drain: steady-state reconcile (%s past quiescence)",
            postPurge ? "post-purge window -- re-binding GC-churned natives (variant-1)"
                      : "pending twin/b3/destroy/kerfur work -- armed after the join sweep");
    RunReconcile();
}

void Reset() {
    // Session teardown only: the queues survive a bracket close, since they drain at quiescence or
    // in steady state, and the per-bracket index in pile_spawn_bind resets separately. An item
    // unresolved at teardown is a target that never loaded this session.
    g_pendingSaveTimeTwin.clear();
    g_ghostSweepArmed = false;  // the pending ghost adjudication goes too
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

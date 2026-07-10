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
#include "coop/player/players_registry.h"      // F1 piece 2: Local() -> settled-skip a held actor at apply
#include "coop/props/save_time_retire_util.h"  // FindExactMatch + UnmarkAndDestroy (shared kernel)
#include "coop/config/config.h"           // IsIniKeyTrue (pile_dup_probe gate)
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
    int     unresolvedPasses = 0;  // bounded-keep: a twin held (unconfirmed) or unmatched across passes is
                                   // dropped after kMaxTwinPasses so it can never pin HasPendingWork() forever.
    bool    hostVacate = false;    // b3 OWNER (docs/piles/12): armed from a host PropSnapPos = the host
                                   // AUTHORITATIVELY moved E off this @old pos. Retire the stale native@old on
                                   // the host's word -- NO client-side position-confirm guess, NO >50% cap
                                   // (the host IS the authority; the guess is what GC pointer-reuse corrupted).
};
std::unordered_map<uint32_t, PendingTwin> g_pendingSaveTimeTwin;
// A host-vacate twin matches the stale native@old by POSITION alone (the host said "E left here"; whatever
// same-spot chipPile lingers is the stale copy, regardless of chipType) -> arm with this wildcard chipType.
constexpr uint8_t kAnyChipType = 0xFF;

// PROBE (2026-07-01, gated [dev] pile_dup_probe): the mass-move dup class is VERIFIED-closed to ONE residual
// (docs/piles/12). The current aggregate sweep log ("N confirmed retired") cannot tell WHY a twin's
// FindExactMatch returns -1: (0) no unbound native@old = clean, (>1) co-located-ambiguous, or the orphan is
// BOUND-to-the-wrong-eid (excluded from the unbound-only candidate set -> invisible to every retire path, the
// predicted GC-pointer-reuse tail). Read-only per-twin diagnostic that distinguishes the three -- so we PROBE
// the residual before touching the VERIFIED owner. [[feedback-probe-dont-guess-rule]]
bool DupProbeOn() {
    static const bool s_on = coop::config::IsIniKeyTrue("pile_dup_probe");
    return s_on;
}
// A twin retires when its eid is CONFIRMED moved @new: E's currently-bound native lives farther than this
// from the twin's save-pos (a real host move is meters; a same-spot re-bind is ~0). (50 cm)^2.
constexpr float kTwinMovedThresholdCm2 = 2500.f;
// Keep an unconfirmed/unmatched twin at most this many drain passes (~10 s at the 250 ms debounce) so a moved
// pile's @new native has time to arrive+bind and CONFIRM the move; then drop it (the @old stays -- the safe
// fallback -- rather than pinning the reconcile forever). Spans the join-window convert delivery.
constexpr int kMaxTwinPasses = 40;

// b3 (v90, PropSnapPos) -- a join-window position correction for a save-authoritative chipPile the host moved
// while our reliable channel wasn't ready. Armed on receipt, applied at the quiescence sweep (the bind has
// registered the native + the load tail is settled). Keyed by host eid; the latest correction wins.
// BOUNDED (2026-07-03, docs/piles/12 eid=4435): a correction whose eid never binds must not retry forever --
// that pinned HasPendingWork() true and ran the full-array reconcile at 4 Hz in perpetuity (the 20:25-26 log:
// twins cap at kMaxTwinPasses, deferred destroys at kMaxDeferApplies, pos-corrections had NO cap). The primary
// fix is the identity re-bind (savePos-immutable two-phase match in save_identity_bind) that makes the eid
// bindable; this cap is the terminalization so no residual shape can ever pin the drain again.
struct PendingPosCorrection { float x, y, z, pitch, yaw, roll; int unresolvedPasses = 0; };
std::unordered_map<uint32_t, PendingPosCorrection> g_pendingPosCorrection;
constexpr int kMaxPosCorrectionPasses = 40;  // ~10 s at the 250 ms debounce -- spans any late re-create+re-bind

// DESTROY-BEFORE-LOAD (2026-06-30): PropDestroys that arrived before this peer loaded the doomed prop. Armed
// by remote_prop::OnDestroy (the event handler CAPTURES) ONLY during the load tail (pre-quiescence -- a true
// before-load race); applied at the quiescence sweep AFTER the rebind (the order owner SEQUENCES).
// BOUNDED (2026-06-30 FPS fix): the defer is a load-tail BRIDGE, not a permanent retry. An entry that cannot
// resolve within kMaxDeferApplies post-quiescence passes is DROPPED -- the load tail is definitively over, so
// the target never loaded here (host-only / already-gone). Forever-retry pinned HasPendingWork() true -> the
// full-array reconcile ran 4x/sec in perpetuity (the 15:34 client-FPS death: 500+ "still has no local actor"
// retries on a steady-state death-watch destroy for an eid this peer never mirrored). [[feedback-one-owner-order-axis]]
struct PendingDestroy {
    coop::net::PropDestroyPayload payload;
    int failedApplies = 0;  // post-quiescence apply attempts that still found no target
};
std::vector<PendingDestroy> g_pendingDestroy;
constexpr int kMaxDeferApplies = 8;  // ~2s at the 250ms reconcile debounce -- grace for a late bind, then drop

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

    // FRESH GC-robust walk of live UNBOUND native chipPiles (candidate stale twins@old). The world-ready index
    // is stale (built BEFORE these async-loaded); no stored internal indices (a sweep-time mass-purge stales
    // them). A BOUND native is the mirror -- never a twin -- so skip it (the twin@old we retire is the leftover
    // UNBOUND save-loaded copy, distinct from E's authoritative @new mirror).
    struct LiveNative { void* actor; int32_t idx; float x, y, z; uint8_t chipType; };
    std::vector<LiveNative> natives;
    // PROBE-only parallel census of BOUND chipPile natives + the eid each is bound to. A native@old bound to an
    // eid != the twin's eid is the bound-to-wrong-eid residual; it is deliberately EXCLUDED from `natives` so the
    // sweep never sees it -- this census is the only way to observe it. Empty (and never walked into) when off.
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
                // Registry::EidForActor is the UNIFIED reverse (mirrors + locals). The prior read here
                // (GetPropElementIdForActor) returns kInvalidId for MIRRORS by its locals-only contract, so
                // every bound mirror probed as "wrong-eid 4294967295" (the 20:24 run's 17 mislabels).
                boundChip.push_back({o, bl.X, bl.Y, bl.Z,
                    static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(o))});
            }
            continue;  // bound native is the mirror, not a twin
        }
        const ue_wrap::FVector loc = ue_wrap::engine::GetActorLocation(o);
        natives.push_back({o, R::InternalIndexOf(o), loc.X, loc.Y, loc.Z, ue_wrap::prop::GetChipType(o)});
    }

    // Per-eid decision (2026-07-03 rewrite, docs/piles/12 eid=4435):
    //   matched + CONFIRMED   -> retire PER-EID, no cap (positive evidence: E is bound to a live actor --
    //                            the host's word for a vacate twin, bound-FAR for an event twin).
    //   matched + UNCONFIRMED -> HOLD, bounded by kMaxTwinPasses. E is unbound (the candidate may be E's OWN
    //                            purge re-create -- its only expression; the identity re-bind in step 2 of
    //                            this same pass claims it) or not yet far. NEVER retire without positive
    //                            per-eid evidence ([[feedback-join-reconcile-sweep-safety]]).
    //   miss                  -> premise-dead drop (event twin whose E is bound AT the key) or bounded hold.
    // RULE 2: the pre-identity-map "small handful -> retire unconfirmed" arm and its >50% cap are GONE. They
    // retired on ZERO evidence -- post-purge (870 unbound natives -> cap never trips) an unbound-E vacate
    // twin's candidate was E's own re-create, and that arm destroyed it one step before the re-bind could
    // claim it: the same orphan-forever hazard this fix closes, through the other door. (The 20:24 run only
    // survived it because the sweep happened to run AFTER the mass re-bind, leaving natives.size()==1 so the
    // cap tripped -- luck, not design.)
    std::vector<bool> consumedFlags(natives.size(), false);
    std::vector<uint32_t> resolvedEids;  // erase from the pending map (retired OR dropped-after-N)
    int confirmedRetired = 0, held = 0;
    for (auto& [eid, p] : g_pendingSaveTimeTwin) {
        const ue_wrap::FVector key{p.x, p.y, p.z};
        const int idx = coop::save_time_retire_util::FindExactMatch(
            natives, consumedFlags, key,
            [&p](const LiveNative& nv) { return p.chipType == kAnyChipType || nv.chipType == p.chipType; });
        // E's current binding -- IsLiveByIndex, never raw IsLive: an element-held pointer may be freed memory
        // after a purge, and IsLive on freed memory misreads (blocked re-binds/retires the whole 20:24 run;
        // [[feedback-islive-unsafe-on-freed-cached-pointer]]).
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
        // b3 OWNER, TIGHTENED (docs/piles/12 eid=4435): a host-vacate twin retires ONLY while E is bound to a
        // live actor. When E is UNBOUND, the native@old may be E's own purge-re-create -- its ONLY expression
        // (re-creates spawn at the GAME's save position, never at @new): retiring it would orphan the eid
        // forever (pile missing client-side + its pos-correction pinned = the 4 Hz drain). Hold (bounded);
        // the identity re-bind claims the native and the correction snaps it @new. An event-armed twin still
        // needs the positive move evidence (E bound FAR from the twin key).
        const bool confirmed = p.hostVacate ? (bound != nullptr)
                                            : (bound && boundD2 > kTwinMovedThresholdCm2);
        // Dead-premise terminalization: an EVENT twin whose eid is bound AT the twin key (<=50cm) can never
        // confirm -- the host put the pile back (or the move never took). Burning kMaxTwinPasses on it is pure
        // walk+log noise (the 20:24 run: 17 such twins x 40 passes of 4 Hz [PILE-1C] spam). Drop it now.
        if (!p.hostVacate && bound && boundD2 >= 0.f && boundD2 <= kTwinMovedThresholdCm2) {
            if (idx >= 0) {
                // An unbound native ALSO sits at the key while E is bound there (two actors <1cm apart) --
                // ambiguous co-located; leave it to the ambiguity-conservative paths. Never retire without
                // positive per-eid move evidence.
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
                // HOLD -- candidate NOT consumed (an unbound-E twin's candidate is likely E's own re-create;
                // the identity re-bind in step 2 of this same pass claims it). Bounded.
                if (++p.unresolvedPasses >= kMaxTwinPasses) resolvedEids.push_back(eid);
                ++held;
            }
        } else {
            // PROBE: why did FindExactMatch MISS? Distinguish clean(0) / ambiguous(>1) / bound-to-wrong-eid.
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
    // This runs ONLY post-quiescence (OnTick gates on HasLoadTailQuiesced), so the load tail is over: a target
    // that is ever going to load has loaded (BindUnboundReCreates ran in step 2 of THIS pass, before us). An
    // entry that still cannot resolve, after a small grace for a late bind, never will -> DROP it. Never keep a
    // deferred destroy forever -- that pins HasPendingWork() true and runs the full-array reconcile 4x/sec.
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

// ---- Queue ARM entry points (event handlers CAPTURE here; they never apply) ----

void ArmPendingSaveTimeTwin(coop::element::ElementId eid, const ue_wrap::FVector& savePos, uint8_t chipType) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    // The host's word supersedes the inference IN BOTH ARM ORDERS: ArmHostVacateTwin already overwrites an
    // event twin, but the reverse arm (a [PILE-09] spawn-miss arriving AFTER the PropSnapPos, the 20:24:32
    // eid=4435 sequence) silently DOWNGRADED the vacate twin back to a confirm-guess event twin. Keep the
    // vacate; the event arm carries no information the host's authoritative arm didn't.
    if (auto it = g_pendingSaveTimeTwin.find(static_cast<uint32_t>(eid));
        it != g_pendingSaveTimeTwin.end() && it->second.hostVacate)
        return;
    // docs/piles/09: record the save-time key for the post-quiescence sweep. No bracket index needed -- the
    // sweep does a FRESH GUObjectArray walk for the late-loaded native@old and matches this key via the shared
    // kernel. Idempotent per eid (the latest grab/land/spawn-miss wins).
    g_pendingSaveTimeTwin[static_cast<uint32_t>(eid)] = PendingTwin{savePos.X, savePos.Y, savePos.Z, chipType};
    UE_LOGI("[PILE-09] CLIENT armed pending save-time twin eid=%u key=(%.1f,%.1f,%.1f) chipType=%u "
            "(in-window grabbed/moved or world-ready-miss pile -> sweep retires the stale native@old at quiescence)",
            static_cast<unsigned>(eid), savePos.X, savePos.Y, savePos.Z, static_cast<unsigned>(chipType));
}

void ArmHostVacateTwin(coop::element::ElementId eid, const ue_wrap::FVector& oldPos) {
    if (eid == 0u || eid == coop::element::kInvalidId) return;
    // b3 OWNER (docs/piles/12): the host's PropSnapPos said E moved off oldPos -> that pos is AUTHORITATIVELY
    // vacated. Arm a host-vacate twin (wildcard chipType, retire without the position-confirm guess). Overwrites
    // any event-armed twin for this eid -- the host's word supersedes the inference. Idempotent.
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
    // F1 piece 2 (2026-07-09): resolve the local player once for the per-entry settled-skip below (skip a
    // correction whose actor is now held by THIS client). Cold path (the quiescence drain / a late arrival).
    void* localPlayer = coop::players::Registry::Get().Local();
    for (auto it = g_pendingPosCorrection.begin(); it != g_pendingPosCorrection.end(); ) {
        const uint32_t eid = it->first;
        PendingPosCorrection& c = it->second;
        coop::element::Element* el = coop::element::Registry::Get().Get(eid);
        void* actor = el ? el->GetActor() : nullptr;
        // IsLiveByIndex, never raw IsLive: a purge frees the bound actor's memory while the row lingers; raw
        // IsLive on the freed pointer can misread TRUE and "apply" the snap onto freed memory.
        if (!actor || !el || !R::IsLiveByIndex(actor, el->GetInternalIdx())) {
            // Not bound/loaded yet -- retry next drain, BOUNDED. An eid that never becomes bindable (the
            // 20:24 eid=4435 shape before the re-bind fix) must not pin HasPendingWork forever (twins cap,
            // destroys cap; this was the uncapped leg of the 4 Hz drain). The identity re-bind is the primary
            // fix; this is the terminal backstop, dropped LOUD so a recurrence is visible in the log.
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
        // SETTLED RE-VALIDATION (F1 piece 2, 2026-07-09): the actor is LIVE, but a grab/convert may have landed
        // BETWEEN arm (PropSnapPos receipt) and apply (this sweep). If it did, eid E now renders as a CARRIED
        // clump (a grabbed pile) or a held prop -- SetActorLocation here would fling the carried body to a ghost
        // pos (the latent bug: this loop USED to re-check only liveness). A grabbed ROCK is already gone (hold-R
        // pickup destroys the world actor -> the liveness check above drops it), so this is the PILE/clump guard.
        // Defer bounded (share unresolvedPasses with the not-bound leg) so a TRANSIENT grab still gets its
        // correction after release; drop LOUD if it stays held past the cap rather than pinning the drain.
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
        // A save-loaded chipPile native rests at STATIC mobility -> SetActorLocation silently no-ops (the K2
        // call returns true but the actor never moves), so the b3 snap "applied" with the full divergence
        // still present (drift unchanged = the long-standing "moved pile stays wrong on the client unless the
        // host re-interacts" bug; kerfurs reconcile because an NPC is Movable). Force the root Movable FIRST so
        // the teleport takes. [[lesson-runtime-staticmeshactor-must-be-movable]]
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
    // Arm-if-absent (2026-07-03, the savePos re-bind assist): when the identity re-bind claims a purge
    // re-create at savePos for an eid whose host position is elsewhere, the actor must still be SNAPPED to the
    // host pos. Usually the original PropSnapPos correction is still pending (it was what pinned the drain);
    // when it already applied-and-erased before the churn, re-arm from the identity map's hostPos overlay. An
    // armed correction keeps its (fresher, host-sent) rotation -- never overwrite it with this reconstruction.
    const auto key = static_cast<uint32_t>(eid);
    if (g_pendingPosCorrection.count(key) > 0) return;
    ArmPendingPosCorrection(eid, loc, rot);
}

void CancelPendingSaveTimeTwin(coop::element::ElementId eid) {
    // The savePos re-bind just bound the native AT the twin's key to E itself -- the "stale @old copy" premise
    // is dead (there is exactly ONE native there, and it IS E; the >1 co-located case never re-binds, by the
    // FindExactMatch ambiguity skip). Without the cancel the twin burns kMaxTwinPasses of 4 Hz walk+log noise
    // before dropping (it can never match: the native it would retire is now BOUND, excluded from candidates).
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

// ---- HasPendingWork / the sequence / the triggers / Reset ----

// v106b GHOST-SWEEP arm (2026-07-07, the wholesale "bring the client world to the host's at
// once" adjudication). Set by the events that can strand an identity-less native chipPile on
// the client (a rebind displacing a live native; an E-press landing on an unbound native);
// consumed by RunReconcile -- step 2's BindUnboundReCreates GHOST-RETIRE tail retires EVERY
// such ghost in one pass. A bool, not a queue: the pass re-derives the full ghost set itself.
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

void RunReconcile(bool joinSweep) {
    // Order matters (preserved from the join one-shot tail this was extracted from):
    // twin-destroy + kerfur-retire BEFORE / around the position re-bind, the deferred destroy AFTER the bind
    // (so its target resolves), and b3 pos-correction LAST (it resolves the now-bound eid). Each step is
    // internally bounded to armed/pending work, so a steady-state pass with nothing pending is cheap.
    SweepReconcileSaveTimeTwins();                               // 1: retire stale native chipPile@old (pile twin)
    bool ghostDrained = true;
    coop::save_identity_bind::BindUnboundReCreates(&ghostDrained);  // 2: re-bind unbound natives (identity layer; chip=pos, kerfur=key)
                                                                 //    + the v106b GHOST-RETIRE tail (post-quiescence: retire every
                                                                 //    identity-less native pile the binds could not claim)
    // The arm is consumed only when the tail DRAINED (2026-07-10 audit pass-cap): a
    // capped pass or a >50% valve abort keeps the sweep armed so the next reconcile
    // pass finishes the ghost set instead of stranding it mid-drain.
    if (ghostDrained) g_ghostSweepArmed = false;
    else UE_LOGI("quiescence_drain: GHOST-SWEEP kept armed (retire tail capped/valved this pass)");
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
    g_ghostSweepArmed = false;  // v106b: session teardown drops the pending ghost adjudication too
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

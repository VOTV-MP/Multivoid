// coop/element/identity_reconcile.cpp -- see coop/element/identity_reconcile.h.

#include "coop/element/identity_reconcile.h"

#include "coop/props/pile_reconcile.h"
#include "coop/props/prop_element_tracker.h"  // InPurgeEpisode (post-purge re-bind trigger)
#include "coop/props/remote_prop_spawn.h"   // HasLoadTailQuiesced
#include "coop/props/save_identity_bind.h"
#include "ue_wrap/log.h"

#include <chrono>

namespace coop::element {

namespace {
// Debounce for the steady-state reconcile: when work is pending we want to retire
// the stale twin promptly (so a grabbed save-pile's ghost@old does not linger),
// but not re-walk the GUObjectArray every tick. 250 ms is well under human-visible
// for a ghost to vanish and bounds the walk rate. Game-thread only -> a plain
// static is safe.
constexpr auto kSteadyReconcileDebounce = std::chrono::milliseconds(250);
std::chrono::steady_clock::time_point g_lastSteadyReconcile{};

// Post-purge re-bind window (the variant-1 trigger). A mass-purge (engine GC during a save-transfer join)
// reaps the bound save-natives; the game then RE-CREATES them UNBOUND. The join one-shot sweep often fires
// DURING the purge (before the re-creates land) or aborts on the incomplete snapshot, so variant-1 must
// also run AFTER the purge drains. While InPurgeEpisode is active we just remember the time; for this window
// AFTER it clears we run RunIdentityReconcile on the debounce so variant-1 re-binds the re-created natives.
constexpr auto kPostPurgeWindow = std::chrono::seconds(6);
std::chrono::steady_clock::time_point g_lastPurgeAt{};
}  // namespace

void RunIdentityReconcile(bool joinSweep) {
    // Order matters (preserved from the join one-shot tail it was extracted from):
    // twin-destroy BEFORE the position re-bind (a destroyed stale twin must not be
    // a re-bind candidate), and b3 pos-correction LAST (it resolves the now-bound
    // eid). Each step is internally bounded to armed/pending work, so a steady-
    // state pass with nothing pending is cheap (early-returns inside each).
    coop::pile_reconcile::SweepReconcileSaveTimeTwins();          // D1: retire stale native@old
    coop::save_identity_bind::BindUnboundReCreates();             // re-bind unbound natives (chip=pos, kerfur=key)
    coop::pile_reconcile::ApplyPendingPosCorrections();           // b3: snap window-moved piles
    if (joinSweep) coop::pile_reconcile::LogCensus();             // one-shot orphan census (join only)
}

void OnReconcileTick() {
    // Steady-state trigger (the D1 structural fix). Only meaningful after the join
    // load tail has drained -- before that, the join one-shot owns reconcile and a
    // mid-load native set is not yet trustworthy. Then: only when there is armed
    // reconcile work, and only on the debounce edge, do the (bounded) walk.
    const auto now = std::chrono::steady_clock::now();
    // Track the purge BEFORE the quiescence gate: the join-window mass-purge happens DURING the load tail
    // (before the sweep fires = before HasLoadTailQuiesced), so recording it must NOT be gated on quiescence
    // (the 15:44 bug: the gate ran first, so g_lastPurgeAt was never set and the post-purge window never
    // opened). We only ACT (run the reconcile) post-quiescence, but we always REMEMBER the purge.
    const bool purging = coop::prop_element_tracker::InPurgeEpisode();
    if (purging) g_lastPurgeAt = now;
    if (!coop::remote_prop_spawn::HasLoadTailQuiesced()) return;
    const bool postPurge = g_lastPurgeAt.time_since_epoch().count() != 0 && !purging &&
                           (now - g_lastPurgeAt) < kPostPurgeWindow;
    // Run when there's armed twin/b3 work (D1) OR we're in the post-purge window (variant-1 re-binds the
    // re-created natives). Otherwise nothing to do -- no GUObjectArray walk (perf rule).
    if (!coop::pile_reconcile::HasPendingWork() && !postPurge) return;
    if (now - g_lastSteadyReconcile < kSteadyReconcileDebounce) return;
    g_lastSteadyReconcile = now;
    UE_LOGI("sync_reconcile: steady-state identity reconcile (%s past quiescence)",
            postPurge ? "post-purge window -- re-binding GC-churned natives (variant-1)"
                      : "pending twin/b3 work -- a save-pile grabbed/moved after the join sweep");
    RunIdentityReconcile(/*joinSweep=*/false);
}

}  // namespace coop::element

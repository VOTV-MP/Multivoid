// coop/session/world_load_episode.cpp -- see coop/session/world_load_episode.h.
// The episode latch plus the load-tail quiescence probe that gates the ClientWorldReady announce.

#include "coop/session/world_load_episode.h"

#include "coop/config/config.h"  // ReadEnv, for the drill switch

#include "coop/creatures/npc_sync.h"          // IsAllowlistedClass (the NPC load tail)
#include "coop/props/prop_element_tracker.h"  // HasSeededOnce / InPurgeEpisode (purge-aware progress)
#include "coop/props/prop_lifecycle.h"        // IsPerPlayerPropClass
#include "ue_wrap/core/hot_path_guard.h"           // UE_ASSERT_GAME_THREAD (probe fields are GT-owned)
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <chrono>
#include <string>

namespace coop::world_load_episode {

namespace R = ue_wrap::reflection;

namespace {

// g_inEpisode is atomic: Arm() runs on the harness timeline thread, the destroy seam reads on the
// game thread, and the roll census tags records from ProcessEvent worker threads. Advisory, so
// staleness is fine.
std::atomic<bool> g_inEpisode{false};

// The probe-session open request. Arm() may NOT touch the plain probe fields below: it runs off the
// game thread and would race the game-thread ticker, one of those fields being a std::string. Arm
// raises this atomic only; the ticker consumes it and opens the session on the game thread.
// HasQuiesced() reads a pending request as "session open", so the window between an off-thread Arm
// and the first tick cannot leak an early announce.
std::atomic<bool> g_joinProbeRequested{false};

// ---- Quiescence-probe session state (game thread only; no mutex) ----
bool g_probeOpen  = false;     // a session is open, probing
bool g_everOpened = false;     // any session opened since Reset. While false,
                               // HasQuiesced() is VACUOUSLY true: a flow that never
                               // saw a world load has nothing to wait for
bool g_quiesced   = false;     // the latch of the most recent session
std::chrono::steady_clock::time_point g_probeArmedAt{};      // absolute-ceiling base
std::chrono::steady_clock::time_point g_lastProgressAt{};    // no-progress deadline base (reset on purge drain / moving population)
std::chrono::steady_clock::time_point g_lastScanAt{};        // {} => not yet scanned this session
std::chrono::steady_clock::time_point g_quiescedAt{};        // valid while g_quiesced
int  g_lastUnsettledCount = -1;
int  g_stableScans        = 0;
std::string g_probeReason;     // logged at arm + latch

// The probe cadence, stability window and two-tier deadline: the same constants the divergence
// sweep has trusted for its destructive adjudication gate. The announce may not use a looser gate
// than that sweep does.
constexpr int kScanIntervalMs   = 200;    // 5 Hz while a session is open
constexpr int kQuiesceScans     = 10;     // a stable population across this many scans means
                                          // the async load pass has drained. The NPCs load
                                          // well after the props, so a shorter window
                                          // false-signals mid-load
constexpr int kNoProgressMs     = 45000;  // since the last progress, not since the arm:
                                          // fires only after this long with nothing
                                          // happening, never during a draining purge
constexpr int kAbsoluteCeilingMs = 120000;  // since the arm, the stuck-purge backstop; the announce then goes degraded

// ---- The reconcile window; see the header block ----
// `up` and `kindIsLoad` are atomics because Arm() raises off the game thread; every other writer
// and both readers are on it. raisedAt and completeSinceArm are game-thread only, so Arm's raise
// materialises its game-thread half through g_reconRaiseRequested. The ceiling starting one tick
// late is harmless.
std::atomic<bool> g_reconUp{false};
std::atomic<bool> g_reconKindLoad{true};
std::atomic<bool> g_reconRaiseRequested{false};
std::chrono::steady_clock::time_point g_reconRaisedAt{};  // game thread; rising edge only, never restamped by Begin
bool g_reconCompleteSinceArm = false;                     // game thread; the kind classifier
constexpr int kReconCeilingMs = 180000;  // well clear of the slowest measured arm-to-complete

// Game-thread rising-edge raise, shared by the raise sites and the Arm-request consume.
void ReconRaiseGT_(bool kindLoad, const char* who) {
    g_reconKindLoad.store(kindLoad, std::memory_order_relaxed);
    if (!g_reconUp.exchange(true, std::memory_order_relaxed)) {
        g_reconRaisedAt = std::chrono::steady_clock::now();
        UE_LOGI("world_load_episode: reconcile window RAISED (kind=%s by=%s completeSinceArm=%d)",
                kindLoad ? "load" : "midSessionBracket", who, g_reconCompleteSinceArm ? 1 : 0);
    }
}

void ReconLowerGT_(const char* why) {
    if (g_reconUp.exchange(false, std::memory_order_relaxed)) {
        UE_LOGI("world_load_episode: reconcile window LOWERED (%s)", why);
    }
}

// Load-tail population census: two populations whose sum settles exactly when the async load pass
// finishes materialising the world -- the allowlisted NPCs, and the keyless in-universe props that
// have not minted a key yet. Live chipPiles count unconditionally, because a join-time world reload
// drops the field and the async reload climbs it back, so the gate refuses to quiesce through
// either half of that reload, including the window where the purge has started but is not yet
// flagged. One GUObjectArray walk, pure pointer-compare class filters before any key or name read,
// throttled and only while a session is open. Stability is about population CHANGE, not membership:
// a claimed still-keyless actor contributes a constant term that cannot block the latch.
int CountLoadTailUnsettled_() {
    const int32_t n = R::NumObjects();
    int unsettled = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        // (a) The allowlisted-NPC load tail. Lineage test first, so NameOf and IsLive run only for
        // the handful that pass. An unresolved allowlist degrades to prop-only quiescence.
        if (coop::npc_sync::IsAllowlistedClass(cls)) {
            if (!R::IsLive(obj)) continue;
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // CDO
            ++unsettled;
            continue;
        }
        // (b) The keyless-prop load tail.
        if (!ue_wrap::prop::IsClassKeyedInteractable(cls)) continue;
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // CDO (alloc-free)
        // (b') The chipPile field, counted even mid-purge.
        if (ue_wrap::prop::IsChipPile(obj)) { ++unsettled; continue; }
        if (coop::prop_lifecycle::IsPerPlayerPropClass(R::ClassNameOf(obj))) continue;
        const std::wstring key = ue_wrap::prop::GetInteractableKeyString(obj);
        if (key.empty() || key == L"None") ++unsettled;
    }
    return unsettled;
}

void OpenProbeSession_(const char* reason) {
    g_probeOpen = true;
    g_everOpened = true;
    g_quiesced = false;
    g_probeArmedAt = std::chrono::steady_clock::now();
    g_lastProgressAt = g_probeArmedAt;
    g_lastScanAt = {};
    g_lastUnsettledCount = -1;
    g_stableScans = 0;
    g_probeReason = reason ? reason : "?";
    UE_LOGI("world_load_episode: quiescence probe ARMED (%s) -- population stable x%d scans @%dms, "
            "no-progress %ds, absolute ceiling %ds",
            g_probeReason.c_str(), kQuiesceScans, kScanIntervalMs, kNoProgressMs / 1000,
            kAbsoluteCeilingMs / 1000);
}

void Latch_(const char* how) {
    g_probeOpen = false;
    g_quiesced = true;
    g_quiescedAt = std::chrono::steady_clock::now();
    const bool closesEpisode = g_inEpisode.load(std::memory_order_relaxed);
    if (closesEpisode) g_inEpisode.store(false, std::memory_order_relaxed);
    UE_LOGI("world_load_episode: load-tail QUIESCED (%s; session '%s')%s",
            how, g_probeReason.c_str(),
            closesEpisode ? " -- episode CLOSED, KEYED-prop destroy broadcasts resume" : "");
}

}  // namespace

void Arm() {
    // Off-game-thread safe: touches only the atomics. The probe session opens on the next
    // game-thread tick through the request flag. Opening it inline here would race the ticker on
    // eight plain fields.
    if (g_inEpisode.load(std::memory_order_relaxed)) return;  // idempotent: one arm per world load
    g_inEpisode.store(true, std::memory_order_relaxed);
    // Raise the reconcile window, kind load. Atomics here; the game-thread half, the raisedAt stamp
    // and the classifier reset, materialises on the next tick through the request below.
    g_reconKindLoad.store(true, std::memory_order_relaxed);
    g_reconUp.store(true, std::memory_order_relaxed);
    g_reconRaiseRequested.store(true, std::memory_order_release);
    g_joinProbeRequested.store(true, std::memory_order_release);
    UE_LOGI("world_load_episode: ARMED -- client world-load starting; KEYED-prop destroy broadcasts "
            "suppressed until load-tail quiescence (host-wipe root fix); probe session opens on the "
            "next game-thread tick");
}

void ArmQuiesceProbe(const char* reason) {
    UE_ASSERT_GAME_THREAD("world_load_episode::ArmQuiesceProbe");
    OpenProbeSession_(reason);
}

bool TickQuiesceProbe() {
    UE_ASSERT_GAME_THREAD("world_load_episode::TickQuiesceProbe");
    // Materialise a pending off-thread raise: stamp the rising edge and reset the classifier here
    // on the game thread. Before the early return below, since this and the ceiling must run every
    // tick.
    if (g_reconRaiseRequested.exchange(false, std::memory_order_acq_rel)) {
        g_reconCompleteSinceArm = false;
        g_reconRaisedAt = std::chrono::steady_clock::now();
        g_reconUp.store(true, std::memory_order_relaxed);  // re-assert, so a stale raisedAt cannot force-lower the ceiling here
        UE_LOGI("world_load_episode: reconcile window RAISED (kind=load by=Arm completeSinceArm=0)");
    }
    // The ceiling, anchored to the rising edge since Begin never restamps. It bounds every stuck
    // shape: a lost bracket on the reload path, and Begin-without-Complete abort loops.
    if (g_reconUp.load(std::memory_order_relaxed) &&
        g_reconRaisedAt.time_since_epoch().count() != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - g_reconRaisedAt).count() >= kReconCeilingMs) {
        UE_LOGW("world_load_episode: reconcile window CEILING (%d s) -- force-lowering (lost "
                "bracket / pathological reconcile; suppression must not outlive the player's "
                "patience)", kReconCeilingMs / 1000);
        ReconLowerGT_("ceiling");
    }
    // Consume a pending session request: the session opens here, on the game thread, so the plain
    // probe fields below are single-thread-owned.
    if (g_joinProbeRequested.exchange(false, std::memory_order_acq_rel))
        OpenProbeSession_("join world-load");
    if (!g_probeOpen) return HasQuiesced();  // steady, latched or vacuous: bool reads only
    const auto now = std::chrono::steady_clock::now();
    const auto msSince = [now](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count();
    };
    // Throttle the scan; the first scan after arming is not skipped, its timestamp being the epoch.
    if (g_lastScanAt.time_since_epoch().count() != 0 &&
        msSince(g_lastScanAt) < kScanIntervalMs) return false;
    g_lastScanAt = now;

    // The two-tier deadline: the no-progress timer or the absolute ceiling. The ceiling fires even
    // through a stuck purge, so the announce can never defer forever; the no-progress timer never
    // pre-empts a legitimately draining purge, which keeps resetting the progress stamp below.
    if (msSince(g_probeArmedAt) >= kAbsoluteCeilingMs) {
        UE_LOGW("world_load_episode: probe ABSOLUTE ceiling (%d s) -- latching DEGRADED (stuck "
                "purge / pathological load; the settled-world guarantee does NOT hold for this join)",
                kAbsoluteCeilingMs / 1000);
        Latch_("ABSOLUTE ceiling -- DEGRADED");
        return true;
    }
    if (msSince(g_lastProgressAt) >= kNoProgressMs) {
        UE_LOGW("world_load_episode: probe NO-PROGRESS deadline (%d s) -- latching DEGRADED (the "
                "load stalled; the settled-world guarantee does NOT hold for this join)",
                kNoProgressMs / 1000);
        Latch_("no-progress deadline -- DEGRADED");
        return true;
    }
    // A registry mid-purge, or one never seeded, means the loading world is incomplete. A draining
    // purge IS progress, so reset the no-progress base; the stability run restarts once the world
    // re-seeds.
    if (!coop::prop_element_tracker::HasSeededOnce() ||
        coop::prop_element_tracker::InPurgeEpisode()) {
        g_lastProgressAt = now;
        g_lastUnsettledCount = -1;
        g_stableScans = 0;
        return false;
    }
    const int unsettled = CountLoadTailUnsettled_();
    if (unsettled != g_lastUnsettledCount) {
        g_lastUnsettledCount = unsettled;
        g_lastProgressAt = now;  // population still moving = progress
        g_stableScans = 0;
        return false;
    }
    if (++g_stableScans < kQuiesceScans) return false;  // stable, but not for long enough yet
    Latch_("population stable");
    return true;
}

bool HasQuiesced() {
    if (g_joinProbeRequested.load(std::memory_order_acquire)) return false;  // arm raised, session not yet open
    if (g_probeOpen) return false;    // mid-load: a session is probing
    if (!g_everOpened) return true;   // vacuous: no world-load observed since Reset -- nothing to wait for
    return g_quiesced;
}

long long MsSinceQuiesced() {
    if (!g_quiesced) return -1;
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - g_quiescedAt).count();
}

void Reset() {
    g_inEpisode.store(false, std::memory_order_relaxed);
    g_joinProbeRequested.store(false, std::memory_order_relaxed);
    g_probeOpen = false;
    g_everOpened = false;  // back to vacuous: a reconnect with no world change announces freely
    g_quiesced = false;
    g_lastUnsettledCount = -1;
    g_stableScans = 0;
    // Session teardown lowers the reconcile window and clears its classifier.
    g_reconRaiseRequested.store(false, std::memory_order_relaxed);
    ReconLowerGT_("Reset");
    g_reconCompleteSinceArm = false;
    g_reconRaisedAt = {};
}

bool InEpisode() { return g_inEpisode.load(std::memory_order_relaxed); }

// ---- The reconcile window; see the header block ----

void RaiseReconcileForReload() {
    UE_ASSERT_GAME_THREAD("world_load_episode::RaiseReconcileForReload");
    g_reconCompleteSinceArm = false;  // a reload restarts the classifier; a reload IS a load
    ReconRaiseGT_(/*kindLoad*/ true, "reload-arm");
}

void NoteReconcileBegin() {
    UE_ASSERT_GAME_THREAD("world_load_episode::NoteReconcileBegin");
    if (g_reconUp.load(std::memory_order_relaxed)) {
        // Refresh: the kind is kept and the ceiling is not restamped, the rising edge owning it. A
        // normal join's bracket begins with the window already up from Arm, so it stays kind load
        // throughout.
        UE_LOGI("world_load_episode: reconcile Begin (window up, kind=%s kept, completeSinceArm=%d)",
                g_reconKindLoad.load(std::memory_order_relaxed) ? "load" : "midSessionBracket",
                g_reconCompleteSinceArm ? 1 : 0);
        return;
    }
    // Down, so raise. The kind is load exactly when no snapshot completed since the arm -- on the
    // join path, "the curtain never dropped". A late bracket after the flake backstop and an
    // aborted re-bracket both classify as load: the player could still only act blindly.
    ReconRaiseGT_(/*kindLoad*/ !g_reconCompleteSinceArm, "Begin");
}

void NoteReconcileComplete() {
    UE_ASSERT_GAME_THREAD("world_load_episode::NoteReconcileComplete");
    // Unconditional: a Complete arriving after a ceiling force-lower must still flip the
    // classifier, or the next mid-session bracket would misclassify as a load.
    g_reconCompleteSinceArm = true;
    ReconLowerGT_("SnapshotComplete");
}

void NoteBracketFlake() {
    UE_ASSERT_GAME_THREAD("world_load_episode::NoteBracketFlake");
    // The lost-bracket flake backstop. It lowers WITHOUT touching the classifier: no Complete
    // happened, so a late real bracket's Begin must still classify as a load.
    ReconLowerGT_("bracket-flake backstop");
}

bool InReconcileWindow() {
    // Drill calibration: disabling the window reproduces the old close edge, so the drill's
    // destroys broadcast. The instrument must be shown able to see the phenomenon before a green
    // run counts. Never set outside a drill.
    static const bool sDisabled =
        !coop::config::ReadEnv("VOTVCOOP_RECON_DISABLE").empty();
    if (sDisabled) return false;
    return g_reconUp.load(std::memory_order_relaxed);
}

bool ReconcileWindowIsLoadKind() { return g_reconKindLoad.load(std::memory_order_relaxed); }

}  // namespace coop::world_load_episode

// coop/session/world_load_episode.cpp -- see coop/session/world_load_episode.h.
// The episode latch plus the load-tail quiescence probe that gates the ClientWorldReady announce.

#include "coop/session/world_load_episode.h"

#include "coop/config/config.h"  // ReadEnv, for the drill switch
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/session/save_replay_watch.h"

#include "coop/creatures/npc_sync.h"          // IsAllowlistedClass (the NPC load tail)
#include "coop/props/prop_element_tracker.h"  // HasSeededOnce / InPurgeEpisode (purge-aware progress)
#include "coop/props/prop_lifecycle.h"        // IsPerPlayerPropClass
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"           // UE_ASSERT_GAME_THREAD (probe fields are GT-owned)
#include "ue_wrap/core/log.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/reflection.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace coop::world_load_episode {

namespace R = ue_wrap::reflection;

namespace {

// g_inEpisode is atomic: Arm() runs on the harness timeline thread, the destroy seam reads on the
// game thread, and the roll census tags records from ProcessEvent worker threads. Advisory, so
// staleness is fine.
std::atomic<bool> g_inEpisode{false};

// A world change's replay must return within this. MTA's constants bound the connect and the
// client's verification, not a load (NET_CONNECT_TIMEOUT 30 s, CLIENT_VERIFICATION_TIMEOUT 10 s,
// reference/mtasa-blue/Client/mods/deathmatch/logic/Config.h:105-106); this one bounds a world's
// save replay, and is 120 s because a world's load frame can block the game thread for that long.
constexpr int64_t kWorldChangeReplayBoundMs = 120000;

// The probe-session open request (a Request value). Arm() may NOT touch the plain probe fields
// below: it runs off the game thread and would race the game-thread ticker, one of those fields
// being a std::string. Arm raises this atomic only; the ticker consumes it and opens the session on
// the game thread, once the loaded world's save replay has returned. HasQuiesced() reads a pending
// request as "session open", so the window between an off-thread Arm and the session cannot leak an
// early announce.
std::atomic<uint8_t> g_request{0};
std::atomic<uint64_t> g_requestRaisedAtMs{0};  // coop::net::NowMs() when a world change raised it

// The world this connection last settled on; its identity, not its address.
ue_wrap::CachedObjRef g_baseline;

// Per request: the log said once, and why the request failed.
bool g_sweepSkipSaid = false;
RequestFailureInfo g_failure;

// ---- Quiescence-probe session state (game thread only; no mutex) ----
bool g_probeOpen  = false;     // a session is open, probing
bool g_everOpened = false;     // any session opened since Reset. While false,
                               // HasQuiesced() is VACUOUSLY true: a flow that never
                               // saw a world load has nothing to wait for
bool g_quiesced   = false;     // the latch of the most recent session
std::chrono::steady_clock::time_point g_probeArmedAt{};      // absolute-ceiling base
std::chrono::steady_clock::time_point g_lastScanAt{};        // {} => not yet scanned this session
std::chrono::steady_clock::time_point g_quiescedAt{};        // valid while g_quiesced
int  g_lastUnsettledCount = -1;
int  g_stableScans        = 0;
std::string g_probeReason;     // logged at arm + latch

// The probe cadence, stability window and ceiling: the same constants the divergence sweep has
// trusted for its destructive adjudication gate. The announce may not use a looser gate than that
// sweep does.
constexpr int kScanIntervalMs   = 200;    // 5 Hz while a session is open
constexpr int kQuiesceScans     = 10;     // a stable population across this many scans means
                                          // the async load pass has drained. The NPCs load
                                          // well after the props, so a shorter window
                                          // false-signals mid-load
// No deadline measures time without progress. Every scan finds a registry mid-purge or unseeded, a
// population that moved, or one that held still and latches as stable within kQuiesceScans scans,
// so the only stretch such a deadline could ever see is one the probe did not scan at all -- one
// script body holding every tick, a world load's tail -- and it would call the load stalled on the
// first scan after that body, the population having moved throughout it.
constexpr int kAbsoluteCeilingMs = 120000;  // since the session opened, the stuck-purge backstop; the announce then goes degraded

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
const char* g_reconLastLowerWhy = "";                     // game thread; the last lowering's reason (a literal)
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
        g_reconLastLowerWhy = why;
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

void OpenProbeSession_(const char* reason, bool forRequest) {
    g_probeOpen = true;
    g_everOpened = true;
    g_quiesced = false;
    g_probeArmedAt = std::chrono::steady_clock::now();
    g_lastScanAt = {};
    g_lastUnsettledCount = -1;
    g_stableScans = 0;
    g_probeReason = reason ? reason : "?";
    char replay[64] = "";
    if (forRequest) {
        if (save_replay_watch::HasRecord())
            std::snprintf(replay, sizeof replay, "; save replay returned %lld ms before the open",
                          save_replay_watch::MsFromPostTo(g_probeArmedAt));
        else
            std::snprintf(replay, sizeof replay, "; no save replay recorded");
    }
    UE_LOGI("world_load_episode: quiescence probe ARMED (%s%s) -- population stable x%d scans @%dms, "
            "absolute ceiling %ds",
            g_probeReason.c_str(), replay, kQuiesceScans, kScanIntervalMs, kAbsoluteCeilingMs / 1000);
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
    // Off-game-thread safe: touches only the atomics. The probe session opens on a game-thread
    // tick through the request, once the save replay has returned. Opening it inline here would
    // race the ticker on eight plain fields.
    if (g_inEpisode.load(std::memory_order_relaxed)) return;  // idempotent: one arm per world load
    g_inEpisode.store(true, std::memory_order_relaxed);
    // Raise the reconcile window, kind load. Atomics here; the game-thread half, the raisedAt stamp
    // and the classifier reset, materialises on the next tick through the request below.
    g_reconKindLoad.store(true, std::memory_order_relaxed);
    g_reconUp.store(true, std::memory_order_relaxed);
    g_reconRaiseRequested.store(true, std::memory_order_release);
    g_request.store(static_cast<uint8_t>(Request::Join), std::memory_order_release);
    UE_LOGI("world_load_episode: ARMED -- client world-load starting; KEYED-prop destroy broadcasts "
            "suppressed until load-tail quiescence (host-wipe root fix); probe session opens once "
            "the world's save replay has returned");
}

void ArmQuiesceProbe(const char* reason) {
    UE_ASSERT_GAME_THREAD("world_load_episode::ArmQuiesceProbe");
    if (g_request.load(std::memory_order_acquire) != 0) {
        if (!g_sweepSkipSaid)
            UE_LOGI("world_load_episode: %s waits on the pending request's probe session", reason);
        g_sweepSkipSaid = true;
        return;
    }
    OpenProbeSession_(reason, /*forRequest*/ false);
}

void InstallReplayWatch(coop::net::Session& session) {
    save_replay_watch::Drive(session, AwaitingSaveReplay());
}

bool RequestWorldChangeProbe() {
    UE_ASSERT_GAME_THREAD("world_load_episode::RequestWorldChangeProbe");
    uint8_t expect = 0;
    if (g_request.compare_exchange_strong(expect, static_cast<uint8_t>(Request::WorldChange),
                                          std::memory_order_acq_rel)) {
        g_requestRaisedAtMs.store(coop::net::NowMs(), std::memory_order_relaxed);
        return true;
    }
    return expect == static_cast<uint8_t>(Request::WorldChange);
}

bool AwaitingSaveReplay() { return g_request.load(std::memory_order_acquire) != 0; }

bool HasBaselineWorld() { return g_baseline.Raw() != nullptr; }

bool IsBaselineWorld(void* world) {
    return world && world == g_baseline.Raw() && g_baseline.Is(world);
}

void SetBaselineWorld(void* world) {
    if (world) g_baseline.Set(world);  // a null read keeps the baseline held; an empty one hides a later change
}

void ClearBaselineWorld() { g_baseline.Reset(); }

int64_t PendingRequestAgeMs() {
    if (g_request.load(std::memory_order_acquire) != static_cast<uint8_t>(Request::WorldChange)) return -1;
    return static_cast<int64_t>(coop::net::NowMs() - g_requestRaisedAtMs.load(std::memory_order_relaxed));
}

const RequestFailureInfo& RequestFailure() { return g_failure; }

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
    // A pending request: no session until the loaded world's save replay has returned, then the
    // session opens here, on the game thread, so the plain probe fields below are single-thread-owned.
    const auto req = static_cast<Request>(g_request.load(std::memory_order_acquire));
    if (req != Request::None) {
        void* cw = nullptr;  // the world the replay test judged, for the baseline
        g_probeOpen = false;
        g_quiesced = false;  // (1) no session while a request pends
        if (g_failure.failure == Failure::None && (save_replay_watch::Refused() || save_replay_watch::Dead())) {
            // (2) the watch cannot see the replay
            const std::string row = save_replay_watch::RowText();
            const std::string text = "the save-replay watch (" + row + ") is not live";
            g_failure = {Failure::WatchDead, text};
            UE_LOGE("world_load_episode: the save-replay watch (%s) is not live -- %s", row.c_str(),
                    save_replay_watch::Refused() ? "the script gate refused it"
                                                 : "its function does not exist in this game build");
        } else if (g_failure.failure == Failure::None &&
                   (save_replay_watch::ReturnedInThisWorld(&cw) || save_replay_watch::GateBreak())) {
            // (3) the replay returned: the request is consumed and its session opens. An Arm that
            // replaced the request this tick, off the game thread, fails the exchange; the next tick
            // sees the new one.
            uint8_t expect = static_cast<uint8_t>(req);
            if (g_request.compare_exchange_strong(expect, 0, std::memory_order_acq_rel)) {
                SetBaselineWorld(cw);
                g_sweepSkipSaid = false;
                OpenProbeSession_(static_cast<Request>(expect) == Request::Join ? "join world-load"
                                                                                : "world-change re-announce",
                                  /*forRequest*/ true);
            }
        } else if (g_failure.failure == Failure::None && req == Request::WorldChange &&
                   PendingRequestAgeMs() >= kWorldChangeReplayBoundMs) {
            // (4) the world change's bound
            g_failure = {Failure::ReplayTimeout, "the new world's save replay never returned"};
            UE_LOGE("world_load_episode: the new world's save replay never returned in %lld s (or this "
                    "machine cannot name its world)", static_cast<long long>(kWorldChangeReplayBoundMs / 1000));
        }
    }
    if (!g_probeOpen) return HasQuiesced();  // steady, latched or vacuous: bool reads only
    const auto now = std::chrono::steady_clock::now();
    const auto msSince = [now](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count();
    };
    // Throttle the scan; the first scan after arming is not skipped, its timestamp being the epoch.
    if (g_lastScanAt.time_since_epoch().count() != 0 &&
        msSince(g_lastScanAt) < kScanIntervalMs) return false;
    g_lastScanAt = now;

    // The absolute ceiling fires even through a stuck purge, so an open session always latches; the wait
    // for the save replay before it has its own end (a failure, or the join phase's watchdog).
    if (msSince(g_probeArmedAt) >= kAbsoluteCeilingMs) {
        UE_LOGW("world_load_episode: probe ABSOLUTE ceiling (%d s) -- latching DEGRADED (stuck "
                "purge / pathological load; the settled-world guarantee does NOT hold for this join)",
                kAbsoluteCeilingMs / 1000);
        Latch_("ABSOLUTE ceiling -- DEGRADED");
        return true;
    }
    // A registry mid-purge, or one never seeded, means the loading world is incomplete; the
    // stability run restarts once the world re-seeds.
    if (!coop::prop_element_tracker::HasSeededOnce() ||
        coop::prop_element_tracker::InPurgeEpisode()) {
        g_lastUnsettledCount = -1;
        g_stableScans = 0;
        return false;
    }
    const int unsettled = CountLoadTailUnsettled_();
    if (unsettled != g_lastUnsettledCount) {
        g_lastUnsettledCount = unsettled;  // the population is still moving
        g_stableScans = 0;
        return false;
    }
    if (++g_stableScans < kQuiesceScans) return false;  // stable, but not for long enough yet
    Latch_("population stable");
    return true;
}

bool HasQuiesced() {
    if (g_request.load(std::memory_order_acquire) != 0) return false;  // request raised, session not yet open
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
    g_request.store(0, std::memory_order_relaxed);
    save_replay_watch::Reset();
    g_baseline.Reset();
    g_failure = {};
    g_sweepSkipSaid = false;
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

const char* LastReconcileLowerReason() { return g_reconLastLowerWhy; }

bool ReconcileWindowIsLoadKind() { return g_reconKindLoad.load(std::memory_order_relaxed); }

}  // namespace coop::world_load_episode

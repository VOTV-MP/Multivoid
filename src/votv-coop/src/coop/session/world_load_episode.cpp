// coop/session/world_load_episode.cpp -- see coop/session/world_load_episode.h.
//
// The episode latch + the load-tail quiescence probe (moved from join_membership_sweep 2026-07-12,
// join-barrier redesign: the probe now gates the ClientWorldReady ANNOUNCE, not just the sweep).

#include "coop/session/world_load_episode.h"

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

// g_inEpisode is atomic (relaxed): Arm() runs on the harness TimelineThread (the client join
// bringup, harness.cpp DriveMenuModeJoinWorldBoot), the destroy seam reads on the GT, and the
// rng_roll_census probe TAGS records from ProcessEvent-dispatching worker threads (advisory,
// staleness fine).
std::atomic<bool> g_inEpisode{false};

// The join probe-session OPEN request (audit CRITICAL 2026-07-12): Arm() itself may NOT touch the
// plain probe-session fields below -- it runs OFF the game thread (TimelineThread), and the fields
// race the GT-driven TickQuiesceProbe (incl. a std::string = UB). Arm only raises this atomic
// request; the GT ticker consumes it and opens the session ON the GT. HasQuiesced() treats a
// pending request as "session open" (false) so the vacuous-true window between the off-GT Arm and
// the first GT tick cannot leak an early announce (belt on top of the worldUp/seed gates).
std::atomic<bool> g_joinProbeRequested{false};

// ---- Quiescence-probe session state (game-thread only; no mutex) ----
bool g_probeOpen  = false;     // a session is open (probing)
bool g_everOpened = false;     // any session opened since Reset -- while false, HasQuiesced() is
                               // VACUOUSLY true: a flow that never observed a world-load (the
                               // env/.bat already-in-world dev client, a reconnect without a world
                               // change) has nothing to wait for and must announce immediately
bool g_quiesced   = false;     // the latch of the most recent session
std::chrono::steady_clock::time_point g_probeArmedAt{};      // absolute-ceiling base
std::chrono::steady_clock::time_point g_lastProgressAt{};    // no-progress deadline base (reset on purge drain / moving population)
std::chrono::steady_clock::time_point g_lastScanAt{};        // {} => not yet scanned this session
std::chrono::steady_clock::time_point g_quiescedAt{};        // valid while g_quiesced
int  g_lastUnsettledCount = -1;
int  g_stableScans        = 0;
std::string g_probeReason;     // logged at arm + latch

// The probe cadence + stability window + two-tier deadline: the SAME constants the divergence
// sweep trusted for its DESTRUCTIVE adjudication gate since 2026-06-15/25 (docs/piles/10). Moved
// here verbatim -- the announce may not use a looser gate than the doom sweep did.
constexpr int kScanIntervalMs   = 200;    // 5 Hz while a session is open
constexpr int kQuiesceScans     = 10;     // population stable across 10 scans (~2 s) = the async
                                          // loadObjects pass has drained (the kerfur NPCs load with
                                          // multi-hundred-ms gaps AFTER the props -- a short window
                                          // false-signals mid-load, the 2026-06-15 kerfur-dupe)
constexpr int kNoProgressMs     = 45000;  // NO-PROGRESS deadline (since last progress, not since arm):
                                          // fires only after this long with NOTHING happening -- a
                                          // genuine stall, never a legitimate ~50 s purge drain
constexpr int kAbsoluteCeilingMs = 120000; // absolute ceiling since arm (stuck-purge backstop); on
                                          // this latch the announce/sweep proceed in DEGRADED mode

// Load-tail population census: counts two populations whose sum settles exactly when the async
// loadObjects pass finishes materializing the world.
//   (a) ALLOWLISTED NPCs (the kerfur load tail) -- live, non-CDO, tracked OR untracked; only
//       loadObjects spawning a new twin perturbs the count.
//   (b) keyless, in-universe, non-per-player, non-chipPile props reading Key=None -- the prop load
//       tail (a straggler that has not minted its key yet).
//   (b') live chipPiles, counted unconditionally (docs/piles/10 purge-aware gate): a join-time
//       world-reload purge DROPS the field and the async reload CLIMBS it back, so the gate refuses
//       to quiesce through EITHER half of the reload -- including the <=4 s window where the purge
//       has physically started but InPurgeEpisode is not yet flagged.
// ONE GUObjectArray walk (pure pointer-compare class filters before any key/name read), throttled
// to 5 Hz, only while a session is open. Moved verbatim from join_membership_sweep 2026-07-12 with
// ONE change: the old `g_claimedActors` skip is dropped -- the probe now (also) runs PRE-snapshot
// where no claims exist, and stability is about population CHANGE, not membership: a claimed
// still-keyless actor contributes a constant term that cannot block the latch.
int CountLoadTailUnsettled_() {
    const int32_t n = R::NumObjects();
    int unsettled = 0;
    for (int32_t i = 0; i < n; ++i) {
        void* obj = R::ObjectAt(i);
        if (!obj) continue;
        void* cls = R::ClassOf(obj);
        // (a) allowlisted-NPC load tail (lineage test first; NameOf/IsLive only run for the handful
        // that pass). A false return (allowlist not yet resolved) degrades to prop-only quiescence.
        if (coop::npc_sync::IsAllowlistedClass(cls)) {
            if (!R::IsLive(obj)) continue;
            if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // CDO
            ++unsettled;
            continue;
        }
        // (b) keyless-prop load tail.
        if (!ue_wrap::prop::IsClassKeyedInteractable(cls)) continue;
        if (!R::IsLive(obj)) continue;
        if (R::NameStartsWith(R::NameOf(obj), L"Default__")) continue;  // CDO (alloc-free)
        // (b') chipPile field, counted even mid-purge (see the header comment above).
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
    // OFF-GT SAFE (TimelineThread): touches ONLY the two atomics. The probe session opens on the
    // next GT TickQuiesceProbe via g_joinProbeRequested (audit CRITICAL 2026-07-12 -- the first cut
    // opened it inline and raced the GT ticker on eight plain fields incl. a std::string).
    if (g_inEpisode.load(std::memory_order_relaxed)) return;  // idempotent -- one arm per world-load
    g_inEpisode.store(true, std::memory_order_relaxed);
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
    // Consume a pending join-session request (raised off-GT by Arm) -- the session opens HERE, on
    // the GT, so the plain probe fields below are single-thread-owned.
    if (g_joinProbeRequested.exchange(false, std::memory_order_acq_rel))
        OpenProbeSession_("join world-load");
    if (!g_probeOpen) return HasQuiesced();  // steady state / latched / vacuous: bool reads only
    const auto now = std::chrono::steady_clock::now();
    const auto msSince = [now](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - t).count();
    };
    // Throttle to ~5 Hz (skip on the very first scan after arming, when g_lastScanAt is epoch 0).
    if (g_lastScanAt.time_since_epoch().count() != 0 &&
        msSince(g_lastScanAt) < kScanIntervalMs) return false;
    g_lastScanAt = now;

    // Two-tier deadline (docs/piles/10 purge-aware gate): the NO-PROGRESS timer OR the ABSOLUTE
    // ceiling. The ceiling fires even through a stuck purge so the announce/sweep can never defer
    // forever; the no-progress timer never pre-empts a legitimately-draining purge (which keeps
    // resetting g_lastProgressAt below).
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
    // REGISTRY MID-PURGE (or never seeded) -> the loading world is INCOMPLETE; a draining purge IS
    // progress (reset the no-progress base). The population-stability run restarts once the world
    // re-seeds (!InPurgeEpisode is the clean "registry re-seeded" edge).
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
    g_everOpened = false;  // back to the vacuous state -- a reconnect without a world change announces freely
    g_quiesced = false;
    g_lastUnsettledCount = -1;
    g_stableScans = 0;
}

bool InEpisode() { return g_inEpisode.load(std::memory_order_relaxed); }

}  // namespace coop::world_load_episode

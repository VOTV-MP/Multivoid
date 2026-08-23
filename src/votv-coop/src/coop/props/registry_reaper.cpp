// coop/props/registry_reaper.cpp -- see coop/props/registry_reaper.h.
// The block body is verbatim from net_pump.cpp's old reaper section; the ONLY
// non-verbatim lines (each a wrapper-table row in the extraction design doc):
// the function head + trailing `return false`, the maybeReAnnounce lambda BODY
// (-> net_pump::MaybeRequestReAnnounce -- the announce axis' one owner), the
// `!g_fleeing` read (-> !net_pump::IsFleeing()), the TearDown+Flee pair
// (-> net_pump::FleeAfterNativeMenuTravel) + `return` -> `return true`, and
// the file-local alias/static declarations hoisted from net_pump's scope.

#include "coop/props/registry_reaper.h"

#include "coop/dev/perf_probe.h"
#include "coop/element/element.h"
#include "coop/element/element_deleter.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_snapshot.h"
#include "coop/props/save_identity_bind.h"
#include "coop/session/net_pump.h"
#include "coop/session/world_load_episode.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/core/walk_timer.h"

#include <chrono>
#include <cstdint>
#include <vector>

namespace coop::registry_reaper {
namespace {

namespace R  = ue_wrap::reflection;
namespace P  = ue_wrap::profile;
namespace PP = coop::dev::perf_probe;

// True once the local peer has been in the gameplay world during THIS session. Lets the
// per-4s world check tell "left gameplay to the menu" (flee) apart from the transient
// not-yet-in-gameplay window at the start of a join (no flee). Reset by OnSessionStart.
bool g_everInGameplayThisSession = false;

}  // namespace

void OnSessionStart() {
    g_everInGameplayThisSession = false;
}

// Dead-Prop-Element reconciliation (PR-FOUNDATION 2026-05-30). A mass GC
// purge (cave/level transition, save-load) flags ~2000 props PendingKill AT
// ONCE without firing per-actor K2_DestroyActor, so the sole eviction path
// (the PRE observer) is bypassed and the dead Prop Element shadows leak --
// unbounded across transitions, exhausting the 16384 tracker caps after ~7
// and silently breaking prop tracking. Throttle to ~once per 4 s + cap 256
// evicts/call so steady-state cost is one bounded Registry walk every few
// seconds (no-op when nothing is dead) and a post-purge backlog drains over
// a handful of scans. Runs regardless of connection state (props seed at
// boot; a transition can happen before any peer connects) and on BOTH roles
// (each peer maintains its OWN local props). Game-thread (the net_pump Tick)
// -- the reaper Enqueues to the deleter, flushed at the top of the NEXT tick.
bool Tick(coop::net::Session& session) {
    PP::Scope _s{PP::Bucket::Reaper};
    using ReapClock = std::chrono::steady_clock;
    static ReapClock::time_point sNextReap{};
    // The reaper evicts up to this many dead Prop Elements per 4s scan (bounds
    // the per-Flush ~Prop count). SEPARATE from the re-seed episode threshold
    // below.
    constexpr size_t kReapEvictCap = 256;
    // World-change re-seed trigger (snapshot-completeness fix 2026-05-30).
    // The one-shot boot seed runs on the pre-travel world; after VOTV's
    // boot-time `open untitled_1` (and any future cave/level travel) the new
    // level's PLACED props are live-but-untracked (placed props don't fire a
    // catchable Init POST -- the very reason the seed exists). The host then
    // tracks ~70 of ~3300 props -> the late-joiner snapshot ships ~2%.
    //
    // DETECTOR: the reaper ONLY ever finds props flagged PendingKill WITHOUT a
    // K2_DestroyActor (normal destruction goes through that observer -> reaped
    // is 0 in steady gameplay). So a single scan reaping >= kReseedPurge props
    // is a mass purge = a level/world transition. The threshold is well below
    // the eviction cap so it also catches SMALL transitions (e.g. exiting a
    // cave back to the main world purges only the cave's props -- a 256-cap-hit
    // gate would miss it and leave the main world untracked; audit 2026-05-30),
    // while staying above any incidental GC.
    constexpr size_t kReseedPurge = 64;
    // We defer the re-seed to the END of the purge EPISODE -- the scan where the
    // reaper drain CATCHES UP (reaps < kReseedPurge => backlog fully drained).
    // Running it against a fully-drained registry avoids any recycled-address
    // idempotency edge (a new prop reusing a not-yet-drained dead prop's actor
    // address would be skipped by MarkPropElement). The episode flag (not a
    // cooldown) gates re-arming: one purge drains over many 4s scans, all ONE
    // episode; a genuinely NEW transition (later cave entry) starts a fresh
    // episode and re-seeds again. Same GT cost class as the boot seed (both
    // ~thousands of MarkPropElement on this Tick); the reaper leaves the
    // re-seeded props (fresh valid internalIdx -> IsLiveByIndex true). Smoke
    // 2026-05-30: snapshot 70 -> 2314 (the 2314-vs-3328-found gap is empty/None-
    // key props, correctly excluded as non-syncable).
    //
    // Gate hardening 2026-06-10: the flag moved to prop_element_tracker
    // (SetInPurgeEpisode/InPurgeEpisode) so the snapshot coherence gate
    // can read it -- the world-stamp alone proved insufficient (the
    // boot/save-load flow leaves the stamped UWorld alive while the
    // registry is majority-dead; smoke-falsified). This module owns
    // every detection edge below (RULE 2: one flag, one owner of writes).
    const auto reapNow = ReapClock::now();
    // (v106: the v105b forced-reconcile request path is RETIRED -- pickup
    // destroys broadcast at the K2_DestroyActor Func seam, drop/place actors
    // express at the hand edge / FinishSpawningActor Func seam, all
    // event-driven at the moment they happen. This reap + the periodic
    // census below remain the SAFETY NET for mass GC purges and any
    // non-BeginDeferred spawn path -- background cadence, no user-visible
    // latency rides on them anymore.)
    if (reapNow >= sNextReap) {
        sNextReap = reapNow + std::chrono::seconds(4);
        // Gameplay-world gate (2026-06-01, post-flee menu-leak fix). The reaper +
        // world-change re-seed are GAMEPLAY-only. After a gameplay->menu travel (the
        // local-death flee to the main menu) the tracker still holds thousands of
        // now-dead prop-element shadows; running the reaper/re-seed at the MENU reaps
        // them and then re-seeds on the menu's OWN actors in a vicious allocating loop
        // that balloons RAM to OOM ~1 min after the flee (user-observed: menu was fine
        // for a minute, then ballooned -- exactly when the temporary detour bypass
        // expired and this resumed). At a non-gameplay world we skip BOTH: the shadows
        // sit inert and the reaper resumes + cleans them on the next gameplay entry.
        // Gameplay world is untitled_1; the menu is /Game/menu.menu.
        // L5 (FPS): the World instance changes ~once per session (level travel), so DON'T full-walk
        // ~237k GUObjectArray for it every 4s -- cache it + IsLiveByIndex-revalidate, re-resolving (one
        // walk) only on a miss. A travel kills the old world -> serial bump -> IsLiveByIndex false ->
        // re-resolve finds the new /Game/menu world, so the menu-travel RAM-balloon guard below STILL
        // fires; the death-watch scans the Prop Registry (not reapWorld), so it is unaffected. (engine.cpp:60
        // EnsureWorldContext caches identically but returns the immortal GameInstance -- the reaper needs
        // the WORLD instance for its gameplay-vs-menu name check, so it keeps its own cache.)
        static void*   g_reapWorld    = nullptr;
        static int32_t g_reapWorldIdx = -1;
        if (!g_reapWorld || !R::IsLiveByIndex(g_reapWorld, g_reapWorldIdx)) {
            ue_wrap::ScopedWalkTimer _wt("reaper:FindWorld");
            g_reapWorld    = R::FindObjectByClass(P::name::WorldClass);
            g_reapWorldIdx = g_reapWorld ? R::InternalIndexOf(g_reapWorld) : -1;
        }
        void* reapWorld = g_reapWorld;
        // Allocation-free name checks (audit 2026-06-10): the old ToString
        // wstring here was the one survivor of the balloon-fix pattern in
        // this file -- 4s-throttled so never a balloon, but same pattern,
        // same diff. Substring semantics preserved ("ntitled" matches the
        // gameplay world prefix-case-agnostically).
        const bool inGameplayWorld =
            reapWorld && R::NameContains(R::NameOf(reapWorld), L"ntitled");
        // R-2b: publish the verdict for the reseed hub consumer's gate (same source +
        // cadence the retired steady branch read; prop_census must not dereference a
        // world_identity pointer for a name check).
        coop::prop_element_tracker::SetReaperInGameplayWorld(inGameplayWorld);
        // RAM-balloon guard (2026-06-08, user: HOST pressed MAIN MENU mid-session ->
        // ballooning). VOTV's OWN quit-to-menu travels to /Game/menu WITHOUT going
        // through our FleeToMainMenu, so the session stays running + our whole layer
        // keeps churning at the menu = the balloon. Once we've been in gameplay this
        // session, a transition to the MENU world (matched specifically, so a cave /
        // streamed sublevel never trips it) means the local peer LEFT -> flee now (Stop
        // the session + arm the dormancy bypass). The harness running->stopped edge
        // (host) would also catch the resulting stop; the flee latch dedupes. Piggybacks on
        // this existing 4 s world scan (no new per-tick cost) -- 4 s << the ~1 min balloon.
        if (inGameplayWorld) {
            g_everInGameplayThisSession = true;
        } else if (g_everInGameplayThisSession && !coop::net_pump::IsFleeing() && session.running() &&
                   reapWorld && R::NameContains(R::NameOf(reapWorld), L"menu")) {
            UE_LOGW("net: gameplay->MENU while a session is live (VOTV quit-to-menu?) -- "
                    "ending the session + stopping the layer churn (RAM-balloon guard)");
            coop::prop_element_tracker::SetInPurgeEpisode(false);  // left gameplay (matches the !inGameplayWorld reset below)
            // FULL teardown first (2026-07-04 client ESC->menu crash): this path used
            // to skip the disconnect fanout entirely (FleeToMainMenu's edge reset also
            // suppresses the aggregate-disconnect edge), leaving stale weather/time/sky
            // caches + pending applies armed over the teardown -- see
            // TearDownCoopStateForSessionEnd. travel=false: the user's own menu
            // transition is ALREADY in flight; a second dispatch loaded the menu twice.
            coop::net_pump::FleeAfterNativeMenuTravel(session);
            return true;
        }
        if (!inGameplayWorld) coop::prop_element_tracker::SetInPurgeEpisode(false);  // inert at menu; re-arm on gameplay re-entry
        std::vector<coop::element::ElementId> reapedEids;
        const size_t reaped = inGameplayWorld
            ? coop::prop_element_tracker::ReapDeadLocalPropElements(kReapEvictCap, &reapedEids)
            : 0;
        // Reaper escalation (2026-06-27, purge-timing race fix lever (a)). A mass-purge
        // backlog otherwise drains at kReapEvictCap (256) per 4 s throttle -- ~9 scans x 4 s
        // = ~37 s for a ~2300 backlog. That slow drain is the 09:54 ghost root: the
        // episode-end re-seed (below) fires only after the drain catches up, so a 37 s drain
        // lands the re-seed ~37 s into the join -- AFTER the save-identity bind/reconcile has
        // already armed on the pre-purge world, orphaning the save-authoritative natives
        // (tracked-but-unbound = ghosts). The 16:42 run worked only because a fast 4096-cap
        // self-heal drain happened to land the re-seed BEFORE the bind armed. So when THIS
        // scan hit the eviction cap (backlog remains) or a purge episode is still mid-drain,
        // cancel the 4 s throttle: the next tick reaps again immediately and the backlog
        // drains at frame cadence (~256/frame, ~0.15 s total) under the join cover instead of
        // 256/4 s. Per-call cost stays bounded at 256 (no single-frame hitch). Self-
        // terminating: the first scan that reaps < cap with the episode ended restores the
        // throttle. This races the re-seed back AHEAD of the bind (restores 16:42 timing);
        // lever (b) re-binds deterministically if a late GC purge still lands after the arm.
        if (inGameplayWorld &&
            (reaped >= kReapEvictCap || coop::prop_element_tracker::InPurgeEpisode())) {
            sNextReap = reapNow;  // drain again next tick -- no 4 s wait while a backlog remains
        }
        // PART 1 (2026-06-18) HOST-AUTHORITATIVE prop/pile death-watch. A host prop/pile
        // destroyed via a BP-internal EX_CallMath path (garbage-truck collect, ambient cull,
        // removeWOrespawn/LifeSpan despawn, grab-morph) NEVER fires K2_DestroyActor, so the
        // ProcessEvent detour can't see it -- the ONLY thing that ever removed the peer's
        // stale mirror was the retired 4s full re-snapshot's sweep. The reaper above detects
        // exactly these (a dead LOCAL element K2 never drained). In STEADY state (NOT a mass-
        // purge transition -- that is engine teardown the client does on its own travel) the
        // HOST now broadcasts an explicit PropDestroy(eid) per vanish -- MTA Packet_EntityRemove,
        // by IDENTITY (the sound replacement for the retired unsound proximity death-watch).
        // The client resolves the host-range eid -> drops its mirror. Idempotent vs the instant
        // OnPileGrabPre grab-destroy (a 2nd by-eid PropDestroy is a logged no-op on the receiver).
        // ASSUMPTION (audit L3, 2026-06-18): VOTV is ONE persistent untitled_1 world with no
        // in-gameplay sublevel/cave streaming, so the only thing that purges props is a full world
        // swap (reaps ALL ~3300 = a mass purge, excluded by reaped<kReseedPurge). IF a partial
        // LoadStreamLevel cave-travel is ever added (a <64 purge inside a live UWorld with peers in
        // DIFFERENT sublevels), this would PropDestroy the unloaded sublevel's props on a peer still
        // there -- re-gate on a membership/co-location check then.
        if (session.role() == coop::net::Role::Host && reaped > 0 &&
            reaped < kReseedPurge && !coop::prop_element_tracker::InPurgeEpisode()) {
            int sent = 0;
            for (coop::element::ElementId eid : reapedEids) {
                if (eid == coop::element::kInvalidId || eid == 0) continue;
                coop::net::PropDestroyPayload dp{};
                dp.key.len = 0;  // eid-only: the client resolves the mirror by host-range eid
                dp.elementId = static_cast<uint32_t>(eid);
                session.SendPropDestroy(dp);
                ++sent;
            }
            if (sent > 0)
                UE_LOGI("net_pump: host death-watch -- broadcast %d explicit PropDestroy(eid) for "
                        "steady-state prop/pile vanish(es) the un-hookable BP path never replicated "
                        "(MTA per-entity remove, by identity)", sent);
        }
        // Catch up ALREADY-connected peers to the now-complete prop set:
        // their connect-edge snapshot (if it ran at all -- the fork-A
        // coherence gate defers it mid-transition) enumerated the PRE-
        // re-seed registry. Re-trigger the per-slot snapshot;
        // prop_snapshot queues it if a drain is in flight and
        // re-enumerates the (now complete) registry at dequeue, and the
        // client's RegisterPropMirror dedupes props it already holds.
        // No-op when no peer is connected yet. Host-only. (DEFERRED
        // joiners are independent of this added>0 path: the re-seed's
        // generation bump wakes them via prop_snapshot's DrainChunk
        // flush unconditionally.)
        auto retriggerReadySlots = [&session]() {
            if (session.role() != coop::net::Role::Host) return;
            for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
                if (session.IsSlotReady(slot)) {
                    coop::prop_snapshot::TriggerForSlot(slot);
                    UE_LOGI("net_pump: re-snapshot ready slot %d after re-seed", slot);
                }
            }
        };
        // CLIENT re-announce gate (2026-06-16). Re-announce world-ready ONLY when the UWorld
        // actually SWAPPED since our last announce -- NOT for the join's menu-shadow drain within
        // the same world (which spuriously double-snapshotted + duped kerfurs). The announce
        // axis' state + compare live with their ONE owner (net_pump).
        auto maybeReAnnounce = [&session, reapWorld]() {
            coop::net_pump::MaybeRequestReAnnounce(session, reapWorld);
        };
        if (reaped >= kReseedPurge) {
            if (!coop::prop_element_tracker::InPurgeEpisode()) {
                coop::prop_element_tracker::SetInPurgeEpisode(true);
                UE_LOGI("net_pump: mass-purge detected (reaped %zu >= %zu) -- world-change re-seed deferred to drain-complete",
                        reaped, kReseedPurge);
            }
        } else if (coop::prop_element_tracker::InPurgeEpisode()) {
            // Drain caught up: the old level's dead Prop Elements are fully
            // evicted and the new level has loaded. Re-seed now (clean -- no
            // recycled-address collisions) + catch up connected peers.
            coop::prop_element_tracker::SetInPurgeEpisode(false);
            // (a) RE-SEED-ORPHAN FIX (2026-06-28, the 09:54 ghost root). The episode-defer above already
            // waits for the reap backlog to fall below kReseedPurge, BUT the reaper Take()s each dead Prop
            // Element and DEFERS ~Element to ElementDeleter::Flush (worker-thread safety, element_deleter.h).
            // The top-of-Tick Flush ran BEFORE this Tick's reaper, so the natives the reaper just Took THIS
            // Tick are still pending -- their registry actor->eid reverse is STALE until ~Element. A re-seed
            // running now would adjudicate that half-gone state (MarkPropElement's IsBoundMirrorNative /
            // EidForActor guards read a Taken-but-not-yet-destructed Element), orphaning a churned save-native
            // bind into a tracked-but-unbound ghost. Flush the pending deferred-deletes FIRST so the re-seed
            // sees SETTLED state -- force the precondition, don't race it. [[feedback-snapshot-before-state-ready]]
            coop::element::ElementDeleter::Get().Flush();
            // v122: the mass purge that just drained is exactly when element-less keyed
            // index entries (no Registry row -> invisible to the element reaper) died
            // en masse -- drain them here before the walk re-indexes the new world.
            const size_t keyDrained = coop::prop_element_tracker::DrainDeadKeyIndexEntries();
            if (keyDrained > 0)
                UE_LOGI("net_pump: post-purge key-index drain evicted %zu dead element-less keyed entr(ies)", keyDrained);
            const size_t added = coop::prop_element_tracker::ReSeedKnownKeyedProps();
            UE_LOGI("net_pump: world-change re-seed added %zu live keyed prop(s) (snapshot-completeness)", added);
            // (b) RE-BIND ON RE-SEED (2026-06-28). A GC-churned save-native re-creates UNBOUND at its save
            // position (the per-family cursor was already consumed -> it overflow-drops). Re-bind it BY
            // POSITION right HERE -- the purge-surviving stable key is the host-shipped save-time position
            // (1cm exact; the moved-in-window case is handled by b3 PropSnapPos / proxy-wins). Don't wait
            // ~30s for the late quiescence divergence sweep to do it -- that gap IS the 09:54 orphan window.
            // Cheap early-out (returns 0) when nothing churned; only the rare post-purge re-seed pays the
            // GUObjectArray walk -- NOT the 0.25 Hz steady re-seed below (W-2 perf rule).
            if (coop::save_identity_bind::IsEnabled()) {
                const int rebound = coop::save_identity_bind::BindUnboundReCreates();
                if (rebound > 0)
                    UE_LOGI("net_pump: post-purge re-seed re-bound %d unbound save-native(s) (chip by position, "
                            "kerfur by key; 09:54 orphan window closed at the re-seed edge, not the late sweep)", rebound);
            }
            if (added > 0) retriggerReadySlots();
            // CLIENT: re-announce world-ready so the host re-replays its authoritative state into
            // the new world (re-binds our keyless chipPiles to host eids) -- but ONLY if the world
            // actually swapped (maybeReAnnounce). The join's same-world shadow drain must not.
            maybeReAnnounce();
        } else if (inGameplayWorld &&
                   coop::prop_element_tracker::HasSeededOnce() &&
                   !coop::prop_element_tracker::IsRegistrySeededForCurrentWorld()) {
            // Fork A small-travel companion: a travel purging fewer than
            // kReseedPurge keyed elements never starts a purge episode, so
            // the episode-end re-seed -- the only post-travel stamp
            // refresher -- would never run and the snapshot coherence gate
            // would stay closed forever. The reap above (cap 256 > 64)
            // already evicted the sub-64 dead THIS scan, so this re-seed
            // runs against a drained registry (no recycled-address
            // window, the reason the EPISODE path defers to drain-
            // complete). HasSeededOnce keeps the boot window owned by
            // SeedKnownKeyedProps (observers-before-seed doctrine).
            // Ordered as `else if` behind the episode branches: the first
            // scan of a MASS travel takes the episode path and this never
            // preempts the throttled drain.
            const size_t added = coop::prop_element_tracker::ReSeedKnownKeyedProps();
            UE_LOGI("net_pump: world changed without a mass purge -- re-seeded (%zu new keyed)", added);
            if (added > 0) retriggerReadySlots();
            maybeReAnnounce();  // only if the UWorld actually swapped (the owner compares)
        }
        // (R-2b, 2026-08-23: the STEADY-world third branch -- the 0.25 Hz single-frame
        // full census with its NumObjects high-water guard -- is RETIRED whole (RULE 2).
        // Its detector now lives as scan-hub consumer #14 "prop_reseed" (prop_census.cpp
        // InstallReseedScanConsumer/DrainReseedQueue, ticked from subsystems beside the
        // hub): sliced candidate collection + a ~1 ms/tick budget adjudication drain,
        // grew-based SeedGeneration bump parity, ~20 s recycled-slot latency preserved
        // via the hub backstop. Field basis + design:
        // votv-reseed-hub-consumer-DESIGN-2026-08-23.md. The episode-end + small-travel
        // branches above KEEP their synchronous walks -- rare transition events that
        // need a settled result before retriggerReadySlots/maybeReAnnounce.)
    }
    return false;
}

}  // namespace coop::registry_reaper

// coop/props/registry_reaper.cpp -- see coop/props/registry_reaper.h. The 4 s scan that reaps
// dead local Prop elements, detects a mass purge (a world transition) and re-seeds the registry
// at the episode's end, broadcasts the host's steady-state prop deaths, and ends the session
// when the local peer has quit to the menu.

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
#include "ue_wrap/engine/world_identity.h"

#include <chrono>
#include <cstdint>
#include <vector>

namespace coop::registry_reaper {
namespace {

namespace PP = coop::dev::perf_probe;

// True once the local peer has been in the gameplay world this session, so the world check can
// tell a quit to the menu (flee) from the not-yet-in-gameplay window at the start of a join (no
// flee). Reset by OnSessionStart.
bool g_everInGameplayThisSession = false;

}  // namespace

void OnSessionStart() {
    g_everInGameplayThisSession = false;
}

// The dead-element reconciliation. A mass GC purge (a level transition, a save load) flags about
// 2,000 props PendingKill at once without firing K2_DestroyActor, so the eviction observer never
// runs and the dead shadows leak until the 16384 tracker caps exhaust and tracking breaks
// silently. Once per 4 s and capped per call, so the steady cost is one bounded registry walk
// and a backlog drains over a few scans. Regardless of connection state and on both roles (each
// peer maintains its own local props). Game thread; the reaper enqueues to the deleter, flushed
// at the top of the next tick.
bool Tick(coop::net::Session& session) {
    PP::Scope _s{PP::Bucket::Reaper};
    using ReapClock = std::chrono::steady_clock;
    static ReapClock::time_point sNextReap{};
    // The evictions per scan, bounding the per-flush destructor count; separate from the re-seed
    // threshold below.
    constexpr size_t kReapEvictCap = 256;
    // The world-change re-seed trigger. The boot seed runs on the pre-travel world, and after the
    // boot-time level travel (or any later one) the new level's placed props are live but
    // untracked, since a placed prop fires no catchable Init, and the host once tracked a few dozen
    // of thousands. The detector: the reaper only ever finds props purged without K2_DestroyActor
    // (reaped is 0 in steady gameplay), so a scan reaping this many is a mass purge. Well below the
    // eviction cap, so a small transition (leaving a cave purges only the cave's props) is caught
    // too, and above any incidental GC.
    constexpr size_t kReseedPurge = 64;
    // The re-seed waits for the end of the purge episode, the scan where the drain catches up: a
    // re-seed against a fully drained registry has no recycled-address edge (a new prop reusing a
    // not-yet-drained dead prop's address would be skipped as already tracked). The episode flag,
    // not a cooldown, gates re-arming: one purge drains over many scans as one episode, and a new
    // transition starts a fresh one. The flag lives in prop_element_tracker so the snapshot's
    // coherence gate can read it; this module owns every write.
    const auto reapNow = ReapClock::now();
    // This reap and the periodic census are the safety net for a mass purge and any spawn path
    // outside BeginDeferred; every pickup, drop and place expresses at its own seam.
    if (reapNow >= sNextReap) {
        sNextReap = reapNow + std::chrono::seconds(4);
        // The gameplay-world gate: the reap and the re-seed run in gameplay only. After a
        // gameplay-to-menu travel the tracker holds thousands of dead shadows, and reaping them at
        // the menu and re-seeding on the menu's own actors is an allocating loop that ballooned RAM
        // to OOM within a minute; at a non-gameplay world the shadows sit inert and the reaper
        // cleans them on the next gameplay entry. The reader is world_identity's classification,
        // resolved through the immortal GameInstance chain, which the dying world cannot hold
        // alive: a cache revalidated by IsLiveByIndex kept answering with the dying world (not yet
        // flagged) and left the quit-to-menu flee unreachable, the body standing on the host. No
        // walk: a memo refreshed at 10 Hz.
        const auto  kind  = ue_wrap::world_identity::CurrentWorldKind();
        // One world read per scan, at the same instant as the kind, never re-read at the use sites:
        // those sit after synchronous full-array walks (measured over a second on one host), a
        // re-read there lands in a different refresh and can answer null mid-travel, and a null
        // into MaybeRequestReAnnounce reads as a world change and arms a spurious re-announce (a
        // double snapshot and duplicate kerfurs).
        void* const world = ue_wrap::world_identity::CurrentWorld();
        const bool inGameplayWorld = (kind == ue_wrap::world_identity::WorldKind::Gameplay);
        // Published: computed here and never published, the re-seed gate read a false value for the
        // process's life, the scan-hub consumer dropped every candidate it collected on both peers,
        // the host's key index held a few dozen keyed props against thousands, every coin-gun sale
        // of a save-loaded prop was refused with the item already gone, and SeedGeneration (the
        // wake signal deferred joiners wait on) never bumped.
        coop::prop_element_tracker::SetReaperInGameplayWorld(inGameplayWorld);
        // The one case this reader is worse than the walk: an unresolvable world is Unknown, not
        // Gameplay, so five things stop (the purge reap, the world-change re-seed, the steady
        // re-seed and with it SeedGeneration, and the quit-to-menu flee). Treating Unknown as
        // gameplay would resume the reap at the menu; leaving it keeps the session running at the
        // menu, the same balloon by another route, but cannot re-seed on the menu's actors. After a
        // recook the mod needs a port either way; what it must not do is degrade quietly, so a
        // sustained Unknown alarms. Keyed on the symptom, not on Degraded(): a failed class resolve
        // leaves that flag false while the world resolves to null forever. sEverKnown keeps a slow
        // boot from tripping it.
        {
            constexpr int kUnknownScansBeforeAlarm = 8;  // times the 4 s scan, about 32 s
            static int  sUnknownScans = 0;
            static bool sEverKnown = false, sSaid = false;
            if (kind != ue_wrap::world_identity::WorldKind::Unknown) {
                sEverKnown = true;
                sUnknownScans = 0;
            } else if (sEverKnown && !sSaid && ++sUnknownScans >= kUnknownScansBeforeAlarm) {
                sSaid = true;
                UE_LOGE("reaper: the current world has been UNRESOLVABLE for ~%d s "
                        "(world_identity degraded=%d). While it stays that way FIVE things are "
                        "off: the dead-Prop-Element reap, the world-change re-seed, the steady "
                        "re-seed (so SeedGeneration is FROZEN and deferred joiners never wake), "
                        "and the quit-to-menu flee -- so a peer that leaves to the menu keeps its "
                        "session, and prop tracking breaks silently after ~7 world transitions "
                        "(the 16384 caps). This is a version-surface break: see the world_identity "
                        "resolution line and docs/VERSION_MIGRATION.md.",
                        kUnknownScansBeforeAlarm * 4,
                        ue_wrap::world_identity::Degraded() ? 1 : 0);
            }
        }
        // The RAM-balloon guard: VOTV's own quit-to-menu travels to the menu without our flee, so
        // the session stayed running and the whole layer churned at the menu. Once this session has
        // been in gameplay, a transition to a non-gameplay world (matched positively, so a cave or
        // a streamed sublevel never trips it) means the local peer left: the session stops and the
        // dormancy bypass arms. It rides this 4 s scan, well inside the minute the balloon takes.
        if (inGameplayWorld) {
            g_everInGameplayThisSession = true;
        } else if (kind == ue_wrap::world_identity::WorldKind::Other &&
                   g_everInGameplayThisSession && !coop::net_pump::IsFleeing() && session.running()) {
            // Positive Other, never Unknown: a travel publishes null for about a second in both
            // directions, and fleeing on that would end the session of a peer mid-load. Other also
            // covers preLoad and the tutorial maps, which a name test for "menu" did not.
            UE_LOGW("net: gameplay->MENU while a session is live (VOTV quit-to-menu?) -- "
                    "ending the session + stopping the layer churn (RAM-balloon guard)");
            coop::prop_element_tracker::SetInPurgeEpisode(false);  // left gameplay
            // The full teardown first: skipping the disconnect fan-out left stale weather, time and
            // sky caches and pending applies armed over the teardown. travel=false: the user's own
            // menu transition is already in flight, and a second dispatch loaded the menu twice.
            coop::net_pump::FleeAfterNativeMenuTravel(session);
            return true;
        }
        // Positive Other here too: a gate that ends something may not fire on Unknown. Inside an
        // episode the reaper runs at tick rate, and a negation here would clear a live episode
        // within a frame of a travel's Unknown and skip the whole episode-end block.
        if (kind == ue_wrap::world_identity::WorldKind::Other)
            coop::prop_element_tracker::SetInPurgeEpisode(false);  // inert at the menu; re-arms on gameplay re-entry
        std::vector<coop::element::ElementId> reapedEids;
        const size_t reaped = inGameplayWorld
            ? coop::prop_element_tracker::ReapDeadLocalPropElements(kReapEvictCap, &reapedEids)
            : 0;
        // The reaper escalation: a purge backlog drained at the cap per 4 s scan took about 40 s
        // for a join's worth, and the episode-end re-seed then landed after the save-identity bind
        // had armed on the pre-purge world, orphaning the save-authoritative natives as
        // tracked-but-unbound ghosts. When this scan hit the cap, or an episode is mid-drain, the
        // throttle is cancelled and the next tick reaps again, so the backlog drains at frame
        // cadence under the join cover; the per-call cost stays bounded, and the first scan that
        // reaps under the cap with the episode ended restores the throttle.
        if (inGameplayWorld &&
            (reaped >= kReapEvictCap || coop::prop_element_tracker::InPurgeEpisode())) {
            sNextReap = reapNow;  // drain again next tick; no 4 s wait while a backlog remains
        }
        // The host's death-watch: a prop or pile destroyed through a BP-internal path (a truck
        // collect, an ambient cull, a lifespan despawn, a grab morph) never fires K2_DestroyActor,
        // and the reaper is what detects it (a dead local element K2 never drained). In steady
        // state (not a mass purge, which is engine teardown the client does on its own travel) the
        // host broadcasts an explicit PropDestroy per vanish, by identity; the client resolves the
        // host-range eid and drops its mirror, and a second by-eid destroy after a grab-destroy is
        // a logged no-op. This assumes one persistent gameplay world with no in-gameplay sublevel
        // streaming: a partial cave unload under this gate would destroy the unloaded sublevel's
        // props on a peer still in it.
        if (session.role() == coop::net::Role::Host && reaped > 0 &&
            reaped < kReseedPurge && !coop::prop_element_tracker::InPurgeEpisode()) {
            int sent = 0;
            for (coop::element::ElementId eid : reapedEids) {
                if (eid == coop::element::kInvalidId || eid == 0) continue;
                coop::net::PropDestroyPayload dp{};
                dp.key.len = 0;  // eid only: the client resolves the mirror by host-range eid
                dp.elementId = static_cast<uint32_t>(eid);
                session.SendPropDestroy(dp);
                ++sent;
            }
            if (sent > 0)
                UE_LOGI("net_pump: host death-watch -- broadcast %d explicit PropDestroy(eid) for "
                        "steady-state prop/pile vanish(es) the un-hookable BP path never replicated "
                        "(MTA per-entity remove, by identity)", sent);
        }
        // Already-connected peers catch up to the now-complete prop set: their connect snapshot
        // enumerated the pre-re-seed registry (if the coherence gate let it run at all), so the
        // per-slot snapshot re-triggers, re-enumerates at dequeue, and the client dedupes what it
        // holds. Host only. Deferred joiners are independent: the re-seed's generation bump wakes
        // them.
        auto retriggerReadySlots = [&session]() {
            if (session.role() != coop::net::Role::Host) return;
            for (int slot = 1; slot < coop::players::kMaxPeers; ++slot) {
                if (session.IsSlotReady(slot)) {
                    coop::prop_snapshot::TriggerForSlot(slot);
                    UE_LOGI("net_pump: re-snapshot ready slot %d after re-seed", slot);
                }
            }
        };
        // The client's re-announce, only when the UWorld actually swapped since the last announce,
        // not for the join's shadow drain within the same world (which double-snapshotted and
        // duplicated kerfurs); net_pump owns the compare.
        auto maybeReAnnounce = [&session, world]() {
            coop::net_pump::MaybeRequestReAnnounce(session, world);
        };
        if (reaped >= kReseedPurge) {
            if (!coop::prop_element_tracker::InPurgeEpisode()) {
                coop::prop_element_tracker::SetInPurgeEpisode(true);
                UE_LOGI("net_pump: mass-purge detected (reaped %zu >= %zu) -- world-change re-seed deferred to drain-complete",
                        reaped, kReseedPurge);
            }
        } else if (inGameplayWorld && coop::prop_element_tracker::InPurgeEpisode()) {
            // inGameplayWorld is explicit: an Unknown scan reaches this branch with reaped 0 (the
            // reap is gameplay-gated) and would otherwise run the episode-end work, the re-announce
            // included, mid-travel. The drain has caught up: the old level's dead elements are
            // evicted and the new level loaded, so the re-seed runs clean and the connected peers
            // catch up.
            coop::prop_element_tracker::SetInPurgeEpisode(false);
            // The deleter flushes first: the reaper Takes each dead element and defers its
            // destructor to the next tick's flush, so the natives taken this tick still hold a
            // stale actor-to-eid reverse, and a re-seed against that half-gone state orphaned a
            // churned save-native bind into a tracked-but-unbound ghost. Force the settled state;
            // do not race it.
            coop::element::ElementDeleter::Get().Flush();
            // The element-less keyed index entries (no registry row, so invisible to the element
            // reaper) died en masse in the purge; drained before the walk re-indexes the new world.
            const size_t keyDrained = coop::prop_element_tracker::DrainDeadKeyIndexEntries();
            if (keyDrained > 0)
                UE_LOGI("net_pump: post-purge key-index drain evicted %zu dead element-less keyed entr(ies)", keyDrained);
            const size_t added = coop::prop_element_tracker::ReSeedKnownKeyedProps();
            UE_LOGI("net_pump: world-change re-seed added %zu live keyed prop(s) (snapshot-completeness)", added);
            // A GC-churned save native re-creates unbound at its save position (its family cursor
            // was already consumed), so it is re-bound by position right here, on the
            // purge-surviving key the host shipped, rather than 30 s later at the divergence sweep,
            // the orphan window. A cheap early-out when nothing churned; only the post-purge
            // re-seed pays the walk.
            if (coop::save_identity_bind::IsEnabled()) {
                const int rebound = coop::save_identity_bind::BindUnboundReCreates();
                if (rebound > 0)
                    UE_LOGI("net_pump: post-purge re-seed re-bound %d unbound save-native(s) (chip by position, "
                            "kerfur by key; 09:54 orphan window closed at the re-seed edge, not the late sweep)", rebound);
            }
            if (added > 0) retriggerReadySlots();
            // The client re-announces world-ready, so the host re-replays its state into the new
            // world (re-binding the keyless piles to host eids), only if the world swapped.
            maybeReAnnounce();
        } else if (inGameplayWorld &&
                   coop::prop_element_tracker::HasSeededOnce() &&
                   !coop::prop_element_tracker::IsRegistrySeededForCurrentWorld()) {
            // The small-travel companion: a travel purging fewer keyed elements than the threshold
            // starts no episode, and the episode-end re-seed is the only post-travel stamp
            // refresher, so the snapshot coherence gate would stay closed forever. The reap above
            // already evicted the few dead this scan, so the re-seed runs against a drained
            // registry. HasSeededOnce leaves the boot window to the boot seed; behind the episode
            // branches, so a mass travel's first scan takes the episode path.
            const size_t added = coop::prop_element_tracker::ReSeedKnownKeyedProps();
            UE_LOGI("net_pump: world changed without a mass purge -- re-seeded (%zu new keyed)", added);
            if (added > 0) retriggerReadySlots();
            maybeReAnnounce();  // only if the UWorld actually swapped; the owner compares
        }
        // The steady-world census is the scan-hub consumer in prop_census.cpp (a sliced candidate
        // collection and a budgeted drain); the two transition branches above keep their
        // synchronous walks, since a rare transition needs a settled result before the re-trigger
        // and the re-announce.
    }
    return false;
}

}  // namespace coop::registry_reaper

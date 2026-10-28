// coop/dev/octa_drill_host.cpp -- the host's half of the Octa drill: the checks before an arm, the walker's place, the
// Octa spawned beside it, the host's own legs for the control, and the watch that judges the Octa's liveness. See
// coop/dev/octa_drill.h and coop/dev/octa_drill_internal.h.

#include "coop/dev/octa_drill.h"

#include "coop/creatures/geom_octa_sync.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "octa_drill_internal.h"  // co-located private header
#include "coop/dev/octa_probe.h"
#include "coop/dev/spawn_npc.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace coop::dev::octa_drill::detail {

namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace GS = coop::geom_octa_sync;

namespace {

// The live Octas of the world, to tell the one a spawn made from the rest.
struct OctaSet {
    void*  octas[16] = {};
    size_t count = 0;
};

void CollectOctas(OctaSet& out) {
    out.count = 0;
    void* cls = ue_wrap::object_index::ClassByName(kOctaClass);
    if (!cls) return;
    ue_wrap::object_index::ForEachInstance(cls,
        [](void* ctx, void* obj, int32_t) {
            auto* set = static_cast<OctaSet*>(ctx);
            if (set->count < 16 && !R::NameStartsWith(R::NameOf(obj), L"Default__")) set->octas[set->count++] = obj;
        },
        &out);
}

// The Octa a spawn made: the live one that was not among `before`; with `fallback`, only once the wait for one is over,
// else the one nearest `at` within 200 cm; null when
// there is neither.
void* NewOctaSince(const OctaSet& before, const FVector& at, bool fallback) {
    OctaSet now;
    CollectOctas(now);
    void* nearest = nullptr;
    float nearestD = 200.f;
    for (size_t i = 0; i < now.count; ++i) {
        bool known = false;
        for (size_t j = 0; j < before.count; ++j)
            if (before.octas[j] == now.octas[i]) known = true;
        if (!known) return now.octas[i];
        FVector p{};
        if (fallback && E::TryGetActorLocation(now.octas[i], p) && FlatDist(p, at) <= nearestD) {
            nearest = now.octas[i];
            nearestD = FlatDist(p, at);
        }
    }
    return nearest;
}

void HostWait(coop::net::Session* s, void* me) {
    if (!s->AnyWorldReadyPeer()) return;
    float worldNow = 0.f;
    if (!E::TryGetWorldTimeSeconds(worldNow)) return;
    if (Fires() && !DamageProbeOn()) {
        Fail("damage_probe=1 is required: each blast must still log its [DMG] line");
        return;
    }
    if (ArmOf() == Arm::Attack && HostWalks()) {
        Fail("the attack arm judges the client's relay: octa_measure_target=host belongs to measure");
        return;
    }
    if (!Guard()) {
        Fail("startInvinc did not resolve on the host's player, so a blast would not leave it alive");
        return;
    }
    g_stepMs = coop::net::NowMs();
    if (Fires()) {
        g_hstep = HStep::Place;   // the walker takes its place first; the Octa is spawned beside it
        return;
    }
    HostBirths(me);
}

constexpr uint64_t kSpawnWaitMs = 5000;   // the gamemode route's new Octa must be live within this of the call

// The gamemode route's spawn while it is not yet live: the instances before the call, where it was placed, and the
// distances the READY line will say.
struct Pending {
    OctaSet  before;
    FVector  at{};
    float    walkerD = 0.f;
    float    otherD = 0.f;
    uint64_t since = 0;
};
Pending g_pending;

void OctaSpawned(void* octa, const FVector& at, float walkerD, float otherD);

// The places of the players other than the walker: the host, unless it is the walker, and every live puppet but the
// walker's. The Octa must stand out of all their reach.
struct Others {
    FVector at[coop::players::kMaxPeers];
    int     count = 0;
};

Others OthersOf(void* me, bool hostWalks, uint8_t walkerSlot) {
    Others o;
    FVector p{};
    if (!hostWalks && E::TryGetActorLocation(me, p)) o.at[o.count++] = p;
    for (uint8_t s = 0; s < coop::players::kMaxPeers && o.count < coop::players::kMaxPeers; ++s) {
        if (!hostWalks && s == walkerSlot) continue;
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(s);
        void* body = rp ? rp->GetActor() : nullptr;
        if (body && R::IsLive(body) && E::TryGetActorLocation(body, p)) o.at[o.count++] = p;
    }
    return o;
}

// measure: spawns the Octa about 600 cm from the walker, on the side away from the other players (a reachable point of a
// ring about the walker, the one farthest from the nearest of them, at least 1300 cm from each). The Octa's sphere
// registers inside its own finish-spawn and reports the walker's capsule as an overlap there, which is how it aggros,
// with the others out of the sphere's reach.
void SpawnOctaBeside(void* me, const FVector& walker, const Others& others) {
    const std::vector<FVector> ring = D::ReachableStandpoints(me, walker, kSpawnNearCm, kStandpoints, kStandReachCm);
    const FVector* best = nullptr;
    float bestD = 0.f;
    for (const FVector& p : ring) {
        float d = 1e9f;   // the distance to the nearest other player
        for (int i = 0; i < others.count; ++i) d = (std::min)(d, FlatDist(p, others.at[i]));
        if (d >= kSpawnAwayCm && (!best || d > bestD)) {
            best = &p;
            bestD = d;
        }
    }
    if (!best) {
        Fail("no reachable point 600 cm from the walker lies 1300 cm or more from the other player");
        return;
    }
    // The baseline is read before the spawn: the sphere reports the walker standing inside it as it registers, inside
    // the spawn's finish, so the first aggro lands before SpawnAt returns.
    g_base = GS::GetCounters();
    // The dev tool's NPC spawn is enrolled at its interceptor, which also sends the spawn; the gamemode's spawn verb is
    // caught at the EX catch and enrolled, and sent, at the drain a tick later: the Octa's first aggro, inside the
    // spawn's finish, then precedes both, and the lane keeps it until the spawn has been sent.
    void* octa = nullptr;
    if (SpawnByGamemode()) {
        // The gamemode's verb spawns through a deferred spawn the engine finishes after the call returns, so the new
        // Octa is not yet among the live instances: the diff is retried each tick (HostAwaitOcta), up to kSpawnWaitMs.
        g_pending = Pending{};
        CollectOctas(g_pending.before);
        if (!CallGamemodeSpawn(*best)) {
            Fail("the Octa did not spawn");
            return;
        }
        g_pending.at = *best;
        g_pending.walkerD = FlatDist(*best, walker);
        g_pending.otherD = bestD;
        octa = NewOctaSince(g_pending.before, *best, /*fallback=*/false);
        if (!octa) {
            g_pending.since = coop::net::NowMs();
            g_hstep = HStep::Spawning;
            return;
        }
    } else {
        octa = coop::dev::spawn_npc::SpawnAt(kOctaClass, *best);
    }
    if (!octa) {
        Fail("the Octa did not spawn");
        return;
    }
    OctaSpawned(octa, *best, FlatDist(*best, walker), bestD);
}

// The Octa is live: the run's state for it, and the line that says where it stands.
void OctaSpawned(void* octa, const FVector& at, float walkerD, float otherD) {
    g_octa.Set(octa);
    GS::OpenVerdictWindow(kBlastsWanted, octa);
    g_volleysSeen = 0;
    g_wstep = WStep::Stand;
    g_spawnMs = g_stepMs = coop::net::NowMs();
    g_hstep = HStep::Watch;
    const uint32_t eid = NpcEidOf(octa);
    g_eidSaid = eid != 0;   // the gamemode's Octa is enrolled at the drain, a tick on: its id is said then
    char eidText[24];
    if (eid) std::snprintf(eidText, sizeof(eidText), "%u", eid);
    else std::snprintf(eidText, sizeof(eidText), "pending");
    UE_LOGI("[OCTA-DRILL] host READY %s: Octa (%s route) eid=%s at=%.0f,%.0f,%.0f, %.0f cm from the walker (slot %u), "
            "%.0f from the nearest other player", FiringArmName(), SpawnByGamemode() ? "gamemode" : "sat", eidText, at.X, at.Y,
            at.Z, walkerD, static_cast<unsigned>(g_walkerSlot), otherD);
}

// The gamemode route, one tick on: the new Octa, or the wait's end.
void HostAwaitOcta() {
    // The nearest-Octa fallback is tried once, when the wait is over, never on a retry tick: an Octa that is not new but
    // stands at the placement would otherwise be taken for the spawn's.
    const bool over = coop::net::NowMs() - g_pending.since > kSpawnWaitMs;
    void* octa = NewOctaSince(g_pending.before, g_pending.at, over);
    if (octa) {
        OctaSpawned(octa, g_pending.at, g_pending.walkerD, g_pending.otherD);
        return;
    }
    if (over) Fail("the Octa did not spawn");
}

// measure: the walker takes its place. The client's: its director walks it (the client's own half) and the host spawns
// the Octa once the puppet has stood still for two seconds, 1300 cm or more from the host. The host's: its own director
// walks it 2200 cm from the client's puppet, and the Octa is spawned beside it.
void HostPlace(void* me) {
    uint8_t slot = 0;
    void* puppet = LivePuppetBody(slot);
    FVector hp{}, pp{};
    if (!E::TryGetActorLocation(me, hp)) return;
    if (!puppet || !E::TryGetActorLocation(puppet, pp)) {
        if (Expired(kBirthsBoundMs)) Fail("the client's puppet never resolved on the host");
        return;
    }
    if (HostWalks()) {
        if (PlaceWalkTick(me, pp, "host") != PlaceStep::Reached) return;
        g_walkerSlot = 0;
        UE_LOGI("[OCTA-DRILL] host in place t=%llu", Now());
        if (ArmOf() == Arm::Join) {   // the Octa is spawned on the joiner's schedule (HostTiming), not now
            g_hstep = HStep::Timing;
            g_stepMs = coop::net::NowMs();
            return;
        }
        if (!E::TryGetActorLocation(me, hp)) return;
        SpawnOctaBeside(me, hp, OthersOf(me, true, 0));
        return;
    }
    // The client is in place when its puppet is far from the host and has not moved for two seconds.
    const uint64_t now = coop::net::NowMs();
    if (!g_puppetStillAt || FlatDist(pp, g_puppetLast) >= kStillCm) {
        g_puppetLast = pp;
        g_puppetStillAt = now;
        return;
    }
    if (now - g_puppetStillAt < kStillMs || FlatDist(pp, hp) < kSpawnAwayCm) return;
    g_walkerSlot = slot;
    SpawnOctaBeside(me, pp, OthersOf(me, false, slot));
}

// octa_measure_target=host: the host's own player, in place beside the Octa it spawned, stands until the Octa has fired
// three volleys and walks out, from the Octa's live place. A volley is one fire, which is one host verdict: a laser
// makes several explosions, so the host counts its verdicts, not its explosions. The bound is the client's.
void HostWalkTick(void* octa, void* me) {
    switch (g_wstep) {
    case WStep::Stand: {
        if (ArmOf() == Arm::Join) {
            // The join arm's host stands where it is until the joiner's chat line, and the arm ends there: the proof
            // is the joiner's, so the host needs no walk-out (its place can have no NavMesh route out of the sphere).
            if (!g_joinHoldAt) g_joinHoldAt = coop::net::NowMs();
            if (!JoinerDone()) {
                if (coop::net::NowMs() - g_joinHoldAt > kJoinWaitMs) Fail("the joiner never said it was done (360 s)");
                return;
            }
            const GS::Counters c = GS::GetCounters();
            UE_LOGI("[OCTA-DRILL] join DONE aggros=%u verdicts=%u idles=%u lasers=%u t=%llu",
                    c.hostAggro - g_base.hostAggro, c.hostVerdict - g_base.hostVerdict, c.idle - g_base.idle,
                    c.laser - g_base.laser, Now());
            g_wstep = WStep::Finished;
            g_hstep = HStep::Done;
            return;
        }
        const uint32_t now = GS::GetCounters().hostVerdict - g_base.hostVerdict;
        if (now > g_volleysSeen) {
            g_volleysSeen = now;
            g_stepMs = coop::net::NowMs();
            UE_LOGI("[OCTA-DRILL] host: volley %u of %u fired", now, kBlastsWanted);
        }
        if (g_volleysSeen >= kBlastsWanted) {
            if (BeginWalkOut(me, octa, "host")) g_wstep = WStep::Out;
            return;
        }
        if (Expired(kFireBoundMs)) Fail("no host fire (the host's Octa fired no volley for 90 s)");
        return;
    }
    case WStep::Out: {
        const int state = g_walk ? g_walk->state.load() : 2;
        if (state == 0) return;
        if (state == 2) {   // the next ring
            ++g_outRing;
            BeginWalkOut(me, octa, "host");
            return;
        }
        UE_LOGI("[OCTA-DRILL] host out t=%llu", Now());
        g_wstep = WStep::Finished;
        return;
    }
    default:
        return;
    }
}

void HostWatchMeasure() {
    void* octa = g_octa.Get();
    if (!octa) {
        Fail("the Octa is gone before the arm ended");
        return;
    }
    Guard();
    void* me = coop::players::Registry::Get().Local();
    const GS::Counters c = GS::GetCounters();
    FVector op{};
    FVector pp{};
    uint8_t puppetSlot = 0;
    void* puppet = LivePuppetBody(puppetSlot);
    // The body that stands in the sphere: the client's puppet, or the host's own player for the control.
    void* walker = HostWalks() ? me : puppet;
    const bool haveBoth = E::TryGetActorLocation(octa, op) && walker && E::TryGetActorLocation(walker, pp);
    const float dist = haveBoth ? FlatDist(op, pp) : -1.f;
    const uint64_t now = coop::net::NowMs();
    coop::dev::octa_probe::Tick(octa, me, puppet);
    if (HostWalks()) {
        HostWalkTick(octa, me);
        if (g_hstep == HStep::Done) return;
    }
    if (!g_eidSaid) {
        if (const uint32_t eid = NpcEidOf(octa)) {
            g_eidSaid = true;
            UE_LOGI("[OCTA-DRILL] host: the Octa is enrolled, eid=%u", eid);
        }
    }
    if (!g_aggroSaid && c.hostAggro > g_base.hostAggro) {
        g_aggroSaid = true;
        char want[16];
        std::snprintf(want, sizeof(want), "slot:%u", static_cast<unsigned>(g_walkerSlot));
        UE_LOGI("[OCTA-DRILL] host: the Octa aggro'd target=%s (the walker is %s) %llu ms after the spawn",
                GS::LastAggroTarget(), want, static_cast<unsigned long long>(now - g_spawnMs));
        if (std::strcmp(GS::LastAggroTarget(), want) != 0) {
            Fail("aggro'd the wrong player");
            return;
        }
    }
    if (!g_aggroSaid && now - g_spawnMs > kAggroBoundMs) {
        Fail("no host aggro (30 s from the spawn)");
        return;
    }
    // An idle after the third verdict, with the puppet out of the sphere. The lane keeps the idle count at the moment
    // it counted the third verdict, so an idle between that and this poll is not absorbed. The join arm ends in
    // HostWalkTick, on the joiner's word.
    const bool idled = ArmOf() != Arm::Join && c.windowReached && c.idle > c.idleAtWindow;
    if (idled && haveBoth && dist > kSphereCm) {
        if (ArmOf() == Arm::Attack)
            UE_LOGI("[OCTA-DRILL] attack DONE aggros=%u verdicts=%u idles=%u lasers=%u t=%llu",
                    c.hostAggro - g_base.hostAggro, c.hostVerdict - g_base.hostVerdict, c.idle - g_base.idle,
                    c.laser - g_base.laser, Now());
        else
            UE_LOGI("[OCTA-DRILL] measure DONE aggros=%u verdicts=%u idles=%u lasers=%u t=%llu",
                    c.hostAggro - g_base.hostAggro, c.hostVerdict - g_base.hostVerdict, c.idle - g_base.idle,
                    c.laser - g_base.laser, Now());
        g_hstep = HStep::Done;
    }
}

// The join arm's timing, read from the session's own edges. The witness (slot 1) is admitted and world-ready first: its
// load time L is the span between the two. The observer (slot 2, the joiner) is admitted later; the Octa is spawned at
// its admit + max(0, L - kJoinLeadMs), so the Octa aggros the host about kJoinLeadMs before the observer's world-ready,
// and the observer's snapshot finds the attack in its pre-fire window.
uint64_t g_admitAt[3] = {};   // the net clock at each slot's admit and world-ready edge on the host (0: not yet)
uint64_t g_readyAt[3] = {};
uint64_t g_spawnAt = 0;       // when the Octa is to be spawned (0: not yet decided)

void NoteJoinEdges(coop::net::Session* s) {
    const uint64_t now = coop::net::NowMs();
    for (int slot = 1; slot <= 2; ++slot) {
        if (!g_admitAt[slot] && s->HasPeerConn(slot)) g_admitAt[slot] = now;
        if (g_readyAt[slot] || !s->IsSlotWorldReady(slot)) continue;
        g_readyAt[slot] = now;
        if (slot == 2 && !g_octa.Get()) {
            UE_LOGI("[OCTA-DRILL] join snapshot: no host Octa");
            Fail("INCONCLUSIVE -- the observer was world-ready before the Octa stood");
        }
    }
}

void HostTiming(void* me) {
    Guard();
    if (!g_spawnAt && g_readyAt[1] && g_admitAt[1] && g_admitAt[2]) {
        const uint64_t loadMs = g_readyAt[1] - g_admitAt[1];
        const uint64_t waitMs = loadMs > kJoinLeadMs ? loadMs - kJoinLeadMs : 0;
        g_spawnAt = g_admitAt[2] + waitMs;
        UE_LOGI("[OCTA-DRILL] join timing L=%llu D=%llu", static_cast<unsigned long long>(loadMs),
                static_cast<unsigned long long>(waitMs));
    }
    if (!g_spawnAt || coop::net::NowMs() < g_spawnAt) {
        if (Expired(kJoinWaitMs)) Fail("the observer was never admitted (360 s)");
        return;
    }
    FVector hp{};
    if (!E::TryGetActorLocation(me, hp)) return;
    SpawnOctaBeside(me, hp, OthersOf(me, true, 0));
}

// The join arm's measurement validity: the observer's snapshot must have found the Octa mid-attack, or the Aggro path it
// proves was not exercised. Judged once, on the first snapshot after the Octa stood. A snapshot the drill holds no
// Octa for, or one between attacks, ends the arm INCONCLUSIVE: never silence.
void JudgeJoinSnapshot() {
    if (g_snapJudged) return;
    const GS::Counters c = GS::GetCounters();
    const bool mid = c.joinMid > g_base.joinMid;
    const bool between = c.joinBetween > g_base.joinBetween;
    if (!mid && !between) return;
    g_snapJudged = true;
    if (!g_octa.Get()) {
        UE_LOGI("[OCTA-DRILL] join snapshot: no host Octa");
        Fail("INCONCLUSIVE -- the joiner's snapshot ran and the host holds no Octa");
    } else if (mid) {
        UE_LOGI("[OCTA-DRILL] join snapshot: mid-attack");
    } else {
        UE_LOGI("[OCTA-DRILL] join snapshot: between attacks");
        Fail("INCONCLUSIVE -- the joiner's snapshot found the Octa between attacks");
    }
}

}  // namespace

void ResetJoinHost() {
    for (uint64_t& t : g_admitAt) t = 0;
    for (uint64_t& t : g_readyAt) t = 0;
    g_spawnAt = 0;
}

void HostTick(coop::net::Session* s) {
    if (!s->running()) return;
    if (ArmOf() == Arm::Join) {
        NoteJoinEdges(s);
        JudgeJoinSnapshot();
    }
    if (g_hstep == HStep::Done) return;
    void* me = coop::players::Registry::Get().Local();
    if (!me || !Due()) return;
    if (g_hstep == HStep::Wait) {
        HostWait(s, me);
        return;
    }
    if (g_hstep == HStep::Place) HostPlace(me);
    else if (g_hstep == HStep::Timing) HostTiming(me);
    else if (g_hstep == HStep::Spawning) HostAwaitOcta();
    else if (ArmOf() == Arm::Births) HostWatchBirths();
    else HostWatchMeasure();
    if (g_hstep == HStep::Done) Unguard();
}

}  // namespace coop::dev::octa_drill::detail

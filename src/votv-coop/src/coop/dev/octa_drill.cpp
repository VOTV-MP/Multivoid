// coop/dev/octa_drill.cpp -- see coop/dev/octa_drill.h.

#include "coop/dev/octa_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/creature_hit.h"
#include "coop/creatures/geom_octa_relay.h"
#include "coop/creatures/geom_octa_sync.h"
#include "coop/dev/director/director.h"
#include "coop/dev/director/routes.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_hold.h"
#include "octa_drill_internal.h"  // co-located private header
#include "coop/dev/octa_probe.h"
#include "coop/dev/spawn_npc.h"
#include "coop/element/registry.h"
#include "coop/net/net_clock.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/world/explosion_sync.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/fname_utils.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/sdk_profile.h"  // NpcClass_GeomOcta
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/world_singleton.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::octa_drill {
namespace detail {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace DH = coop::dev::drill_hold;
namespace GS = coop::geom_octa_sync;

Arm ArmOf() {
    static const Arm arm = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::octa_drill);
        return v == "births" ? Arm::Births : v == "measure" ? Arm::Measure : v == "attack" ? Arm::Attack
             : v == "join" ? Arm::Join : Arm::Off;
    }();
    return arm;
}
// The arms that place a walker and let the Octa fire: measure (no relay judged), attack and join (the relay judged).
bool Fires() { return ArmOf() == Arm::Measure || RelayJudged(); }
bool RelayJudged() { return ArmOf() == Arm::Attack || ArmOf() == Arm::Join; }
bool IsJoiner() { return ArmOf() == Arm::Join && coop::players::Registry::Get().LocalPeerId() == 2; }
const char* FiringArmName() { return ArmOf() == Arm::Join ? "join" : ArmOf() == Arm::Attack ? "attack" : "measure"; }

// Where the measure and attack arms spawn the Octa (octa_spawn_route): the NPC spawn of the dev tool, or the gamemode's
// own spawn verb with the spawn menu's Octa row, whose Octa the host's EX catch enrols at the drain.
bool SpawnByGamemode() {
    static const bool gm = coop::config::ResolveEnum(::coop::config_registry::rows::octa_spawn_route) == "gamemode";
    return gm;
}

// Who walks into the Octa's sphere: the host's own player in the join arm (client 1 is the joiner and stands away), else
// the control row octa_measure_target says, the client by default.
bool HostWalks() {
    static const bool host =
        ArmOf() == Arm::Join ||
        coop::config::ResolveEnum(::coop::config_registry::rows::octa_measure_target) == "host";
    return host;
}

HStep g_hstep = HStep::Wait;
CStep g_cstep = CStep::Ready;

uint64_t g_nextMs = 0;
uint64_t g_stepMs = 0;            // the step began; a FAIL bound counts from here
int      g_invinc = -1;           // the value startInvinc is put back to (-1: not held)
ue_wrap::CachedObjRef g_octa;     // HOST: the Octa it spawned or the gamemode's spawn made
std::shared_ptr<D::BackgroundWalk> g_walk;

// HOST (measure)
GS::Counters g_base{};            // the lane's counters when the Octa stood
uint64_t g_spawnMs = 0;           // the Octa was spawned; the aggro bound counts from here
bool     g_aggroSaid = false;
bool     g_eidSaid = false;       // the Octa's NPC element id has been said (the gamemode route enrols it a tick late)
uint8_t  g_walkerSlot = 0;        // the slot the Octa must aggro: the host's own (0), or the client's puppet's
FVector  g_puppetLast{};          // the client puppet's place at the last poll, and since when it has not moved
uint64_t g_puppetStillAt = 0;
WStep    g_wstep = WStep::Stand;  // octa_measure_target=host: the host's legs after the spawn
uint32_t g_volleysSeen = 0;       // the host's own verdicts (one per fire) since the Octa was spawned, as last polled
bool     g_snapJudged = false;    // join, host: the joiner's snapshot has been judged
// CLIENT (measure)
uint32_t g_appliedBase = 0;      // the explosion lane's applied count when the Octa mirror appeared
uint32_t g_appliedSeen = 0;
uint32_t g_volleys = 0;           // the volleys applied since then
coop::geom_octa_relay::Counters g_relay0{};   // attack: the relay's counters when the mirror appeared
uint32_t g_verdictsSeen = 0;      // attack: the verdicts it has applied since
uint32_t g_laserSeen = 0;         // attack: this client's own lasers counted so far
uint64_t g_laserPendingMs = 0;    // attack: the stamp of a laser still waiting for the blast that follows it
uint64_t g_joinHoldAt = 0;
bool     g_joinerSeen = false;
size_t   g_outRing = 0;           // the walk-out ring in use (kOutRingsCm)
uint64_t g_lastAppliedMs = 0;     // when the last explosion was seen applied
bool     g_calledEvent = false;   // CLIENT (births): the night roll's event has been called

bool Due() {
    const uint64_t now = coop::net::NowMs();
    if (now < g_nextMs) return false;
    g_nextMs = now + kEveryMs;
    return true;
}
bool Expired(uint64_t bound) { return coop::net::NowMs() - g_stepMs > bound; }
unsigned long long Now() { return static_cast<unsigned long long>(coop::net::NowMs()); }

// ---- the victim guard ----

// startInvinc held true on this peer's own player, asserted again each tick: its one writer is a Delay(5) after
// begin-play that clears it, and a blast must find it set. False when the player or the member does not resolve.
bool Guard() {
    return DH::Hold(coop::players::Registry::Get().Local(), kGuardMember, g_invinc, 0, kTag);
}
void Unguard() { DH::Put(coop::players::Registry::Get().Local(), kGuardMember, g_invinc, kTag); }

// The arm ends on a FAIL line; the guard goes back with it.
void Fail(const char* what) {
    UE_LOGW("[OCTA-DRILL] FAIL: %s", what);
    g_hstep = HStep::Done;
    g_cstep = CStep::Done;
    Unguard();
}

void Go(CStep s) { g_cstep = s; g_stepMs = coop::net::NowMs(); }

bool DamageProbeOn() {
    static const bool on = coop::config::ResolveFlag(::coop::config_registry::rows::damage_probe);
    return on;
}

// ---- finding things ----

struct Found { void* obj = nullptr; };

// The first live instance of the class, the default object left out; null when none.
void* FirstInstance(const wchar_t* className) {
    void* cls = ue_wrap::object_index::ClassByName(className);
    if (!cls) return nullptr;
    Found found;
    ue_wrap::object_index::ForEachInstance(cls,
        [](void* ctx, void* obj, int32_t) {
            auto* out = static_cast<Found*>(ctx);
            if (!out->obj && !R::NameStartsWith(R::NameOf(obj), L"Default__")) out->obj = obj;
        },
        &found);
    return found.obj;
}

// The Octa element's id of `actor` (the host's own or a client's mirror), 0 when the registry names none.
uint32_t NpcEidOf(void* actor) {
    if (!actor || !coop::creature_hit::IsCreature(actor)) return 0;
    return static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(actor));
}

float FlatDist(const FVector& a, const FVector& b) { return std::hypot(a.X - b.X, a.Y - b.Y); }

// The first puppet's body that is live, with its slot; null when there is none.
void* LivePuppetBody(uint8_t& slot) {
    for (uint8_t s = 0; s < coop::players::kMaxPeers; ++s) {
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(s);
        void* body = rp ? rp->GetActor() : nullptr;
        if (body && R::IsLive(body)) {
            slot = s;
            return body;
        }
    }
    return nullptr;
}

// The walker's way out of the sphere: a standpoint on a ring about the Octa's live place, the next ring when the last
// has none or the walk to it ended with no route (an earlier host walk-out failed once on a place with no route 1800 cm
// out). False, said, once the last ring has failed.
bool BeginWalkOut(void* me, void* octa, const char* who) {
    FVector at{};
    if (!octa || !E::TryGetActorLocation(octa, at)) {
        Fail("the Octa is gone before the walk-out");
        return false;
    }
    if (g_walk && g_walk->state.load() == 0) D::CancelBackgroundWalk(*g_walk);   // a re-entry still walking ends here
    for (; g_outRing < kOutRingCount; ++g_outRing) {
        const std::vector<FVector> ring = D::ReachableStandpoints(me, at, kOutRingsCm[g_outRing], kStandpoints, kStandReachCm);
        if (ring.empty()) continue;
        g_walk = D::StartBackgroundWalk(ring[0], kStandReachCm, kOutWalkDeadlineS);
        UE_LOGI("[OCTA-DRILL] %s: walking out to a standpoint %.0f cm from the Octa", who, kOutRingsCm[g_outRing]);
        return true;
    }
    Fail("no NavMesh route from the Octa reaches a standpoint 1800, 2400 or 3000 cm out: the walker cannot leave its sphere");
    return false;
}

namespace {
std::vector<FVector> g_placeRing;   // the standpoints of the walker's place, shortest route first
size_t   g_placeIdx = 0;            // the one walked to now
FVector  g_placeLast{};             // where the walker stood when its progress was last seen, and since when
uint64_t g_placeSince = 0;
}  // namespace

PlaceStep PlaceWalkTick(void* me, const FVector& about, const char* who) {
    const uint64_t now = coop::net::NowMs();
    if (!g_walk) {
        if (g_placeRing.empty()) {
            g_placeRing = D::ReachableStandpoints(me, about, kPlaceFromCm, kStandpoints, kStandReachCm);
            g_placeIdx = 0;
            if (g_placeRing.empty()) {
                char what[128];
                std::snprintf(what, sizeof(what), "no NavMesh route from the %s reaches a standpoint 2200 cm from the other player",
                              who);
                Fail(what);
                return PlaceStep::Failed;
            }
        }
        if (g_placeIdx >= g_placeRing.size()) {
            Fail("the walker could not reach its place");
            return PlaceStep::Failed;
        }
        g_walk = D::StartBackgroundWalk(g_placeRing[g_placeIdx], kStandReachCm, kFirstWalkDeadlineS);
        E::TryGetActorLocation(me, g_placeLast);
        g_placeSince = now;
        UE_LOGI("[OCTA-DRILL] %s: walking to its place, 2200 cm from the other player (standpoint %u of %u)", who,
                static_cast<unsigned>(g_placeIdx + 1), static_cast<unsigned>(g_placeRing.size()));
        return PlaceStep::Walking;
    }
    const int state = g_walk->state.load();
    if (state == 1) {
        g_walk.reset();
        g_placeRing.clear();
        return PlaceStep::Reached;
    }
    if (state == 2) {   // the walk ended with no route: the next standpoint
        g_walk.reset();
        UE_LOGW("[OCTA-DRILL] %s: the walk to standpoint %u ended with no route; trying the next", who,
                static_cast<unsigned>(g_placeIdx + 1));
        ++g_placeIdx;
        return PlaceStep::Walking;
    }
    FVector p{};
    if (E::TryGetActorLocation(me, p) && FlatDist(p, g_placeLast) >= kPlaceMoveCm) {
        g_placeLast = p;
        g_placeSince = now;
    } else if (now - g_placeSince >= kPlaceStuckMs) {   // the director never gives up: the walker does
        D::CancelBackgroundWalk(*g_walk);
        g_walk.reset();
        UE_LOGW("[OCTA-DRILL] %s: stuck 10 s on the way to standpoint %u; trying the next", who,
                static_cast<unsigned>(g_placeIdx + 1));
        ++g_placeIdx;
    }
    return PlaceStep::Walking;
}

// The joiner says this line in the lobby chat when it has applied its verdict; the host, which stands in the sphere,
// leaves on it.
bool JoinerDone() {
    if (!g_joinerSeen && coop::dev::drill_chat::PollDue())
        g_joinerSeen = coop::dev::drill_chat::ChatHasLive(kJoinerDoneLine);
    return g_joinerSeen;
}

void ResetWalkers() {
    g_placeRing.clear();
    g_placeIdx = 0;
}

namespace {

// ---- CLIENT ----

uint32_t AppliedSum() {
    const coop::explosion_sync::Counters c = coop::explosion_sync::GetCounters();
    return c.applied[0] + c.applied[1] + c.applied[2];
}

void ClientFindMirror() {
    void* octa = FirstInstance(kOctaClass);
    const uint32_t eid = NpcEidOf(octa);
    if (!eid) {
        if (Expired(kBirthsBoundMs)) Fail("the host's Octa never materialised as a mirror on this client");
        return;
    }
    if (ArmOf() == Arm::Births) {
        ClientBirthsFound(eid);
        return;
    }
    // In place already: the host spawned the Octa beside it, and it stands until the host's blasts have come.
    g_octa.Set(octa);
    g_appliedBase = g_appliedSeen = AppliedSum();
    if (!IsJoiner()) g_relay0 = coop::geom_octa_relay::GetCounters();   // the joiner's stays zero
    g_laserSeen = GS::GetCounters().laser;
    UE_LOGI("[OCTA-DRILL] client: the Octa mirror eid=%u stands beside it t=%llu applied=%u", eid, Now(), g_appliedBase);
    Go(CStep::Stand);
}

// measure, the client's place: its director walks it 2200 cm from the host's puppet; the host spawns the Octa beside it
// once it has stood still.
void ClientPlace(void* me) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(coop::players::kPeerIdHost);
    void* hostBody = rp ? rp->GetActor() : nullptr;
    FVector hp{};
    if (!hostBody || !R::IsLive(hostBody) || !E::TryGetActorLocation(hostBody, hp)) {
        if (Expired(kBirthsBoundMs)) Fail("the host's puppet never resolved on this client");
        return;
    }
    if (PlaceWalkTick(me, hp, "client") != PlaceStep::Reached) return;
    UE_LOGI("[OCTA-DRILL] client in place t=%llu", Now());
    Go(CStep::Find);
}

// The §5.5 spread: each of this client's own lasers against the first blast the explosion lane applies after it, both
// stamped from the net clock. The client's chain runs one latency behind the host's, so a blast can come before it.
void NoteSpread() {
    const GS::Counters c = GS::GetCounters();
    if (c.laser > g_laserSeen) {
        g_laserSeen = c.laser;
        g_laserPendingMs = c.lastLaserMs;
    }
    const uint64_t applyMs = coop::explosion_sync::GetCounters().lastApplyMs;
    if (g_laserPendingMs && applyMs >= g_laserPendingMs) {
        UE_LOGI("[OCTA-DRILL] client: laser -> first blast applied %llu ms",
                static_cast<unsigned long long>(applyMs - g_laserPendingMs));
        g_laserPendingMs = 0;
    }
}

// The attack arm's green is a clean relay: since the mirror appeared, no Aggro skipped, no verdict timed out or dropped,
// no event dropped, and as many answers written into the overlap read as verdicts applied. The first thing that is not,
// said as the arm's failure; null when clean.
const char* RelayDirty() {
    const coop::geom_octa_relay::Counters r = coop::geom_octa_relay::GetCounters();
    const coop::geom_octa_relay::Counters& b = g_relay0;
    if (r.aggroSkipped > b.aggroSkipped) return "the relay skipped an Aggro";
    if (r.verdictTimeout > b.verdictTimeout) return "a verdict timed out";
    if (r.verdictDropped > b.verdictDropped) return "a verdict was dropped";
    if (r.eventDropped > b.eventDropped) return "an event was dropped";
    if (r.overwriteWrites - b.overwriteWrites != r.verdictApplied - b.verdictApplied)
        return "the overwrite did not write once per verdict applied";
    return nullptr;
}

// The join arm's joiner, the observer (client 2): it is world-ready after the Octa stood and was fighting the host, so
// its world-ready snapshot carries the Octa and the host's one join Aggro. It stands guarded and never walks. Green: the
// Aggro is received and replayed (its target resolved: nothing skipped), then a host verdict is applied with the answer
// written into the overlap read; it prints join DONE and says so in the lobby chat for the host.
void JoinerStand() {
    Guard();
    NoteSpread();
    const coop::geom_octa_relay::Counters r = coop::geom_octa_relay::GetCounters();
    // The host's verdicts arrive and no Aggro did: the join's send never came (the red octa_red=no_join).
    if (r.aggroReceived == g_relay0.aggroReceived && r.verdictReceived > g_relay0.verdictReceived) {
        Fail("no aggro at join (the host's verdicts reached this client and no Aggro did)");
        return;
    }
    if (const char* dirty = RelayDirty()) {
        Fail(dirty);
        return;
    }
    const uint32_t applied = r.verdictApplied - g_relay0.verdictApplied;
    if (applied > g_verdictsSeen) {
        g_verdictsSeen = applied;
        g_stepMs = coop::net::NowMs();
        UE_LOGI("[OCTA-DRILL] join: verdict %u applied", applied);
    }
    if (applied >= kJoinVerdicts) {
        if (r.aggroReplayed == g_relay0.aggroReplayed) {
            Fail("no aggro replayed at join (a verdict was applied, the join's Aggro was not)");
            return;
        }
        UE_LOGI("[OCTA-DRILL] join DONE role=joiner replayed=%u applied=%u wrote=%u base=%u superseded=%u t=%llu",
                r.aggroReplayed - g_relay0.aggroReplayed, applied, r.overwriteWrites - g_relay0.overwriteWrites,
                r.lastAggroBase, r.verdictSuperseded - g_relay0.verdictSuperseded, Now());
        coop::dev::drill_chat::Say(kJoinerDoneLine);
        Unguard();
        Go(CStep::Done);
        return;
    }
    if (Expired(kFireBoundMs)) Fail("no host fire (this client applied no verdict for 90 s)");
}

// Stands until the Octa has fired three volleys. measure counts them as volleys of the explosions this client's
// explosion lane applies (one laser makes several, so an explosion more than kVolleyGapMs after the previous one starts
// a new volley); attack counts the verdicts the relay has applied, which it can now see. The bound counts from the last
// sign of progress, so a wait that is still going is never cut.
void ClientStand(void* me) {
    if (IsJoiner()) {
        JoinerStand();
        return;
    }
    Guard();
    const uint32_t now = AppliedSum();
    if (now > g_appliedSeen) {
        const uint64_t at = coop::net::NowMs();
        g_appliedSeen = now;
        g_stepMs = at;
        if (g_volleys == 0 || at - g_lastAppliedMs > kVolleyGapMs) {
            ++g_volleys;
            if (ArmOf() == Arm::Measure)
                UE_LOGI("[OCTA-DRILL] client: volley %u of %u applied (%u explosions so far)", g_volleys, kBlastsWanted,
                        now - g_appliedBase);
        }
        g_lastAppliedMs = at;
    }
    if (RelayJudged()) {
        NoteSpread();
        if (const char* dirty = RelayDirty()) {
            Fail(dirty);
            return;
        }
        const uint32_t applied = coop::geom_octa_relay::GetCounters().verdictApplied - g_relay0.verdictApplied;
        if (applied > g_verdictsSeen) {
            g_verdictsSeen = applied;
            g_stepMs = coop::net::NowMs();
            UE_LOGI("[OCTA-DRILL] client: verdict %u of %u applied", applied, kBlastsWanted);
        }
    }
    const uint32_t reached = RelayJudged() ? g_verdictsSeen : g_volleys;
    if (reached >= kBlastsWanted) {
        if (BeginWalkOut(me, g_octa.Get(), "client")) Go(CStep::Out);
        return;
    }
    if (Expired(kFireBoundMs)) Fail("no host fire (the client's explosion lane applied no blast for 90 s)");
}

void ClientOut(void* me) {
    Guard();
    const int state = g_walk ? g_walk->state.load() : 2;
    if (state == 0) return;
    if (state == 2) {   // the next ring
        ++g_outRing;
        BeginWalkOut(me, g_octa.Get(), "client");
        return;
    }
    if (RelayJudged()) {
        if (const char* dirty = RelayDirty()) {   // a last look, over the walk-out
            Fail(dirty);
            return;
        }
        const coop::geom_octa_relay::Counters r = coop::geom_octa_relay::GetCounters();
        UE_LOGI("[OCTA-DRILL] client out t=%llu", Now());
        UE_LOGI("[OCTA-DRILL] attack DONE role=client replayed=%u applied=%u wrote=%u t=%llu",
                r.aggroReplayed - g_relay0.aggroReplayed, r.verdictApplied - g_relay0.verdictApplied,
                r.overwriteWrites - g_relay0.overwriteWrites, Now());
    } else {
        UE_LOGI("[OCTA-DRILL] client out t=%llu", Now());
    }
    Unguard();
    Go(CStep::Done);
}

void ClientTick() {
    void* me = coop::players::Registry::Get().Local();
    if (!me) return;
    switch (g_cstep) {
    case CStep::Ready:
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle)
            return;
        if (Fires() && !DamageProbeOn()) {
            Fail("damage_probe=1 is required: each blast must still log its [DMG] line");
            return;
        }
        if (ArmOf() == Arm::Attack && HostWalks()) {
            Fail("the attack arm judges the client's relay: octa_measure_target=host belongs to measure");
            return;
        }
        if (!Guard()) {
            Fail("startInvinc did not resolve on the client's player, so a blast would not leave it alive");
            return;
        }
        if (ArmOf() == Arm::Births && !g_calledEvent && !ClientBirthsCall()) return;
        if (ArmOf() == Arm::Join && !IsJoiner()) {
            UE_LOGI("[OCTA-DRILL] client: the join arm's witness; it stands guarded where it spawned");
            Go(CStep::Done);
            return;
        }
        if (IsJoiner()) {
            // The observer: world-ready after the Octa stood, so its mirror is there at the first look. Its relay's
            // baseline stays ZERO: the join Aggro is replayed inside the world-ready snapshot, before this tick.
            Go(CStep::Find);
            return;
        }
        if (Fires() && HostWalks()) {
            // The control: the host's player takes its place and meets the Octa; this client stands where it spawned,
            // guarded, and has nothing to wait for.
            UE_LOGI("[OCTA-DRILL] client: stands where it spawned, the host walks");
            Go(CStep::Done);
            return;
        }
        // measure: the client takes its place first; the Octa is spawned beside it after.
        Go(Fires() ? CStep::Place : CStep::Find);
        return;
    case CStep::Place:
        ClientPlace(me);
        return;
    case CStep::Find:
        ClientFindMirror();
        return;
    case CStep::Stand:
        ClientStand(me);
        return;
    case CStep::Out:
        ClientOut(me);
        return;
    default:
        return;
    }
}

}  // namespace
}  // namespace detail

void Tick(coop::net::Session* s) {
    using namespace detail;
    if (ArmOf() == Arm::Off || !s) return;
    if (s->role() == coop::net::Role::Host) {
        HostTick(s);
        return;
    }
    if (!s->connected() || g_cstep == CStep::Done) return;
    if (g_cstep == CStep::Ready || Due()) ClientTick();
}

void OnDisconnect() {
    using namespace detail;
    if (ArmOf() == Arm::Off) return;
    if (g_walk && g_walk->state.load() == 0) D::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
    Unguard();
    g_hstep = HStep::Wait;
    g_cstep = CStep::Ready;
    g_nextMs = g_stepMs = 0;
    g_octa.Reset();
    g_base = GS::Counters{};
    g_spawnMs = 0;
    g_aggroSaid = false;
    g_eidSaid = false;
    g_walkerSlot = 0;
    g_puppetStillAt = 0;
    g_wstep = WStep::Stand;
    g_volleysSeen = 0;
    coop::dev::octa_probe::Reset();
    g_appliedBase = g_appliedSeen = 0;
    g_volleys = 0;
    g_relay0 = coop::geom_octa_relay::Counters{};
    g_verdictsSeen = g_laserSeen = 0;
    g_laserPendingMs = 0;
    g_outRing = 0;
    g_joinHoldAt = 0;
    g_joinerSeen = false;
    g_snapJudged = false;
    ResetWalkers();
    ResetJoinHost();
    g_lastAppliedMs = 0;
    g_calledEvent = false;
}

}  // namespace coop::dev::octa_drill

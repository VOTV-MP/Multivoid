// coop/dev/explosion_drill_request.cpp -- see coop/dev/explosion_drill_internal.h.
//
// The request arms' client half. The client's game causes one explosion of its own through the game's hand verbs
// (the source arms' verbs, explosion_drill_source_verbs.cpp) and judges its own counters, as deltas against the
// world-ready edge: the shots and requests it sent, its own births dissolved, the host's blasts applied -- once
// each, never twice -- and every counter the arm does not use at 0. Past a reading's want is a failure at once; short
// of it is only not yet, until the failure bound. The host's half is explosion_drill_request_host.cpp. The table
// below is the arms and what each half must count.

#include "coop/props/join_membership_sweep.h"
#include "coop/session/net_pump.h"
#include "coop/dev/explosion_drill_internal.h"

#include "explosion_drill_source.h"  // the source arms' shared types and verbs (co-located private header)

#include "coop/dev/drill_hand.h"
#include "coop/player/hand_item.h"
#include "coop/player/players_registry.h"

#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/engine/engine.h"  // SetActorLifeSpan: request_expiry's end
#include "ue_wrap/engine/engine_attach.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace coop::dev::explosion_drill::detail {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace ES = coop::explosion_sync;
namespace ER = coop::explosion_request;

namespace {

constexpr int       kFires = 12;      // the sticky bombs of the detonate arms
constexpr int       kSwaps = 13;      // the hand swaps of request_switch: one past the hand lane's burst of twelve
constexpr long long kMaxMs = 500;     // a rocket's or sticky bomb's birth to its blast applied, on the rig's loopback
constexpr float     kUp = 100000.f;   // request_expiry's aim: straight up
constexpr float     kExpiryLifeS = 1.f;  // request_expiry: the rocket's lifespan from its tracking

// How the arm causes its explosion.
enum class Flow : uint8_t { Rocket, Sticky, Stickies, StickiesSwitch, Hand, Thrown, Self, Switch, Expiry };

struct Spec {
    const char* name;
    Arm         arm;
    Flow        flow;
    RequestWant want;  // {shots, {projectile, hand, self}, blasts, born, expired, waitedMin}
    bool        early = false;  // acts at the world-ready edge, before the client's streams start
};

constexpr Spec kSpecs[] = {
    {"request_rocket", Arm::RequestRocket, Flow::Rocket, {1, {1, 0, 0}, 1, 1, 0, 0}},
    {"request_sticky", Arm::RequestSticky, Flow::Sticky, {1, {1, 0, 0}, 1, 1, 0, 0}},
    {"request_detonate", Arm::RequestDetonate, Flow::Stickies, {kFires, {kFires, 0, 0}, kFires, kFires, 0, 0}},
    {"request_detonate_switch", Arm::RequestDetonateSwitch, Flow::StickiesSwitch,
     {kFires, {kFires, 0, 0}, kFires, kFires, 0, 0}},
    {"request_hand", Arm::RequestHand, Flow::Hand, {0, {0, 1, 0}, 1, 1, 0, 0}},
    {"request_hand_poop", Arm::RequestHandPoop, Flow::Hand, {0, {0, 1, 0}, 1, 1, 0, 0}},
    {"request_hand_soap", Arm::RequestHandSoap, Flow::Hand, {0, {0, 1, 0}, 1, 1, 0, 0}},
    {"request_pipe", Arm::RequestPipe, Flow::Hand, {0, {0, 1, 0}, 1, 1, 0, 0}},
    {"request_self", Arm::RequestSelf, Flow::Self, {0, {0, 0, 1}, 1, 1, 0, 0}},
    // The control: a thrown grenade is the host's own prop, so the request lane changes nothing on that path.
    {"request_thrown", Arm::RequestThrown, Flow::Thrown, {0, {0, 0, 0}, 1, -1, 0, 0}},
    // The shot waits on the hand state the hand lane still owes (waited >= 1; none = INCONCLUSIVE).
    {"request_switch", Arm::RequestSwitch, Flow::Switch, {1, {1, 0, 0}, 1, 1, 0, 1}},
    // No birth: the rocket ends by its own life and the host's table lets the shot go.
    {"request_expiry", Arm::RequestExpiry, Flow::Expiry, {1, {0, 0, 0}, 0, 0, 1, 0}},
    // Fired before the client's streams start: the shot waits in the outbox for the hand announce, so no timing bound.
    {"request_rocket_early", Arm::RequestRocketEarly, Flow::Rocket, {1, {1, 0, 0}, 1, 1, 0, 0}, true},
};

const Spec* SpecOf(Arm arm) {
    for (const Spec& s : kSpecs)
        if (s.arm == arm) return &s;
    return nullptr;
}

constexpr const char* kSentNames[3] = {"requestsSent[projectile]", "requestsSent[hand]", "requestsSent[self]"};

// The leg the arm acts through: the walk's own definitions, and the two grenades that derive from the walk's.
LegDef LegFor(Arm arm) {
    LegDef d{};
    switch (arm) {
    case Arm::RequestRocket:
    case Arm::RequestSwitch:
    case Arm::RequestExpiry:
    case Arm::RequestRocketEarly: return *LegOf(Kind::Rocket);
    case Arm::RequestSticky:
    case Arm::RequestDetonate:
    case Arm::RequestDetonateSwitch: return *LegOf(Kind::Sticky);
    case Arm::RequestPipe: return *LegOf(Kind::Pipebomb);
    case Arm::RequestSelf: return *LegOf(Kind::SelfPill);
    case Arm::RequestHandPoop:
        d = *LegOf(Kind::Grenade);
        d.name = "grenade_poop";
        d.row = L"grenade_poop";
        d.cls = L"prop_grenade_poop_C";
        d.path = L"/Game/objects/prop_grenade_poop.prop_grenade_poop_C";
        return d;
    case Arm::RequestHandSoap:
        d = *LegOf(Kind::Grenade);
        d.name = "grenade_soap";
        d.row = L"grenade_soap";
        d.cls = L"prop_grenade_soap_C";
        d.path = L"/Game/objects/prop_grenade_soap.prop_grenade_soap_C";
        return d;
    default: return *LegOf(Kind::Grenade);  // request_hand, request_thrown
    }
}

enum class RP : uint8_t { Fixture, Guard, Leg, Done };

struct Req {
    RP                phase = RP::Fixture;
    LegDef            leg{};
    Clock::time_point startAt{}, actAt{}, dissAt{}, appliedAt{};
    int               fired = 0, swaps = 0;
    bool              began = false;  // the leg changed the game (the hand, the pill): its put-back is owed
    bool              reGuarded = false;
    // The sticky bombs seen attached, so each is asked once.
    ue_wrap::CachedObjRef attached[kFires];
    int                   attachedN = 0;
};
Req g_req;

void Edge(Clock::time_point& t) {
    if (t == Clock::time_point{}) t = Clock::now();
}

// ---- the end ----

// Back to the start: the guard put back, the hand emptied, or the pill written back and the player put out.
void Finish() {
    Req& q = g_req;
    Unguard();
    if (q.began) {
        if (q.leg.kind == Kind::SelfPill) CleanupMeasure(q.leg, false);
        else coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr);
    }
    q.phase = RP::Done;
}

void Abandon(const char* why) {
    Warn("ABANDONED %s", why);
    Finish();
}

bool Fail(const Spec& sp, const Miss& m) {
    Warn("FAIL client arm=%s %s got=%llu want=%llu (step %d)", sp.name, m.name, static_cast<unsigned long long>(m.got),
         static_cast<unsigned long long>(m.want), static_cast<int>(g_src.step));
    Finish();
    return true;
}

// ---- the legs ----

// The arm's first verb (the held item's, or the pill's ignite) re-asserts the guard: the player's one Delay(5) may
// have cleared startInvinc since Setup held it. False once the arm is abandoned for a member that no longer resolves.
bool ReGuard() {
    Req& q = g_req;
    if (q.reGuarded) return true;
    q.reGuarded = true;
    if (!Guard(false)) return true;
    Abandon("the guard could not be re-asserted at the arm's first verb");
    return false;
}

// Every bomb standing is stuck to something, or the attach bound is up. A bomb seen attached is not asked again.
bool BombsSettled() {
    Req& q = g_req;
    g_scan.clear();
    Count(kBombCls, true, &g_scan);
    for (void* b : g_scan) {
        bool seen = false;
        for (int i = 0; i < q.attachedN && !seen; ++i) seen = q.attached[i].Is(b);
        if (seen) continue;
        void* parent = nullptr;
        if (!(E::GetAttachParentActor(b, parent) && parent)) return Past(g_src.stepAt, kAttachBound);
        if (q.attachedN < kFires) q.attached[q.attachedN++].Set(b);
    }
    return true;
}

// The held gun's reload timer, through the offset the reflection keeps per class.
bool ReloadOf(void* gun, float& out) {
    static R::InstanceOffset sReload{L"reload_primary"};
    const int32_t off = gun ? sReload.Of(gun) : -1;
    if (off < 0) return false;
    out = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(gun) + off);
    return true;
}

// The held item's verb: the game's own, and the edge the arm's timing starts from.
void Shoot(const LegDef& d, void* player) {
    if (!ReGuard()) return;
    Act(d, player);
    if (!g_src.r.skip) Edge(g_req.actAt);
}

// The first tick the hand holds the item, the launcher of request_switch fires at once: its own announce has gone
// out, the host's hand state is still owed.
void Held(const Spec& sp, const LegDef& d, void* player) {
    const bool announced = sp.flow != Flow::Switch ||
                           coop::hand_item::HeldClassIs(coop::players::Registry::Get().LocalPeerId(), d.cls);
    if (HeldOf(player, d.cls) && announced) {
        if (sp.flow == Flow::Switch) return Shoot(d, player);
        return Enter(sp.flow == Flow::Hand ? LStep::Act : LStep::Aim);
    }
    if (!Late(kStepBound)) return;
    std::snprintf(g_src.r.skipText, sizeof(g_src.r.skipText),
                  "the hand never held the item: holdingActor_main is %s, the row builds %ls",
                  HeldClassName(player).c_str(), d.cls);
    Abandon(g_src.r.skipText);
}

void ActStep(const Spec& sp, const LegDef& d, void* player) {
    Req& q = g_req;
    switch (sp.flow) {
    case Flow::Stickies:
    case Flow::StickiesSwitch: {
        if (q.fired == kFires) return Enter(LStep::AwaitAttach);
        float reload = 0.f;
        if (ReloadOf(HeldOf(player, d.cls), reload) && reload > 0.f) {
            if (Late(kStepBound)) Abandon("reload_primary never fell to 0 between two shots");
            return;
        }
        Shoot(d, player);
        if (!g_src.r.skip) {
            ++q.fired;
            Enter(LStep::Act);  // the next fire's wait is bounded from this one
        }
        return;
    }
    case Flow::Switch:
        if (q.swaps < kSwaps) {  // one swap a tick, the two items alternating
            BeginItem(*LegOf(q.swaps++ % 2 ? Kind::Pipebomb : Kind::Grenade));
            if (!g_src.r.skip) Enter(LStep::Act);
            return;
        }
        ++q.swaps;  // the launcher taken: Held fires it
        return BeginItem(d);
    default: return Shoot(d, player);
    }
}

void LegTick(const Spec& sp) {
    Req&          q = g_req;
    const LegDef& d = q.leg;
    void* player = coop::players::Registry::Get().Local();
    if (!player) return;
    if (g_src.r.skip) return Abandon(g_src.r.skip);
    switch (g_src.step) {
    case LStep::Begin:
        if (sp.flow == Flow::Self) {
            if (ReGuard()) PillIgnite(player);
            return;
        }
        return BeginItem(d);
    case LStep::Held: return Held(sp, d, player);
    case LStep::Aim: return Aim(player);
    case LStep::Act: return ActStep(sp, d, player);
    case LStep::Act2:
        if (sp.flow != Flow::Thrown) return;  // the hand arms hold the armed item to its fuse
        q.actAt = Clock::now();                // the throw is the edge the blast is timed from
        return Act2(d, player);
    case LStep::AwaitBomb: return AwaitBomb(d);
    case LStep::AwaitAttach:
        if (sp.flow == Flow::Sticky) return AwaitAttach();
        if (BombsSettled()) Enter(LStep::Detonate);
        return;
    case LStep::Detonate:
        if (sp.flow != Flow::StickiesSwitch) return Detonate(player);
        // The launcher's destruction detonates the bombs it holds.
        if (!coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr)) return Abandon("the hand could not be emptied");
        return Enter(LStep::Birth);
    case LStep::Birth:
        if (sp.flow == Flow::Expiry && !g_src.r.track.Raw()) {
            g_scan.clear();
            Count(kRocketCls, true, &g_scan);
            // The game ends a rocket that met nothing by its lifespan (InitialLifeSpan 30 s), with no blast; the arm
            // starts that same expiry now rather than trust half a minute of empty sky (a rocket sent straight up
            // once met something on the way).
            if (!g_scan.empty()) {
                Track(g_scan[0]);
                if (!ue_wrap::engine::SetActorLifeSpan(g_scan[0], kExpiryLifeS)) return Abandon("the rocket's lifespan did not set");
            }
        }
        return;
    default: return;  // the pill is lit, or the walk has ended: the counters are the verdict
    }
}

// The blast point, then the guard on the acting game; true once both are in and the leg may begin.
bool Setup(const Spec& sp) {
    Req& q = g_req;
    if (q.phase == RP::Fixture) {
        void* me = coop::players::Registry::Get().Local();
        const Fix f = ComputeB(me, me, g_src.B);
        if (f == Fix::Blocked) Abandon("request fixture blocked");
        else if (f == Fix::NoTrace) Abandon("request fixture trace unresolvable");
        else if (f == Fix::Ok) {
            SnapshotPre();  // the arm's projectiles are the ones born after this
            q.phase = RP::Guard;
        }
        return false;
    }
    g_src.host = false;
    if (Guard(false)) return false;  // a member that did not resolve is said once; the bound ends the wait
    g_src.r = Reading{};
    g_src.aim = g_src.B;
    if (sp.flow == Flow::Expiry) g_src.aim.Z += kUp;
    q.leg = LegFor(sp.arm);
    q.began = true;  // from here a put-back is owed
    Info("request %s fixture B=(%.0f,%.0f,%.0f)", sp.name, g_src.B.X, g_src.B.Y, g_src.B.Z);
    Enter(sp.flow == Flow::Switch ? LStep::Aim : LStep::Begin);  // request_switch aims before its swaps
    q.phase = RP::Leg;
    return true;
}

// ---- the judge ----

bool Judge(const Spec& sp, const ES::Counters& s0, const ER::Counters& q0) {
    Req&                q = g_req;
    const RequestWant&  w = sp.want;
    const ES::Counters  s = ES::GetCounters();
    const ER::Counters  c = ER::GetCounters();
    uint64_t sent = 0, applied = 0;
    Miss     m;
    for (int k = 0; k < 3; ++k) {
        const uint64_t out = c.requestsSent[k] - q0.requestsSent[k];
        sent += out;
        applied += s.applied[k] - s0.applied[k];
        Compare(m, kSentNames[k], out, w.kind[k]);
    }
    const uint64_t dissolved = s.dissolved - s0.dissolved;
    const Clock::time_point now = Clock::now();
    if (w.born > 0 && dissolved >= static_cast<uint64_t>(w.born)) Edge(q.dissAt);
    if (w.blasts > 0 && applied >= w.blasts) Edge(q.appliedAt);
    Compare(m, "shotsSent", c.shotsSent - q0.shotsSent, w.shots);
    Compare(m, "dissolved", dissolved, w.born < 0 ? dissolved : static_cast<uint64_t>(w.born));
    Compare(m, "applied", applied, w.blasts);
    // The counters the arm does not use stay at 0.
    Compare(m, "pendingDropped", c.pendingDropped - q0.pendingDropped, 0);
    Compare(m, "pendingDead", c.pendingDead - q0.pendingDead, 0);
    Compare(m, "pendingOrphaned", c.pendingOrphaned - q0.pendingOrphaned, 0);
    Compare(m, "clientDropped", c.clientDropped - q0.clientDropped, 0);
    Compare(m, "unmappedProjectile", c.unmappedProjectile - q0.unmappedProjectile, 0);
    Compare(m, "shotNotAnnounced", c.shotNotAnnounced - q0.shotNotAnnounced, 0);
    if (m.over) return Fail(sp, m);
    // request_expiry ends on the rocket's own end, the client's readiness edge.
    const bool ended = sp.flow != Flow::Expiry || (g_src.r.track.Raw() && !g_src.r.track.Alive());
    if (!m.name && !ended) m = Miss{"the rocket's end", 0, 1, false};
    // Equal counters are not the end: the arm's rockets and sticky bombs must be gone too, so a second blast after the
    // first is still read.
    if (!m.name) {
        const int standing = Count(kRocketCls, true) + Count(kBombCls, true);
        if (standing > 0) m = Miss{"projectiles standing", static_cast<uint64_t>(standing), 0, false};
    }
    if (m.name) return Past(q.startAt, kVerdictBound) ? Fail(sp, m) : false;
    const Clock::time_point from = q.dissAt != Clock::time_point{} ? q.dissAt : q.actAt;
    const Clock::time_point to = q.appliedAt != Clock::time_point{} ? q.appliedAt : now;
    const long long ms = (std::max)(0LL, static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count()));
    if ((sp.flow == Flow::Rocket || sp.flow == Flow::Sticky) && !sp.early && ms > kMaxMs)
        return Fail(sp, Miss{"ms from the birth to the blast applied", static_cast<uint64_t>(ms), kMaxMs, true});
    Info("client DONE PASS arm=%s sent=%llu applied=%llu ms=%lld", sp.name, static_cast<unsigned long long>(sent),
         static_cast<unsigned long long>(applied), ms);
    Finish();
    return true;
}

}  // namespace

bool RequestClientTick(Arm arm, const ES::Counters& sync0, const ER::Counters& req0) {
    Req& q = g_req;
    const Spec* sp = SpecOf(arm);
    if (!sp) return true;
    if (q.phase == RP::Done) return true;
    Edge(q.startAt);
    // An arm acts once the client's own streams run (the pose gate's two terms, net_pump), as a player in a settled
    // world does; the early arm acts at the world-ready edge on purpose.
    if (q.phase != RP::Leg && !sp->early &&
        !(coop::net_pump::IsWorldSettled() && coop::join_membership_sweep::HasLoadTailQuiesced())) {
        if (!Past(q.startAt, kVerdictBound)) return false;
        Abandon("the client's own streams never started (the load tail did not settle)");
        return true;
    }
    if (q.phase != RP::Leg) {
        const bool ready = Setup(*sp);
        if (q.phase == RP::Done) return true;
        if (!ready) {
            if (!Past(q.startAt, kBound)) return false;
            Abandon("the request arm's blast point or guard never resolved");
            return true;
        }
    }
    LegTick(*sp);
    if (q.phase == RP::Done) return true;
    return Judge(*sp, sync0, req0);
}

void RequestClientReset() {
    if (g_req.startAt != Clock::time_point{} && g_req.phase != RP::Done) Finish();
    g_req = Req{};
}

bool RequestArmFromName(const std::string& value, Arm& out) {
    for (const Spec& s : kSpecs)
        if (value == s.name) {
            out = s.arm;
            return true;
        }
    return false;
}

const char* RequestArmName(Arm a) {
    const Spec* s = SpecOf(a);
    return s ? s->name : "off";
}

const RequestWant& RequestWantOf(Arm a) {
    static const RequestWant none{};
    const Spec* s = SpecOf(a);
    return s ? s->want : none;
}

}  // namespace coop::dev::explosion_drill::detail

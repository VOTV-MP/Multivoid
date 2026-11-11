// coop/dev/explosion_drill_source.cpp -- see coop/dev/explosion_drill_internal.h.
//
// The source arms (source_host, source_client): a measurement of which peer's game reaches each request-eligible
// explosion source's birth, whether a client's rocket or sticky bomb exists on the host, and the salt rocket's
// fuse. The host role walks eight legs: four through the game's hand verbs (grenade, pipe bomb, rocket, sticky
// launcher), two that spawn a salt rocket in the world and call its attemptIgnite, and the two measure legs
// (sticky_parent, selfpill: explosion_drill_source_measure.cpp). The client role walks six, no salt rocket, since a
// client's salt rocket is the host's to light on the client's word. The other role only reads its counts and reports.
// A leg ends on a readiness edge (the birth, the attach, the rest) or on its own failure bound and prints one
// measurement line (a note line may come before it); a verb the Blueprint refuses ends that leg alone ("abandoned",
// never the run's dead marker). The run ends on a DONE line that carries a count, never a verdict.

#include "coop/dev/explosion_drill_internal.h"

#include "explosion_drill_source.h"  // the source arms' shared types (co-located private header)

#include "coop/dev/drill_hand.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_lifecycle.h"  // DestroyLocalProp: the echo-suppressed local destroy
#include "coop/world/explosion_sync.h"

#include "ue_wrap/actors/prop_fire.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/world/daynightcycle.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::explosion_drill::detail {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace ES = coop::explosion_sync;
namespace OI = ue_wrap::object_index;

Src                                g_src;
std::vector<ue_wrap::CachedObjRef> g_pre;
std::vector<void*>                 g_scan;

namespace {

// The game's row names (the list_props table), class leaves and asset paths of the sources; the held ones are
// loaded by path like the salt rocket, so a class nobody loaded yet is no reason to abandon a leg.
constexpr LegDef kLegs[] = {
    {"grenade", Kind::Grenade, L"grenade", L"prop_grenade_C", L"/Game/objects/prop_grenade.prop_grenade_C"},
    {"pipebomb", Kind::Pipebomb, L"pipebomb", L"prop_pipebomb_C", L"/Game/objects/prop_pipebomb.prop_pipebomb_C"},
    {"rocket", Kind::Rocket, L"fungun_rpg", L"prop_funGun_rpg_C", L"/Game/objects/prop_funGun_rpg.prop_funGun_rpg_C"},
    {"sticky", Kind::Sticky, L"fungun_sticky", L"prop_funGun_stickyLauncher_C",
     L"/Game/objects/prop_funGun_stickyLauncher.prop_funGun_stickyLauncher_C"},
    {"saltrocket", Kind::SaltAttached, nullptr, L"prop_saltrocket_C",
     L"/Game/objects/prop_saltrocket.prop_saltrocket_C"},
    {"saltrocket", Kind::SaltFree, nullptr, L"prop_saltrocket_C", L"/Game/objects/prop_saltrocket.prop_saltrocket_C"},
    {"sticky_parent", Kind::StickyParent, L"fungun_sticky", L"prop_funGun_stickyLauncher_C",
     L"/Game/objects/prop_funGun_stickyLauncher.prop_funGun_stickyLauncher_C"},
    {"selfpill", Kind::SelfPill, nullptr, nullptr, nullptr},
};

// The legs each role walks, as indices into kLegs: the acting role is the arm's (Arm::SourceHost acts on the host).
// A client's salt rocket is the host's to light on the client's word, so the client's walk has no salt leg.
constexpr int kHostLegs[] = {0, 1, 2, 3, 4, 5, 6, 7};
constexpr int kClientLegs[] = {0, 1, 2, 3, 6, 7};

struct LegList {
    const int* idx;
    int        count;
};

LegList LegsOf(bool host) {
    return host ? LegList{kHostLegs, static_cast<int>(sizeof(kHostLegs) / sizeof(kHostLegs[0]))}
                : LegList{kClientLegs, static_cast<int>(sizeof(kClientLegs) / sizeof(kClientLegs[0]))};
}

// Every projectile a leg can leave in the world, swept at its end (only what the drill's start did not find).
constexpr const wchar_t* kSwept[] = {L"prop_grenade_C", L"prop_pipebomb_C", L"prop_rocket_C", L"prop_stickybomb_C"};

const char* ArmName(Arm a) { return a == Arm::SourceHost ? "source_host" : "source_client"; }

}  // namespace

// ---- reading the game ----

const char* RoleName(bool host) { return host ? "host" : "client"; }

const LegDef* LegOf(Kind k) {
    for (const LegDef& d : kLegs)
        if (d.kind == k) return &d;
    return nullptr;
}

void Enter(LStep s) {
    g_src.step = s;
    g_src.stepAt = Clock::now();
}
bool Late(std::chrono::seconds bound) { return Past(g_src.stepAt, bound); }
long long Since(Clock::time_point t) {
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count());
}

bool ReadFloat(void* obj, const wchar_t* name, float& out) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return false;
    out = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(obj) + off);
    return true;
}
// A Blueprint bool is one bit of a shared byte: the offset and the mask are the property's own.
bool ReadBool(void* obj, const wchar_t* name, bool& out) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return false;
    out = (*(static_cast<const uint8_t*>(obj) + off) & mask) != 0;
    return true;
}

namespace {

// The day cycle's isRaining byte: its address and bit, or null when the cycle or the member is unreadable.
uint8_t* RainByte(uint8_t& mask) {
    void* cycle = ue_wrap::daynightcycle::Cycle();
    if (!cycle) return nullptr;
    static void*   cls = nullptr;
    static int32_t off = -1;
    static uint8_t bit = 0;
    void* c = R::ClassOf(cycle);
    if (c != cls) {
        cls = c;
        if (!R::FindBoolProperty(c, L"isRaining", off, bit)) off = -1;
    }
    if (off < 0) return nullptr;
    mask = bit;
    return static_cast<uint8_t*>(cycle) + off;
}

// The day cycle's isRaining: 1, 0, or -1 when the cycle or the member is unreadable.
int ReadRain() {
    uint8_t mask = 0;
    const uint8_t* b = RainByte(mask);
    return b ? ((*b & mask) ? 1 : 0) : -1;
}
bool WriteRain(int raining) {
    uint8_t mask = 0;
    uint8_t* b = RainByte(mask);
    if (!b) return false;
    *b = raining ? static_cast<uint8_t>(*b | mask) : static_cast<uint8_t>(*b & ~mask);
    return true;
}

// The weather the leg found, put back when the leg flipped it; the value written, or -1 when nothing was.
int PutBackRain() {
    const int before = g_src.r.rainBefore;
    return before >= 0 && ReadRain() != before && WriteRain(before) ? before : -1;
}

// The acting player is live and not dead: the pawn's own dead flag and its health (an unreadable row reads as
// neither dead nor out of health).
bool ActorAlive(void* player) {
    if (!R::IsLive(player)) return false;
    float dead = 0.f, health = 1.f;
    ue_wrap::vitals::Read(ue_wrap::vitals::Field::Dead, &dead);
    ue_wrap::vitals::Read(ue_wrap::vitals::Field::Health, &health);
    return !(dead > 0.5f) && health > 0.f;
}









struct Scan {
    int                 n = 0;
    std::vector<void*>* out = nullptr;
    bool                newOnly = false;
};
void ScanFn(void* ctx, void* obj, int32_t) {
    if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
    auto* s = static_cast<Scan*>(ctx);
    if (s->newOnly)
        for (const ue_wrap::CachedObjRef& q : g_pre)
            if (q.Is(obj)) return;
    ++s->n;
    if (s->out) s->out->push_back(obj);
}

}  // namespace

void SnapshotPre() {
    g_pre.clear();
    for (const wchar_t* cls : kSwept) {
        void* c = OI::ClassByName(cls);
        if (!c) continue;
        OI::ForEachInstance(
            c,
            [](void*, void* obj, int32_t) {
                if (R::NameStartsWith(R::NameOf(obj), L"Default__")) return;
                ue_wrap::CachedObjRef ref;
                ref.Set(obj);
                g_pre.push_back(ref);
            },
            nullptr);
    }
}

int Count(const wchar_t* cls, bool newOnly, std::vector<void*>* out) {
    void* c = OI::ClassByName(cls);
    if (!c) return 0;
    Scan s;
    s.out = out;
    s.newOnly = newOnly;
    OI::ForEachInstance(c, &ScanFn, &s);
    return s.n;
}

// The drill's own destroy. The acting host's is the game's, so the client's copy goes too; a client's is local to its
// game (DestroyLocalProp marks it as incoming, so the destroy seam sends no PropDestroy), and the host's mirror of
// what crossed is swept by the host's passive role.
void Remove(void* actor, bool broadcast) {
    if (!actor || !R::IsLive(actor)) return;
    if (broadcast) E::DestroyActor(actor);
    else coop::prop_lifecycle::DestroyLocalProp(actor, false);
}

namespace {

void SweepNew(bool broadcast) {
    for (const wchar_t* cls : kSwept) {
        g_scan.clear();
        Count(cls, true, &g_scan);
        for (void* a : g_scan) Remove(a, broadcast);
    }
}

// The watched projectile's location, read on every tick it is live; the last one read stays when it is gone.
void NoteLast() {
    Reading& r = g_src.r;
    void* a = r.track.Get();
    Vec   at{};
    if (a && E::TryGetActorLocation(a, at)) {
        r.last = at;
        r.haveLast = true;
    }
}

}  // namespace

void LastText(char* out, size_t cap) {
    const Reading& r = g_src.r;
    if (r.haveLast) std::snprintf(out, cap, "(%.0f,%.0f,%.0f)", r.last.X, r.last.Y, r.last.Z);
    else std::snprintf(out, cap, "none");
}

void Track(void* actor) {
    g_src.r.track.Set(actor);
    NoteLast();
}

namespace {

// ---- one leg's line ----

void Emit(const LegDef& d, bool host) {
    const Reading& r = g_src.r;
    const char* role = RoleName(host);
    char var[64] = "";
    if (d.kind == Kind::SaltAttached) std::snprintf(var, sizeof(var), "variant=attached parent_sim=%d ", r.parentSim);
    else if (d.kind == Kind::SaltFree) std::snprintf(var, sizeof(var), "variant=free ");
    else if (d.kind == Kind::StickyParent) std::snprintf(var, sizeof(var), "parent_sim=%d ", r.parentSim);
    if (r.skip) {
        Info("source leg=%s role=%s %sabandoned reason=%s", d.name, role, var, r.skip);
        return;
    }
    ++g_src.measured;
    char last[48];
    LastText(last, sizeof(last));
    switch (d.kind) {
    case Kind::Grenade:
        Info("source leg=%s role=%s timer=%.1f thrown=%d birth=%d ms=%lld", d.name, role, r.timer, r.thrown ? 1 : 0,
             r.birth ? 1 : 0, r.ms);
        break;
    case Kind::Pipebomb:
        Info("source leg=%s role=%s active=%d thrown=%d birth=%d ms=%lld", d.name, role, r.active, r.thrown ? 1 : 0,
             r.birth ? 1 : 0, r.ms);
        break;
    case Kind::Rocket:
        Info("source leg=%s role=%s reload_primary=%.1f ammo_primary=%d fired=%d live_prop_rocket_C_max=%d birth=%d "
             "ms=%lld last=%s", d.name, role, r.reload, r.ammo, r.fired ? 1 : 0, r.rocketMax, r.birth ? 1 : 0, r.ms,
             last);
        break;
    case Kind::Sticky:
        Info("source leg=%s role=%s reload_primary=%.1f ammo_primary=%d fired=%d live_prop_stickybomb_C_max=%d "
             "attached=%d detonated=%d birth=%d ms=%lld last=%s", d.name, role, r.reload, r.ammo, r.fired ? 1 : 0,
             r.bombMax, r.attached, r.detonated ? 1 : 0, r.birth ? 1 : 0, r.ms, last);
        break;
    case Kind::SaltAttached:
    case Kind::SaltFree: {
        char entry[24], boom[48];
        if (r.explodeMs >= 0) std::snprintf(entry, sizeof(entry), "%lld", r.explodeMs);
        else std::snprintf(entry, sizeof(entry), "none");
        if (r.birth) std::snprintf(boom, sizeof(boom), "explosion_ms=%lld", r.ms);
        else std::snprintf(boom, sizeof(boom), "no explosion in %lld s",
                           static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(kSaltBound).count()));
        Info("source leg=%s role=%s %signited=%d raining=%d->%d explode_entry_ms=%s %s last=%s", d.name, role, var,
             r.ignited ? 1 : 0, r.rain0, r.rain1, entry, boom, last);
        break;
    }
    case Kind::StickyParent:
    case Kind::SelfPill: EmitMeasure(d, host); break;
    }
}


// ---- the salt rocket legs ----
//
// Blueprint prop_saltrocket, attemptIgnite -> fire() (the control-flow reading): with no flameBase it lights the fuse
// and plays Timeline_0 (four seconds); its finish waits one second, then the attach test at @366 pops into the
// timeline at @292 whichever variant it is, so an attached and a free rocket both thrust, and the timeline's finish
// calls explode(). explode() always spawns explosion_saltrocket_C; its rain branch pops back to the spawn. R7's
// "attached gave no blast" stays unexplained. The leg reads the day cycle's isRaining before the ignite and on every
// tick.

void SaltBegin(const LegDef& d) {
    Reading& r = g_src.r;
    if (r.spawnTries > 0 && Clock::now() - r.spawnAt < kSpawnGap) return;
    void* rcls = ResolveClass(d.cls, d.path);
    if (!rcls) return Skip("prop_saltrocket_C is neither loaded nor loadable by path");
    if (r.rainBefore < 0) r.rainBefore = ReadRain();  // the leg may flip the weather; its end puts it back
    const bool attached = d.kind == Kind::SaltAttached;
    if (attached && !g_src.parent.Get()) {
        void* pcls = ResolveClass(L"prop_paper_C", L"/Game/objects/prop_paper.prop_paper_C");
        if (!pcls) return Skip("prop_paper_C is neither loaded nor loadable by path");
        if (void* p = E::SpawnActor(pcls, {g_src.B.X, g_src.B.Y, g_src.B.Z + kLift})) g_src.parent.Set(p);
        if (!g_src.parent.Get()) return Refused("the world refused the parent's spawn");
    }
    void* rk = E::SpawnActor(rcls, {g_src.B.X, g_src.B.Y, g_src.B.Z + 2.f * kLift});
    if (!rk) return Refused("the world refused the rocket's spawn");
    g_src.rocket.Set(rk);
    g_src.rest = Rest{};
    Enter(LStep::SaltRest);
}

void SaltRest(const LegDef& d) {
    void* body = (d.kind == Kind::SaltAttached ? g_src.parent : g_src.rocket).Get();
    if (!body || !g_src.rocket.Get()) return Skip("the rocket or its parent is gone before the ignite");
    NotePhysics(g_src.rest, body);
    if (AtRest(g_src.rest)) return Enter(LStep::SaltIgnite);
    if (Late(kStepBound)) Skip("the body never came to rest");
}

void SaltIgnite(const LegDef& d) {
    void* rk = g_src.rocket.Get();
    if (!rk) return Skip("the rocket is gone before the ignite");
    Reading& r = g_src.r;
    if (d.kind == Kind::SaltAttached) {
        void* pr = g_src.parent.Get();
        void* root = pr ? E::RootComponentOf(pr) : nullptr;
        void* now = nullptr;
        // The fuse reaches explode() only under a parent whose root simulates physics; any other parent measures the
        // detach route, whatever the line says.
        r.parentSim = pr && E::IsActorRootSimulatingPhysics(pr) ? 1 : 0;
        if (r.parentSim == 0) return Skip("the parent does not simulate physics: the fuse would detach, not explode");
        if (!root || !E::AttachActorToComponentSocket(rk, root, nullptr)) return Skip("the rocket could not attach");
        if (!E::GetAttachParentActor(rk, now) || now != pr) return Skip("the rocket did not attach to its parent");
    }
    r.rain0 = ReadRain();
    r.rain1 = r.rain0;
    g_src.base = ES::GetCounters();
    g_src.verbAt = Clock::now();
    r.ignited = ue_wrap::prop::CallAttemptIgnite(rk);
    if (!r.ignited) return Skip("attemptIgnite did not run");
    Track(rk);
    Enter(LStep::SaltWait);
}

void SaltWait(bool host) {
    Reading& r = g_src.r;
    const int rain = ReadRain();
    if (rain >= 0) {
        r.rain1 = rain;
        if (r.rain0 == 0 && rain == 1 && r.explodeMs < 0) r.explodeMs = Since(g_src.verbAt);
    }
    if (BirthSeen(host)) {
        r.birth = true;
        r.ms = Since(g_src.verbAt);
        return Enter(LStep::End);
    }
    if (Past(g_src.verbAt, kSaltBound)) Enter(LStep::End);
}

// ---- the leg walk ----

void StartLeg() {
    g_src.r = Reading{};
    g_src.aim = g_src.B;
    if (const wchar_t* bad = Guard(false)) {
        std::snprintf(g_src.r.skipText, sizeof(g_src.r.skipText), "the guard %ls is unreadable", bad);
        return Skip(g_src.r.skipText);
    }
    Enter(LStep::Begin);
}

void Cleanup(const LegDef& d, bool host) {
    Remove(g_src.rocket.Get(), host);
    Remove(g_src.parent.Get(), host);
    g_src.rocket.Reset();
    g_src.parent.Reset();
    // The salt rocket's explode() sets isRaining when it is false; the next leg starts from the weather this one
    // found.
    const int rain = PutBackRain();
    if (rain >= 0) Info("source leg=%s role=%s raining restored to %d", d.name, RoleName(host), rain);
    CleanupMeasure(d, host);
    // The bombs first: a launcher that leaves the hand blows the bombs it still holds.
    SweepNew(host);
    if (IsHeld(d.kind)) coop::dev::drill_hand::TakeIntoHandNow(L"None", nullptr);
}

void LegTick(Arm arm, bool host) {
    const LegDef& d = kLegs[LegsOf(g_src.host).idx[g_src.leg]];
    void* player = coop::players::Registry::Get().Local();
    if (!player && g_src.step != LStep::End) {
        if (!Late(kStepBound)) return;
        if (g_src.step == LStep::Settle) return StartLeg();
        return Skip("the local player is gone");
    }
    NoteLast();
    switch (g_src.step) {
    case LStep::Begin:
        if (g_src.r.spawnTries == 0 && !ActorAlive(player)) return Skip("the acting player is dead");
        if (IsSalt(d.kind)) return SaltBegin(d);
        if (d.kind == Kind::StickyParent) return CrateBegin();
        if (d.kind == Kind::SelfPill) return PillIgnite(player);
        return BeginItem(d);
    case LStep::Held:
        if (HeldOf(player, d.cls)) return Enter(LStep::Aim);
        if (Late(kStepBound)) {
            std::snprintf(g_src.r.skipText, sizeof(g_src.r.skipText),
                          "the hand never held the item: holdingActor_main is %s, the row builds %ls",
                          HeldClassName(player).c_str(), d.cls);
            Skip(g_src.r.skipText);
        }
        return;
    case LStep::Aim: return Aim(player);
    case LStep::Act: return Act(d, player);
    case LStep::Act2: return Act2(d, player);
    case LStep::AwaitBomb: return AwaitBomb(d);
    case LStep::AwaitAttach: return d.kind == Kind::StickyParent ? AwaitAttachCrate() : AwaitAttach();
    case LStep::Detonate: return Detonate(player);
    case LStep::Birth: return Birth(d, host);
    case LStep::SaltRest: return SaltRest(d);
    case LStep::SaltIgnite: return SaltIgnite(d);
    case LStep::SaltWait: return SaltWait(host);
    case LStep::CrateRest: return CrateRest(d);
    case LStep::Follow: return Follow();
    case LStep::Followed: return Followed(host);
    case LStep::CrateGone: return CrateGone(host);
    case LStep::PillWait: return PillWait(host, player);
    case LStep::End:
        Emit(d, host);
        Cleanup(d, host);
        if (++g_src.leg >= LegsOf(g_src.host).count) {
            Unguard();
            Info("source DONE arm=%s legs=%d measured=%d", ArmName(arm), LegsOf(g_src.host).count, g_src.measured);
            g_src.phase = SP::Done;
            return;
        }
        g_src.rest = Rest{};
        return Enter(LStep::Settle);
    case LStep::Settle:
        NoteSpeed(g_src.rest, Len(E::GetActorVelocity(player)));
        if (AtRest(g_src.rest) || Late(kStepBound)) StartLeg();
        return;
    }
}

void Abandon(const char* what) {
    Warn("ABANDONED %s", what);
    Unguard();
    g_src.phase = SP::Done;
}

void ActTick(Arm arm, bool host) {
    switch (g_src.phase) {
    case SP::Fixture: {
        void* me = coop::players::Registry::Get().Local();
        if (g_src.fixAt == Clock::time_point{}) g_src.fixAt = Clock::now();
        const Fix f = ComputeB(me, me, g_src.B);
        if (f == Fix::Wait) {
            if (Past(g_src.fixAt, kStepBound)) Abandon("source fixture unreadable");
            return;
        }
        if (f == Fix::Blocked) return Abandon("source fixture blocked");
        if (f == Fix::NoTrace) return Abandon("source fixture trace unresolvable");
        SnapshotPre();
        Info("source %s fixture arm=%s B=(%.0f,%.0f,%.0f) projectiles_before=%d", RoleName(host), ArmName(arm),
             g_src.B.X, g_src.B.Y, g_src.B.Z, static_cast<int>(g_pre.size()));
        g_src.phase = SP::Legs;
        return StartLeg();
    }
    case SP::Legs: return LegTick(arm, host);
    case SP::Done: return;
    }
}

// The role the arm does not name: its count of the two projectile classes, said on every change. The acting peer's
// leg is not shared across peers, so each line carries this peer's own sample number (a count of the changes it has
// seen, rising) and the blasts its own game has seen so far (the host's captures, the client's applied copies): the
// crossing evidence is read against the acting peer's leg lines by those two, never by a leg name. The host also
// watches the grenades and pipe bombs a client threw (PassiveThrown).
void Passive(Arm arm, bool host) {
    if (!g_src.said) {
        g_src.said = true;
        if (host) SnapshotPre();  // the host's mirrors of the client's projectiles are what it holds beyond these
        Info("source passive role=%s arm=%s reads only (the acting peer's leg is not shared: leg=unshared, sample is "
             "this peer's own change count)", RoleName(host), ArmName(arm));
    }
    if (host) PassiveThrown();
    const int rockets = Count(kRocketCls, false), bombs = Count(kBombCls, false);
    if (rockets == g_src.lastRocket && bombs == g_src.lastBomb) return;
    g_src.lastRocket = rockets;
    g_src.lastBomb = bombs;
    const ES::Counters c = ES::GetCounters();
    const uint32_t seen = host ? c.captured : c.applied[0] + c.applied[1] + c.applied[2];
    Info("source passive role=%s leg=unshared sample=%d blasts_seen=%u live prop_rocket_C=%d prop_stickybomb_C=%d",
         RoleName(host), ++g_src.sample, seen, rockets, bombs);
}

}  // namespace

// ---- the entries the legs share (declared in explosion_drill_source.h) ----

void Skip(const char* why) {
    g_src.r.skip = why;
    Enter(LStep::End);
}

// A refused spawn is counted and tried again after a gap; the third refusal ends the leg (the refusal is the answer).
void Refused(const char* why) {
    g_src.r.spawnAt = Clock::now();
    if (++g_src.r.spawnTries >= kSpawnTries) Skip(why);
}

void SourceTick(Arm arm, bool isHost) {
    g_src.host = isHost;
    if ((arm == Arm::SourceHost) == isHost) ActTick(arm, isHost);
    else Passive(arm, isHost);
}

void SourceReset() {
    Unguard();
    PutBackRain();
    // A disconnect inside the selfpill leg leaves the player lit and the pill written: the leg's own put-back.
    if (g_src.phase == SP::Legs && kLegs[LegsOf(g_src.host).idx[g_src.leg]].kind == Kind::SelfPill)
        CleanupMeasure(kLegs[LegsOf(g_src.host).idx[g_src.leg]], g_src.host);
    Remove(g_src.rocket.Get(), g_src.host);
    Remove(g_src.parent.Get(), g_src.host);
    // The host that only read holds a mirror of whatever of the client's crossed; none of it is the host's own.
    if (g_src.said && g_src.host) SweepNew(false);
    MeasureReset();
    g_src = Src{};
    g_pre.clear();
}

}  // namespace coop::dev::explosion_drill::detail

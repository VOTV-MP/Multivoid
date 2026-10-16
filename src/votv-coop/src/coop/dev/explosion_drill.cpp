// coop/dev/explosion_drill.cpp -- see coop/dev/explosion_drill.h.

#include "coop/dev/explosion_drill.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_drive_host.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"  // HasAnnouncedWorldReady
#include "coop/world/explosion_parking.h"
#include "coop/world/explosion_sync.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/explosion.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace coop::dev::explosion_drill {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace SG = ue_wrap::script_gate;
namespace JP = coop::join_progress;
namespace ES = coop::explosion_sync;
namespace X  = ue_wrap::explosion;
using Clock = std::chrono::steady_clock;

constexpr auto kBound = std::chrono::seconds(60);  // a failure bound, never a wait
constexpr auto kWatchRetry = std::chrono::seconds(1);
constexpr int  kWatchTries = 30;
constexpr int  kTag = 0x45584452;  // 'EXDR'
constexpr float kLoadHeight = 50000.f;
constexpr float kWorldAhead = 1500.f;

// The drill's own instrument: the explosion's begin-play, which a dissolved explosion never reaches.
UE_FACT_FN(kRowDrillBeginPlay, "dev.explosion_drill", L"explosion_C", L"ReceiveBeginPlay", Declared);

enum class Step : uint8_t { Idle, Spawned, Broken, Done };

// One half's run: the barrel, the counters at its spawn, and its clock.
struct Run {
    Step                 step = Step::Idle;
    ue_wrap::CachedObjRef barrel;
    Clock::time_point    at{};
    ES::Counters         base{};
    uint32_t             beginPlay0 = 0;
};

Run g_load;
Run g_world;

uint32_t g_beginPlay = 0;
bool     g_watched = false;
bool     g_watchDead = false;
int      g_watchTries = 0;
Clock::time_point g_nextWatch{};

enum class Arm : uint8_t { Off, Dissolve, Apply, Coast, SpawnMalformed };

Arm ArmOf() {
    static const Arm a = [] {
        const std::string v = coop::config::ResolveEnum(::coop::config_registry::rows::explosion_drill);
        if (v == "dissolve") return Arm::Dissolve;
        if (v == "apply") return Arm::Apply;
        if (v == "coast") return Arm::Coast;
        if (v == "spawn_malformed") return Arm::SpawnMalformed;
        return Arm::Off;
    }();
    return a;
}

SG::Verdict OnBeginPlayPre(const SG::Call&) {
    ++g_beginPlay;
    return SG::Verdict::Run;
}

bool WatchLive() { return g_watched && SG::NameWatchLive(kRowDrillBeginPlay, kTag); }

// Registered under the install policy (a refused registration is retried each second, 30 times, then the drill is
// abandoned) and its name resolved on the game thread. False once the watch is dead for good.
bool EnsureWatch() {
    if (g_watchDead) return false;
    if (!g_watched && Clock::now() >= g_nextWatch) {
        g_nextWatch = Clock::now() + kWatchRetry;
        g_watched = SG::WatchName(kRowDrillBeginPlay, kTag, &OnBeginPlayPre, nullptr);
        if (!g_watched && ++g_watchTries >= kWatchTries) {
            g_watchDead = true;
            UE_LOGW("[EXPLOSION-DRILL] ABANDONED: the begin-play watch was refused %d times", kWatchTries);
            return false;
        }
    }
    if (!g_watched) return true;
    SG::ResolvePendingNames();
    if (SG::NameWatchSettled(kRowDrillBeginPlay, kTag) && !WatchLive()) {
        g_watchDead = true;
        UE_LOGW("[EXPLOSION-DRILL] ABANDONED: the begin-play watch settled dead, so a leak cannot be seen");
        return false;
    }
    return true;
}

bool Expired(const Run& r) { return r.at != Clock::time_point{} && Clock::now() - r.at >= kBound; }

void Finish(Run& r) {
    if (void* b = r.barrel.Get()) E::DestroyActor(b);
    r.barrel.Reset();
    r.step = Step::Done;
}

void Fail(Run& r, const char* half, const char* why) {
    UE_LOGW("[EXPLOSION-DRILL] FAIL %s %s", half, why);
    Finish(r);
}

void Abandon(Run& r, const char* why) {
    UE_LOGW("[EXPLOSION-DRILL] ABANDONED: %s", why);
    Finish(r);
}

// The barrel's class, from the index or else the load by path; null when neither has it.
void* BarrelClass() {
    if (void* c = ue_wrap::object_index::ClassByName(L"prop_bombBarrel_C")) return c;
    return ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/prop_bombBarrel.prop_bombBarrel_C");
}

// The barrel's own broken(): its Blueprint spawns the explosion.
bool CallBroken(void* barrel) {
    void* fn = R::FindFunction(R::ClassOf(barrel), L"broken");
    return fn && R::CallFunction(barrel, fn, nullptr);
}

// Spawn the barrel at `at`. 1 when it stands, 0 to try again next call, -1 when the drill cannot go on.
int Spawn(Run& r, const ue_wrap::FVector& at) {
    void* cls = BarrelClass();
    if (!cls) {
        Abandon(r, "prop_bombBarrel_C is neither loaded nor loadable by path");
        return -1;
    }
    void* barrel = E::SpawnActor(cls, at);
    if (!barrel) return 0;  // the world refuses spawns for the moment
    r.barrel.Set(barrel);
    r.base = ES::GetCounters();
    r.beginPlay0 = g_beginPlay;
    r.step = Step::Spawned;
    return 1;
}

// Break the barrel once it stands.
bool Break(Run& r, const char* half) {
    void* barrel = r.barrel.Get();
    if (!barrel) {
        Fail(r, half, "the barrel is gone before its broken()");
        return false;
    }
    if (!CallBroken(barrel)) {
        Abandon(r, "the barrel's broken() did not run");
        return false;
    }
    r.step = Step::Broken;
    return true;
}

bool ClientRuns(coop::net::Session* s) {
    return s && s->running() && s->role() == coop::net::Role::Client;
}

void LoadJudge(Run& r, bool loading) {
    const ES::Counters c = ES::GetCounters();
    const uint32_t dissolved = c.dissolved - r.base.dissolved;
    const uint32_t leaked = g_beginPlay - r.beginPlay0;
    if (dissolved > 1) return Fail(r, "load", "more than one dissolve for the one barrel");
    if (leaked) return Fail(r, "load", "leak: the explosion entered begin-play");
    if (dissolved == 1) {
        if (c.dissolvedByName - r.base.dissolvedByName != 1)
            return Fail(r, "load", "the dissolve came by class, where the load window has no class table");
        if (!WatchLive()) {
            if (!loading || Expired(r)) Fail(r, "load", "the leak watch never went live");
            return;
        }
        UE_LOGI("[EXPLOSION-DRILL] client load PASS by=name");
        Finish(r);
        return;
    }
    if (!loading || Expired(r)) {
        UE_LOGW("[EXPLOSION-DRILL] FAIL load no dissolve before the world-ready edge (leak watch live=%d)",
                WatchLive() ? 1 : 0);
        Finish(r);
    }
}

void WorldJudge(Run& r) {
    const ES::Counters c = ES::GetCounters();
    const uint32_t dissolved = c.dissolved - r.base.dissolved;
    if (g_beginPlay != r.beginPlay0) return Fail(r, "world", "leak: the explosion entered begin-play");
    if (dissolved > 1) return Fail(r, "world", "more than one dissolve for the one barrel");
    if (c.dissolvedByName != r.base.dissolvedByName)
        return Fail(r, "world", "the dissolve came by name, where a loaded world has the class table");
    const bool destroyed = dissolved == 1 && c.destroyedByTask > r.base.destroyedByTask;
    if (destroyed && !R::IsLiveByIndex(c.lastDissolved, c.lastDissolvedIndex)) {
        UE_LOGI("[EXPLOSION-DRILL] client: dissolved actor gone live=0");
        UE_LOGI("[EXPLOSION-DRILL] client DONE PASS arm=dissolve");
        Finish(r);
        return;
    }
    if (Expired(r)) {
        UE_LOGW("[EXPLOSION-DRILL] FAIL world no verdict in 60 s (dissolved=%u destroyedByTask=%u)", dissolved,
                c.destroyedByTask - r.base.destroyedByTask);
        Finish(r);
    }
}

void DissolveLoad(coop::net::Session* s) {
    if (g_load.step == Step::Done || !ClientRuns(s)) return;
    const bool loading = JP::CurrentPhase() == JP::Phase::LoadingWorld &&
                         ue_wrap::world_identity::CurrentWorldKind() == ue_wrap::world_identity::WorldKind::Gameplay;
    switch (g_load.step) {
    case Step::Idle: {
        if (!loading) return;
        if (!EnsureWatch()) return Abandon(g_load, "the begin-play watch is dead");
        if (!g_watched) return;
        if (g_load.at == Clock::time_point{}) g_load.at = Clock::now();
        if (Spawn(g_load, {0.f, 0.f, kLoadHeight}) == 0 && Expired(g_load))
            Fail(g_load, "load", "the barrel could not be spawned in the load");
        return;
    }
    case Step::Spawned:
        if (!loading) return Fail(g_load, "load", "no dissolve before the world-ready edge (the barrel never broke)");
        Break(g_load, "load");
        return;
    case Step::Broken:
        EnsureWatch();
        LoadJudge(g_load, loading);
        return;
    case Step::Done:
        return;
    }
}

void DissolveWorld(coop::net::Session* s) {
    if (g_world.step == Step::Done || !ClientRuns(s)) return;
    switch (g_world.step) {
    case Step::Idle: {
        if (!coop::net_pump::HasAnnouncedWorldReady() || JP::CurrentPhase() != JP::Phase::Idle) return;
        if (!EnsureWatch()) return Abandon(g_world, "the begin-play watch is dead");
        if (g_world.at == Clock::time_point{}) g_world.at = Clock::now();
        if (!WatchLive()) {
            if (Expired(g_world)) Fail(g_world, "world", "the leak watch never went live");
            return;
        }
        void* pawn = coop::players::Registry::Get().Local();
        ue_wrap::FVector at{};
        if (!pawn || !E::TryGetActorLocation(pawn, at)) return;
        const ue_wrap::FVector fwd = E::GetActorForwardVector(pawn);
        if (Spawn(g_world, {at.X + fwd.X * kWorldAhead, at.Y + fwd.Y * kWorldAhead, at.Z + fwd.Z * kWorldAhead}) == 0 &&
            Expired(g_world))
            Fail(g_world, "world", "the barrel could not be spawned");
        return;
    }
    case Step::Spawned:
        Break(g_world, "world");
        return;
    case Step::Broken:
        WorldJudge(g_world);
        return;
    case Step::Done:
        return;
    }
}

// ---- the arms apply, coast and spawn_malformed -----------------------------------------------------------------------
// Both roles run: the host blasts, the client judges its own counters and the states it can observe itself.

constexpr float kRestSpeed = 5.f;   // cm/s: a body is at rest under this (the probe's rule)
constexpr int   kRestTicks = 10;    // consecutive Ticks under kRestSpeed
constexpr auto  kClientBound = std::chrono::seconds(120);  // from the client's world-ready edge
constexpr float kAhead = 600.f;     // the blast point, along the puppet's yaw-forward
constexpr float kClearance = 100.f; // short of whatever the line to the point hits
constexpr float kMinAhead = 300.f;  // nearer than this the fixture is blocked
constexpr float kPaperSpread = 200.f;
constexpr float kPaperLift = 50.f;
constexpr float kFindRadius = 300.f;  // the client's copies lie this close to its own blast point
constexpr float kFlyDistance = 100.f;
constexpr int   kPapers = 4;

using Vec = ue_wrap::FVector;

float Dist(const Vec& a, const Vec& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
float Len(const Vec& v) { return std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z); }
bool Past(Clock::time_point t, std::chrono::seconds bound) { return Clock::now() - t >= bound; }

void Info(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    UE_LOGI("[EXPLOSION-DRILL] %s", buf);
}
void Warn(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    UE_LOGW("[EXPLOSION-DRILL] %s", buf);
}

// One body's rest: the Ticks it has stayed under kRestSpeed, a STATE and never a count of ticks waited.
struct Rest {
    int               ticks = 0;
    bool              have = false;
    Vec               last{};
    Clock::time_point lastAt{};
};
bool AtRest(const Rest& r) { return r.ticks >= kRestTicks; }
void NoteSpeed(Rest& r, float speed) { r.ticks = speed < kRestSpeed ? r.ticks + 1 : 0; }
// A body that does not simulate (the puppet, a kinematic copy): its speed is its location change over the time
// since the last Tick.
void NoteMoved(Rest& r, const Vec& p) {
    const Clock::time_point now = Clock::now();
    if (r.have) {
        const float dt = std::chrono::duration<float>(now - r.lastAt).count();
        if (dt > 0.f) NoteSpeed(r, Dist(p, r.last) / dt);
    }
    r.have = true;
    r.last = p;
    r.lastAt = now;
}
// A simulating prop's speed is its root's physics velocity; an unreadable body is not at rest.
void NotePhysics(Rest& r, void* actor) {
    Vec lin{}, ang{};
    if (E::GetActorRootPhysicsVelocity(actor, lin, ang)) NoteSpeed(r, Len(lin));
    else r.ticks = 0;
}

// The blast point B: kAhead along `body`'s yaw-forward at its Z, short of whatever the line to it hits (the trace
// is the static and dynamic one, pawns are not in its set).
enum class Fix : uint8_t { Wait, Ok, Blocked, NoTrace };
Fix ComputeB(void* body, void* ctx, Vec& out) {
    Vec at{};
    if (!body || !ctx || !E::TryGetActorLocation(body, at)) return Fix::Wait;
    const Vec f = E::GetActorForwardVector(body);
    const float flat = std::sqrt(f.X * f.X + f.Y * f.Y);
    if (flat < 0.001f) return Fix::Wait;
    const Vec dir{f.X / flat, f.Y / flat, 0.f};
    float ahead = kAhead;
    ue_wrap::trace::Hit hit;
    if (!ue_wrap::trace::LineHitStatDyn(ctx, at, {at.X + dir.X * kAhead, at.Y + dir.Y * kAhead, at.Z}, &hit))
        return Fix::NoTrace;
    if (hit.blocked) ahead = Dist(at, hit.point) - kClearance;
    if (ahead < kMinAhead) return Fix::Blocked;
    out = {at.X + dir.X * ahead, at.Y + dir.Y * ahead, at.Z};
    return Fix::Ok;
}

void RestLine(uint32_t eid, const Vec& at) {
    Info("rest eid=%u at=(%.1f,%.1f,%.1f)", static_cast<unsigned>(eid), at.X, at.Y, at.Z);
}

// ---- the host ----

enum class HPhase : uint8_t { WaitSlot, Rest, Fixture, Apply, Malformed, PapersSpawn, PapersEnrol, PapersRest,
                              PapersFly, Done };

struct Host {
    HPhase            phase = HPhase::WaitSlot;
    Clock::time_point at{};
    Rest              body, puppet;
    Vec               B{};
    int               blast = 0;  // apply: the class due next
    bool              awaitCapture = false;
    uint32_t          capturedBefore = 0;
    size_t            coastNoted = 0;
    ue_wrap::CachedObjRef paper[kPapers];
    uint32_t          eid[kPapers] = {};
    Rest              rest[kPapers];
    Vec               pre[kPapers];
    bool              flew[kPapers] = {};
};
Host g_hostRun;

void HostEnter(HPhase p) {
    g_hostRun.phase = p;
    g_hostRun.at = Clock::now();
}
bool HostLate() { return Past(g_hostRun.at, kBound); }
void HostAbandon(const char* what) {
    Warn("ABANDONED %s", what);
    HostEnter(HPhase::Done);
}

// A native-shaped host birth: no lane scope, so the capture sees it as the game's own. False (said) on failure.
bool HostBlast(X::Cls c, const Vec& b, float damage) {
    if (!X::ResolveNow(c)) {
        char what[32];
        std::snprintf(what, sizeof(what), "class %d", static_cast<int>(c));
        HostAbandon(what);
        return false;
    }
    const float loc[3] = {b.X, b.Y, b.Z}, rot[3] = {0.f, 0.f, 0.f}, scale[3] = {1.f, 1.f, 1.f};
    const X::Params p{damage, 1000.f, 10000.f, 0.f, 0.f, 0.f, 0.f, 0, true, false};
    if (!X::SpawnDeferred(c, loc, rot, scale, p, nullptr)) {
        char what[48];
        std::snprintf(what, sizeof(what), "blast class %d did not spawn", static_cast<int>(c));
        HostAbandon(what);
        return false;
    }
    Info("host blast class=%d", static_cast<int>(c));
    return true;
}

bool SendOne(coop::net::Session& s, const coop::net::ExplosionSpawnPayload& p) {
    return s.SendReliable(coop::net::ReliableKind::ExplosionSpawn, &p, static_cast<int>(sizeof(p)));
}

// The one valid payload, built by hand (the capture builds its own inline), then four copies with one field each
// broken.
void HostMalformed(coop::net::Session& s) {
    coop::net::ExplosionSpawnPayload good{};
    good.classId = 0;
    good.flags = 1;
    good.loc[0] = g_hostRun.B.X;
    good.loc[1] = g_hostRun.B.Y;
    good.loc[2] = g_hostRun.B.Z;
    good.scale[0] = good.scale[1] = good.scale[2] = 1.f;
    good.radius = 1000.f;
    good.force = 10000.f;
    bool ok = SendOne(s, good);
    coop::net::ExplosionSpawnPayload bad = good;
    bad.loc[0] = std::numeric_limits<float>::quiet_NaN();
    ok = SendOne(s, bad) && ok;
    bad = good;
    bad.classId = 9;
    ok = SendOne(s, bad) && ok;
    bad = good;
    bad.radius = -1.f;
    ok = SendOne(s, bad) && ok;
    bad = good;
    bad.flags = 4;
    ok = SendOne(s, bad) && ok;
    if (!ok) return HostAbandon("a malformed payload was not sent");
    Info("host sent malformed n=5");
    HostEnter(HPhase::Done);
}

void HostApply() {
    Host& h = g_hostRun;
    if (!h.awaitCapture) {
        h.capturedBefore = ES::GetCounters().captured;
        if (!HostBlast(static_cast<X::Cls>(h.blast), h.B, 10.f)) return;
        h.awaitCapture = true;
        h.at = Clock::now();
        return;
    }
    if (ES::GetCounters().captured > h.capturedBefore) {
        h.awaitCapture = false;
        if (++h.blast == static_cast<int>(X::Cls::Count)) {
            Info("host DONE arm=apply");
            HostEnter(HPhase::Done);
        }
        return;
    }
    if (HostLate()) {
        Warn("FAIL host capture class=%d", h.blast);
        HostEnter(HPhase::Done);
    }
}

void HostPapersSpawn() {
    Host& h = g_hostRun;
    void* cls = ue_wrap::object_index::ClassByName(L"prop_paper_C");
    if (!cls) {
        if (HostLate()) HostAbandon("class prop_paper_C");
        return;
    }
    h.coastNoted = coop::prop_drive_host::CoastingCount();
    const float off[kPapers][2] = {{kPaperSpread, 0.f}, {-kPaperSpread, 0.f}, {0.f, kPaperSpread},
                                   {0.f, -kPaperSpread}};
    for (int i = 0; i < kPapers; ++i) {
        void* a = E::SpawnActor(cls, {h.B.X + off[i][0], h.B.Y + off[i][1], h.B.Z + kPaperLift});
        if (!a) return HostAbandon("a paper did not spawn");
        h.paper[i].Set(a);
        h.rest[i] = Rest{};
    }
    HostEnter(HPhase::PapersEnrol);
}

void HostPapersEnrol() {
    Host& h = g_hostRun;
    int enrolled = 0;
    for (int i = 0; i < kPapers; ++i) {
        void* a = h.paper[i].Get();
        if (!a) return HostAbandon("a paper is gone before it was enrolled");
        h.eid[i] = coop::element::Registry::Get().EidForActor(a);
        if (h.eid[i] != coop::element::kInvalidId) ++enrolled;
    }
    if (enrolled == kPapers) return HostEnter(HPhase::PapersRest);
    if (HostLate()) HostAbandon("the papers were not enrolled");
}

// The papers rest and their own birth streams have ended: the blast with no damage.
void HostPapersRest() {
    Host& h = g_hostRun;
    bool all = true;
    for (int i = 0; i < kPapers; ++i) {
        void* a = h.paper[i].Get();
        if (!a) return HostAbandon("a paper is gone before it rested");
        NotePhysics(h.rest[i], a);
        all = all && AtRest(h.rest[i]);
    }
    if (!all || coop::prop_drive_host::CoastingCount() > h.coastNoted) {
        if (HostLate()) HostAbandon("the papers never rested or their streams never ended");
        return;
    }
    for (int i = 0; i < kPapers; ++i)
        if (!E::TryGetActorLocation(h.paper[i].Get(), h.pre[i])) return HostAbandon("a paper's location is unreadable");
    Info("host papers eids=%u,%u,%u,%u", static_cast<unsigned>(h.eid[0]), static_cast<unsigned>(h.eid[1]),
         static_cast<unsigned>(h.eid[2]), static_cast<unsigned>(h.eid[3]));
    if (!HostBlast(X::Cls::Explosion, h.B, 0.f)) return;
    for (int i = 0; i < kPapers; ++i) h.rest[i] = Rest{};
    HostEnter(HPhase::PapersFly);
}

// Each paper flew and came to rest again; its final rest is logged for the lead's comparison with the client's.
void HostPapersFly() {
    Host& h = g_hostRun;
    bool all = true;
    for (int i = 0; i < kPapers; ++i) {
        void* a = h.paper[i].Get();
        Vec at{};
        if (!a || !E::TryGetActorLocation(a, at)) return HostAbandon("a paper is gone after the blast");
        NotePhysics(h.rest[i], a);
        if (!h.flew[i] && Dist(at, h.pre[i]) > kFlyDistance) {
            h.flew[i] = true;
            h.rest[i].ticks = 0;
        }
        all = all && h.flew[i] && AtRest(h.rest[i]);
    }
    if (all) {
        for (int i = 0; i < kPapers; ++i) {
            Vec at{};
            E::TryGetActorLocation(h.paper[i].Get(), at);
            RestLine(h.eid[i], at);
        }
        Info("host DONE arm=coast");
        return HostEnter(HPhase::Done);
    }
    if (!HostLate()) return;
    char what[64];
    for (int i = 0; i < kPapers; ++i)
        if (!h.flew[i] || !AtRest(h.rest[i])) {
            std::snprintf(what, sizeof(what), "host paper did not fly or rest eid=%u", static_cast<unsigned>(h.eid[i]));
            return HostAbandon(what);
        }
}

void HostTick(coop::net::Session& s, Arm arm) {
    Host& h = g_hostRun;
    switch (h.phase) {
    case HPhase::WaitSlot:
        if (s.IsSlotWorldReady(1)) HostEnter(HPhase::Rest);
        return;
    case HPhase::Rest: {
        void* me = coop::players::Registry::Get().Local();
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1);
        void* pup = rp ? rp->GetActor() : nullptr;
        Vec at{};
        if (!me || !pup || !E::TryGetActorLocation(pup, at)) {
            h.body = Rest{};
            h.puppet = Rest{};
        } else {
            NoteSpeed(h.body, Len(E::GetActorVelocity(me)));
            NoteMoved(h.puppet, at);
        }
        if (AtRest(h.body) && AtRest(h.puppet)) return HostEnter(HPhase::Fixture);
        if (HostLate()) HostAbandon("bodies never rested");
        return;
    }
    case HPhase::Fixture: {
        coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(1);
        const Fix f = ComputeB(rp ? rp->GetActor() : nullptr, coop::players::Registry::Get().Local(), h.B);
        if (f == Fix::Wait) {
            if (HostLate()) HostAbandon("fixture unreadable");
            return;
        }
        if (f == Fix::Blocked) return HostAbandon("fixture blocked");
        if (f == Fix::NoTrace) return HostAbandon("fixture trace unresolvable");
        Info("host fixture B=(%.0f,%.0f,%.0f)", h.B.X, h.B.Y, h.B.Z);
        HostEnter(arm == Arm::Apply ? HPhase::Apply : arm == Arm::Coast ? HPhase::PapersSpawn : HPhase::Malformed);
        return;
    }
    case HPhase::Apply: return HostApply();
    case HPhase::Malformed: return HostMalformed(s);
    case HPhase::PapersSpawn: return HostPapersSpawn();
    case HPhase::PapersEnrol: return HostPapersEnrol();
    case HPhase::PapersRest: return HostPapersRest();
    case HPhase::PapersFly: return HostPapersFly();
    case HPhase::Done: return;
    }
}

// ---- the client ----

enum class CPhase : uint8_t { Edge, Join, Rest, Fixture, Apply, Malformed, Find, Settle, Blast, Fly, Done };

struct Client {
    CPhase            phase = CPhase::Edge;
    Clock::time_point edgeAt{}, at{}, foundAt{};
    ES::Counters      sync{};
    coop::explosion_parking::Counters park{};
    Rest              pawn;
    Vec               B{};
    ue_wrap::CachedObjRef copy[kPapers];
    uint32_t          eid[kPapers] = {};
    Rest              rest[kPapers];
    Vec               base[kPapers];
    bool              moved[kPapers] = {};
};
Client g_clientRun;
std::vector<coop::element::Registry::ActorIdPair> g_snap;  // reused by every find

void ClientEnter(CPhase p) {
    g_clientRun.phase = p;
    g_clientRun.at = Clock::now();
}
void ClientAbandon(const char* what) {
    Warn("ABANDONED %s", what);
    g_clientRun.phase = CPhase::Done;
}
void ClientFail(const char* arm, const char* what) {
    Warn("FAIL %s %s", arm, what);
    g_clientRun.phase = CPhase::Done;
}
bool EdgeLate() { return Past(g_clientRun.edgeAt, kClientBound); }

// Apply: each class applied once, its force narrowed and its rune check refused, nothing dropped. PASS as soon as
// every reading holds; else, at the bound, the first reading that does not.
void ClientApply() {
    Client& c = g_clientRun;
    const ES::Counters sy = ES::GetCounters();
    const coop::explosion_parking::Counters pk = coop::explosion_parking::GetCounters();
    uint32_t dropped = 0;
    for (int i = 0; i < static_cast<int>(ES::DropReason::Count); ++i) dropped += sy.dropped[i] - c.sync.dropped[i];
    const bool applied = sy.applied[0] - c.sync.applied[0] == 1 && sy.applied[1] - c.sync.applied[1] == 1 &&
                         sy.applied[2] - c.sync.applied[2] == 1;
    const bool narrowed = pk.narrowed - c.park.narrowed == 3;
    const bool rune = pk.runeRefused - c.park.runeRefused == 3;
    const bool dropClean = dropped == 0;
    const bool narrowClean = pk.narrowFailed == c.park.narrowFailed;
    if (applied && narrowed && rune && dropClean && narrowClean) {
        uint32_t calls[4], refused[4];
        for (int i = 0; i < 4; ++i) {
            calls[i] = pk.verbCalls[i] - c.park.verbCalls[i];
            refused[i] = pk.verbRefused[i] - c.park.verbRefused[i];
        }
        Info("client DONE PASS arm=apply verbs calls=%u/%u/%u/%u refused=%u/%u/%u/%u", calls[0], calls[1], calls[2],
             calls[3], refused[0], refused[1], refused[2], refused[3]);
        c.phase = CPhase::Done;
        return;
    }
    if (!EdgeLate()) return;
    ClientFail("apply", !applied ? "applied" : !narrowed ? "narrowed" : !rune ? "runeRefused"
                      : !dropClean ? "dropped" : "narrowFailed");
}

// Spawn_malformed: four drops, one each by its reason, and the one valid message applied.
void ClientMalformed() {
    Client& c = g_clientRun;
    const ES::Counters sy = ES::GetCounters();
    auto d = [&](ES::DropReason r) {
        const int i = static_cast<int>(r);
        return sy.dropped[i] - c.sync.dropped[i];
    };
    const uint32_t nonfinite = d(ES::DropReason::NonFinite), cls = d(ES::DropReason::Class),
                   range = d(ES::DropReason::Range), flags = d(ES::DropReason::Flags),
                   applied = sy.applied[0] - c.sync.applied[0];
    if (nonfinite == 1 && cls == 1 && range == 1 && flags == 1 && applied == 1) {
        Info("client DONE PASS arm=spawn_malformed");
        c.phase = CPhase::Done;
        return;
    }
    if (!EdgeLate()) return;
    ClientFail("spawn_malformed", nonfinite != 1 ? "nonfinite" : cls != 1 ? "class" : range != 1 ? "range"
                                : flags != 1 ? "flags" : "applied");
}

// The prop_paper_C elements within kFindRadius of B: how many there are, and the first four.
int FindPapers(const Vec& b, void* out[kPapers], uint32_t eid[kPapers]) {
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, g_snap);
    int n = 0;
    for (const auto& p : g_snap) {
        Vec at{};
        if (!p.actor || !R::IsLiveByIndex(p.actor, p.internalIdx) || !E::TryGetActorLocation(p.actor, at) ||
            Dist(at, b) > kFindRadius || !R::NameEquals(R::NameOf(R::ClassOf(p.actor)), L"prop_paper_C"))
            continue;
        if (n < kPapers) {
            out[n] = p.actor;
            eid[n] = p.id;
        }
        ++n;
    }
    return n;
}

// Every copy's rest, one Tick. False (abandoned) when a copy is gone.
bool TrackCopies(Client& c, Vec at[kPapers]) {
    for (int i = 0; i < kPapers; ++i) {
        void* a = c.copy[i].Get();
        if (!a || !E::TryGetActorLocation(a, at[i])) {
            char what[48];
            std::snprintf(what, sizeof(what), "a paper copy is gone eid=%u", static_cast<unsigned>(c.eid[i]));
            ClientAbandon(what);
            return false;
        }
        NoteMoved(c.rest[i], at[i]);
    }
    return true;
}
bool AllRest(const Client& c) {
    for (int i = 0; i < kPapers; ++i)
        if (!AtRest(c.rest[i])) return false;
    return true;
}

void ClientCoast() {
    Client& c = g_clientRun;
    Vec at[kPapers] = {};
    switch (c.phase) {
    case CPhase::Find: {
        void* found[kPapers];
        uint32_t ids[kPapers];
        const int n = FindPapers(c.B, found, ids);
        if (n > kPapers) return ClientAbandon("more than four papers near B");
        if (n < kPapers) {
            if (Past(c.at, kBound)) ClientAbandon("the four paper copies were not found");
            return;
        }
        for (int i = 0; i < kPapers; ++i) {
            c.copy[i].Set(found[i]);
            c.eid[i] = ids[i];
            c.rest[i] = Rest{};
        }
        c.foundAt = Clock::now();
        c.phase = CPhase::Settle;
        return;
    }
    case CPhase::Settle:
        if (!TrackCopies(c, at)) return;
        if (AllRest(c)) {
            c.phase = CPhase::Blast;
        } else if (ES::GetCounters().applied[0] != c.sync.applied[0]) {
            ClientAbandon("the blast arrived before the paper copies rested");
        } else if (Past(c.foundAt, kBound)) {
            ClientAbandon("the paper copies never came to rest");
        }
        return;
    case CPhase::Blast:
    case CPhase::Fly: {
        if (!TrackCopies(c, at)) return;
        if (c.phase == CPhase::Blast && ES::GetCounters().applied[0] != c.sync.applied[0]) {
            for (int i = 0; i < kPapers; ++i) c.base[i] = at[i];  // the baseline is the blast, never the papers' fall
            c.phase = CPhase::Fly;
        }
        if (c.phase == CPhase::Fly) {
            for (int i = 0; i < kPapers; ++i) {
                if (!c.moved[i] && Dist(at[i], c.base[i]) > kFlyDistance) {
                    c.moved[i] = true;
                    c.rest[i].ticks = 0;
                }
            }
            bool all = true;
            for (int i = 0; i < kPapers; ++i) all = all && c.moved[i] && AtRest(c.rest[i]);
            if (all) {
                for (int i = 0; i < kPapers; ++i) RestLine(c.eid[i], at[i]);
                Info("client DONE PASS arm=coast moved=%.0f/%.0f/%.0f/%.0f", Dist(at[0], c.base[0]),
                     Dist(at[1], c.base[1]), Dist(at[2], c.base[2]), Dist(at[3], c.base[3]));
                c.phase = CPhase::Done;
                return;
            }
        }
        if (!Past(c.foundAt, kBound) && !EdgeLate()) return;
        char what[64];
        for (int i = 0; i < kPapers; ++i)
            if (!c.moved[i]) {
                std::snprintf(what, sizeof(what), "copy did not fly eid=%u", static_cast<unsigned>(c.eid[i]));
                return ClientFail("coast", what);
            }
        for (int i = 0; i < kPapers; ++i)
            if (!AtRest(c.rest[i])) {
                std::snprintf(what, sizeof(what), "copy did not rest eid=%u", static_cast<unsigned>(c.eid[i]));
                return ClientFail("coast", what);
            }
        return;
    }
    default:
        return;
    }
}

void ClientTick(Arm arm) {
    Client& c = g_clientRun;
    switch (c.phase) {
    case CPhase::Edge:
        if (!coop::net_pump::HasAnnouncedWorldReady()) return;
        c.sync = ES::GetCounters();  // before the host's fixture can blast
        c.park = coop::explosion_parking::GetCounters();
        c.edgeAt = Clock::now();
        ClientEnter(CPhase::Join);
        return;
    case CPhase::Join:
        if (JP::CurrentPhase() == JP::Phase::Idle) ClientEnter(CPhase::Rest);
        return;
    case CPhase::Rest: {
        void* pawn = coop::players::Registry::Get().Local();
        if (pawn) NoteSpeed(c.pawn, Len(E::GetActorVelocity(pawn)));
        else c.pawn = Rest{};
        if (AtRest(c.pawn)) return ClientEnter(CPhase::Fixture);
        if (Past(c.at, kBound)) ClientAbandon("the client's pawn never rested");
        return;
    }
    case CPhase::Fixture: {
        void* pawn = coop::players::Registry::Get().Local();
        const Fix f = ComputeB(pawn, pawn, c.B);
        if (f == Fix::Wait) {
            if (Past(c.at, kBound)) ClientAbandon("fixture unreadable");
            return;
        }
        if (f == Fix::Blocked) return ClientAbandon("fixture blocked");
        if (f == Fix::NoTrace) return ClientAbandon("fixture trace unresolvable");
        Info("client fixture B=(%.0f,%.0f,%.0f)", c.B.X, c.B.Y, c.B.Z);
        ClientEnter(arm == Arm::Apply ? CPhase::Apply : arm == Arm::Coast ? CPhase::Find : CPhase::Malformed);
        return;
    }
    case CPhase::Apply: return ClientApply();
    case CPhase::Malformed: return ClientMalformed();
    case CPhase::Find:
    case CPhase::Settle:
    case CPhase::Blast:
    case CPhase::Fly: return ClientCoast();
    case CPhase::Done: return;
    }
}

}  // namespace

void TickLoad(coop::net::Session* s) {
    if (ArmOf() == Arm::Dissolve) DissolveLoad(s);
}

void Tick(coop::net::Session* s) {
    const Arm arm = ArmOf();
    if (arm == Arm::Off || !s) return;
    if (arm == Arm::Dissolve) return DissolveWorld(s);
    if (s->role() == coop::net::Role::Host) {
        if (s->connected()) HostTick(*s, arm);
    } else if (ClientRuns(s)) {
        ClientTick(arm);
    }
}

void OnDisconnect() {
    g_load = Run{};
    g_world = Run{};
    g_hostRun = Host{};
    g_clientRun = Client{};
}

}  // namespace coop::dev::explosion_drill

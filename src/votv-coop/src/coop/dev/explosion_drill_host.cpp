// coop/dev/explosion_drill_host.cpp -- see coop/dev/explosion_drill_internal.h.

#include "coop/dev/explosion_drill_internal.h"

#include "coop/element/element.h"
#include "coop/element/registry.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_drive_host.h"
#include "coop/world/explosion_sync.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/world/explosion.h"

#include <cstdint>
#include <cstdio>
#include <limits>

namespace coop::dev::explosion_drill::detail {

namespace E  = ue_wrap::engine;
namespace ES = coop::explosion_sync;
namespace X  = ue_wrap::explosion;

namespace {

constexpr float kPaperSpread = 200.f;
constexpr float kPaperLift = 50.f;

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

}  // namespace

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

void HostReset() { g_hostRun = Host{}; }

}  // namespace coop::dev::explosion_drill::detail

// coop/dev/container_intent_drill_throw_hostgrab.cpp -- the container intent drill's throwhostgrab arm: the single-player-
// shaped control of throwgrab. The HOST grabs a drive it spawned with its own player and throws it by the fire branch's
// functions (container_intent_drill_throw_act.cpp), aimed at the backpack as the other throw arms aim; no client acts. The
// CLIENT only traces the mirror of the drive and of the backpack. Both peers print the trace lines, the first-hit line
// (container_intent_drill_throw_hit.cpp) and an end line; the host's `MEASURED` and DONE end the run. See
// container_intent_drill_throw.h for what is measured.

#include "container_intent_drill_throw.h"
#include "container_route_drill_detail.h"

#include "coop/dev/container_route_census.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/remote_prop.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace coop::dev::container_intent_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace ci  = ue_wrap::container_inventory;
namespace DR  = coop::director;
namespace DC  = coop::dev::drill_chat;
namespace RD  = coop::dev::container_route_drill::detail;
namespace cen = coop::dev::container_route_census;
using ue_wrap::FVector;

constexpr const char* kHgFixture       = "ci-hgrab host fixture ";   // + "pack=<eid> item=<eid>"
constexpr const char* kHgGo            = "ci-hgrab host go";          // the host is aimed: the client starts its trace
constexpr const char* kHgTracing       = "ci-hgrab client tracing";   // the client traces: the host throws
constexpr const char* kHgEnd           = "ci-hgrab host end";
constexpr const char* kHgDone          = "ci-hgrab client done";
constexpr const char* kHgHostFailed    = "ci-hgrab host failed";
constexpr const char* kHgClientFailed  = "ci-hgrab client failed";

constexpr int      kR4Route          = 3;       // container_route_census::RouteVerbs: R1..R6 are 0..5
constexpr int      kWalkDeadlineS    = 150;
constexpr float    kTargetAheadCm    = 600.f;
constexpr uint64_t kBindBoundMs      = 15000;
constexpr uint64_t kPeerBoundMs      = 120000;  // a world-ready client; the client's wait for the fixture and its mirrors
constexpr uint64_t kWalkBoundMs      = 200000;
constexpr uint64_t kGrabBoundMs      = 20000;
constexpr uint64_t kChatBoundMs      = 120000;  // the go / tracing / end / done lines (drill chat leaves one a second)
constexpr uint64_t kClientTraceBoundMs = 300000;

unsigned long long U(uint64_t v) { return static_cast<unsigned long long>(v); }

// What both peers keep of the throw they trace.
struct Seen {
    Track track;
    HitJudge hit;
    uint32_t packEid = 0;
    int count0 = -1;
    uint64_t ovSeen = 0, ovAll0 = 0, adds0 = 0, pressMs = 0, endAtMs = 0;
    int ovCount = 0;
};

void SeenBegin(Seen& s, const char* role, void* actor, bool releaseKnown) {
    TrackBegin(s.track, role, "local");
    TrackSet(s.track, actor);
    s.hit.Begin(releaseKnown);
    s.count0 = CountOf(s.packEid);
    s.ovSeen = cen::LastR4Overlap().seq;
    s.ovAll0 = cen::R4Overlaps();
    s.adds0 = cen::RouteVerbs(kR4Route, /*take=*/false);
    s.ovCount = 0;
    s.pressMs = cen::UnixMs();
    s.endAtMs = 0;
}

void PollOverlap(Seen& s) {
    const cen::R4Overlap o = cen::LastR4Overlap();
    if (o.seq == s.ovSeen) return;
    s.ovSeen = o.seq;
    if (o.container != s.packEid || !o.actor || o.actor != s.track.ref.Raw()) return;
    ++s.ovCount;
    UE_LOGI("[CI-DRILL] throw %s overlap eid=%u sim=%d t=%llu", s.track.role, o.eid, o.sim ? 1 : 0, U(o.t));
}

// One tick of the trace; true when the throw's trace is over (rest and kAfterRestMs, never under kMinTraceMs, never over
// kFlightBoundMs, from the start of the trace).
bool SeenTick(Seen& s, int rttMs) {
    TrackTick(s.track, rttMs);
    PollOverlap(s);
    s.hit.Tick(s.track);
    const uint64_t now = cen::UnixMs();
    if (!s.endAtMs) {
        if (s.track.atRest) s.endAtMs = s.track.restMs + kAfterRestMs;
        else if (s.track.gone) s.endAtMs = s.track.goneMs + kAfterRestMs;
        if (s.endAtMs && s.endAtMs < s.pressMs + kMinTraceMs) s.endAtMs = s.pressMs + kMinTraceMs;
    }
    return (s.endAtMs && now >= s.endAtMs) || now >= s.pressMs + kFlightBoundMs;
}

void SeenEnd(Seen& s) {
    s.hit.Finish(s.track);
    HitWatchEnd();
    const int count1 = CountOf(s.packEid);
    UE_LOGI("[CI-DRILL] %s %s end: arc=%s gone=%d rest=(%.1f,%.1f,%.1f) overlaps=%d (census all %llu) addObject=%llu target %d -> "
            "%d peak %.0f cm/s at t=%llu", s.track.role, ArmName(), ArcOf(s.track), s.track.gone ? 1 : 0,
            static_cast<double>(s.track.last.X), static_cast<double>(s.track.last.Y), static_cast<double>(s.track.last.Z),
            s.ovCount, U(cen::R4Overlaps() - s.ovAll0), U(cen::RouteVerbs(kR4Route, false) - s.adds0), s.count0, count1,
            static_cast<double>(s.track.peakSpeed), U(s.track.peakT));
}

void Failed(const char* line, const char* fmt, va_list ap) {
    char buf[300];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    DC::Say(line);
    Fail("%s", buf);
}

// ---- the host: the thrower ----------------------------------------------------------------------------------------------

enum class HStep : uint8_t { WaitPeer, Bind, Walk, Grab, Aim, WaitTracing, Flight, WaitDone, Done };
HStep    g_hStep = HStep::WaitPeer;
uint64_t g_hMs = 0, g_hNextMs = 0;
ue_wrap::CachedObjRef g_hTarget, g_hItem;
uint32_t g_hTargetEid = 0, g_hItemEid = 0;
FVector  g_hStart{}, g_hPack{}, g_hStand{}, g_hGoal{};
std::shared_ptr<DR::BackgroundWalk> g_hWalk;
std::unique_ptr<DR::AimedGrab> g_hGrab;
Aimer    g_hAimer;
Seen     g_hSeen;

void HNext(HStep s) { g_hStep = s; g_hMs = ::GetTickCount64(); }
bool HExpired(uint64_t bound) { return ::GetTickCount64() - g_hMs > bound; }

void HFail(const char* fmt, ...) {
    if (g_hWalk && g_hWalk->state.load() == 0) DR::CancelBackgroundWalk(*g_hWalk);
    g_hWalk.reset();
    g_hGrab.reset();
    va_list ap;
    va_start(ap, fmt);
    Failed(kHgHostFailed, fmt, ap);
    va_end(ap);
    HNext(HStep::Done);
}

FVector Place(const FVector& at, const FVector& fwd, float ahead, float side) {
    return {at.X + fwd.X * ahead + fwd.Y * side, at.Y + fwd.Y * ahead - fwd.X * side, at.Z + RD::kDropCm};
}

bool EidBound(uint32_t eid) { return eid != static_cast<uint32_t>(coop::element::kInvalidId); }

// The backpack 600 cm ahead of the host's player and the drive beside the standpoint, 350 cm before the backpack.
void StepHWaitPeer(const coop::net::Session& s) {
    bool peer = false;
    for (int i = 1; i < coop::net::kMaxPeers; ++i) peer = peer || s.IsSlotWorldReady(i);
    void* player = coop::players::Registry::Get().Local();
    FVector at{};
    if (!peer || !player || !E::TryGetActorLocation(player, at)) {
        if (HExpired(kPeerBoundMs)) HFail("no world-ready client within %llu s", U(kPeerBoundMs / 1000));
        return;
    }
    FVector fwd = E::GetActorForwardVector(player);
    const float len = std::hypot(fwd.X, fwd.Y);
    fwd = len < 0.01f ? FVector{1.f, 0.f, 0.f} : FVector{fwd.X / len, fwd.Y / len, 0.f};
    void* targetCls = ue_wrap::asset_load::LoadObjectByPath(RD::kPackPath);
    void* itemCls = R::FindClass(RD::kDriveClass);
    if (!targetCls || !itemCls) {
        HFail("the fixture's class does not load (target %p, drive %p)", targetCls, itemCls);
        return;
    }
    void* target = E::SpawnActor(targetCls, Place(at, fwd, kTargetAheadCm, 0.f));
    void* item = target ? E::SpawnActor(itemCls, Place(at, fwd, kTargetAheadCm - kStandFromTargetCm, kItemSideCm)) : nullptr;
    if (!target || !ci::IsContainer(target) || !item) {
        HFail("the fixture did not spawn (target %p, drive %p)", target, item);
        return;
    }
    g_hTarget.Set(target);
    g_hItem.Set(item);
    g_hStart = at;
    HNext(HStep::Bind);
}

void StepHBind() {
    void* target = g_hTarget.Get();
    void* item = g_hItem.Get();
    if (!target || !item) {
        HFail("a fixture actor died before the walk");
        return;
    }
    const uint32_t te = RD::EidOf(target), ie = RD::EidOf(item);
    const int count = EidBound(te) && EidBound(ie) ? CountOf(te) : -1;
    if (count < 0) {
        if (HExpired(kBindBoundMs)) HFail("the fixture was never bound and published (target eid %u, drive eid %u)", te, ie);
        return;
    }
    g_hTargetEid = g_hSeen.packEid = te;
    g_hItemEid = ie;
    char say[64];
    std::snprintf(say, sizeof(say), "%spack=%u item=%u", kHgFixture, te, ie);
    DC::Say(say);
    E::TryGetActorLocation(target, g_hPack);
    float ux = g_hPack.X - g_hStart.X, uy = g_hPack.Y - g_hStart.Y;
    const float len = std::hypot(ux, uy);
    if (len < 1.f) {
        HFail("the host stands on the target");
        return;
    }
    g_hGoal = {g_hPack.X - ux / len * kStandFromTargetCm, g_hPack.Y - uy / len * kStandFromTargetCm, g_hStart.Z};
    g_hWalk = DR::StartBackgroundWalk(g_hGoal, kWalkReachCm, kWalkDeadlineS, /*carry=*/false);
    UE_LOGI("[CI-DRILL] host %s: target eid=%u, drive eid=%u; the host walks to (%.0f,%.0f,%.0f), %.0f cm before the target", ArmName(),
            te, ie, static_cast<double>(g_hGoal.X), static_cast<double>(g_hGoal.Y), static_cast<double>(g_hGoal.Z),
            static_cast<double>(kStandFromTargetCm));
    HNext(HStep::Walk);
}

void StepHWalk() {
    const int st = g_hWalk ? g_hWalk->state.load() : 2;
    if (st == 0) {
        if (HExpired(kWalkBoundMs)) HFail("the walk to the standpoint never ended");
        return;
    }
    g_hWalk.reset();
    void* player = coop::players::Registry::Get().Local();
    void* item = g_hItem.Get();
    if (st != 1 || !player || !item || !E::TryGetActorLocation(player, g_hStand)) {
        HFail("the walk to the standpoint did not arrive");
        return;
    }
    g_hGrab = std::make_unique<DR::AimedGrab>(player, item);
    HNext(HStep::Grab);
}

void StepHGrab() {
    switch (g_hGrab->Tick()) {
    case DR::GrabState::Working:
        if (HExpired(kGrabBoundMs)) HFail("the grab never took within %llu s", U(kGrabBoundMs / 1000));
        return;
    case DR::GrabState::Failed:
        HFail("the grab did not take: %s, chain pre/use/post %d/%d/%d", g_hGrab->Why(), g_hGrab->ChainPre() ? 1 : 0,
              g_hGrab->ChainUse() ? 1 : 0, g_hGrab->ChainPost() ? 1 : 0);
        return;
    case DR::GrabState::Grabbed:
        UE_LOGI("[CI-DRILL] host %s: the drive is grabbed after %d aim pose(s)", ArmName(), g_hGrab->AimPoses());
        g_hGrab.reset();
        g_hAimer.Begin(1);
        HNext(HStep::Aim);
        return;
    }
}

void StepHAim() {
    void* player = coop::players::Registry::Get().Local();
    char why[160];
    switch (g_hAimer.Tick(player, g_hTarget.Get(), g_hStand, why, sizeof(why))) {
    case Aimer::State::Working: return;
    case Aimer::State::Failed:  HFail("%s", why); return;
    case Aimer::State::Aimed:
        DC::Say(kHgGo);   // the client starts its trace, and answers; the grab is held meanwhile
        HNext(HStep::WaitTracing);
        return;
    }
}

void StepHWaitTracing() {
    void* player = coop::players::Registry::Get().Local();
    void* item = g_hItem.Get();
    if (!DC::ChatHasLive(kHgTracing)) {
        if (HExpired(kChatBoundMs)) HFail("the client never said it traces within %llu s", U(kChatBoundMs / 1000));
        return;
    }
    if (!player || !item || DR::Grabbing(player) != item) {
        HFail("the grab was lost before the throw");
        return;
    }
    if (!HitWatchLive()) {
        HFail("the hit watch on the drive's component-hit handler is not live");
        return;
    }
    SeenBegin(g_hSeen, "host", item, /*releaseKnown=*/true);
    HitWatchBegin(item);   // before the release, so a hit at the release point is recorded
    char why[160];
    if (!ThrowGrabBranch(player, item, 1, why, sizeof(why))) {
        HFail("%s", why);
        return;
    }
    UE_LOGI("[CI-DRILL] host %s throw 1 press t=%llu via=traceThrow+throwShit+dropGrabObject target holds %d record(s)", ArmName(),
            U(g_hSeen.pressMs), g_hSeen.count0);
    HNext(HStep::Flight);
}

}  // namespace

void HostGrabHostTick(const coop::net::Session& session) {
    if (!g_hMs) {   // the first step's clock starts when the arm does, never at reset
        if (!cen::IsEnabled()) {
            HFail("the arm reads the census's overlap watch: run with container_route_census=1 on both peers");
            return;
        }
        HitWatchInstall();
        HNext(HStep::WaitPeer);
    }
    if (g_hStep == HStep::Done) return;
    const uint64_t now = ::GetTickCount64();
    const bool due = now >= g_hNextMs;
    if (due) {
        g_hNextMs = now + RD::kCheckEveryMs;
        if (g_hStep != HStep::WaitPeer && DC::ChatHasLive(kHgClientFailed)) {
            Fail("the client ended the arm with a FAIL");
            HNext(HStep::Done);
            return;
        }
    }
    // The grab, the aim and the flight are read every tick; the rest four times a second.
    switch (g_hStep) {
    case HStep::Grab:   StepHGrab(); return;
    case HStep::Aim:    StepHAim(); return;
    case HStep::Flight:
        if (SeenTick(g_hSeen, session.rttMsForSlot(1))) {
            SeenEnd(g_hSeen);
            SayInstants(g_hSeen.track, g_hSeen.pressMs, "host", 1);   // the client sets its mirror against them
            DC::Say(kHgEnd);
            HNext(HStep::WaitDone);
        }
        return;
    default: break;
    }
    if (!due) return;
    switch (g_hStep) {
    case HStep::WaitPeer:    StepHWaitPeer(session); return;
    case HStep::Bind:        StepHBind(); return;
    case HStep::Walk:        StepHWalk(); return;
    case HStep::WaitTracing: StepHWaitTracing(); return;
    case HStep::WaitDone:
        if (DC::ChatHasLive(kHgDone)) {
            UE_LOGI("[CI-DRILL] host %s MEASURED", ArmName());
            Finish();
            HNext(HStep::Done);
        } else if (HExpired(kChatBoundMs)) {
            HFail("the client never said it was done within %llu s", U(kChatBoundMs / 1000));
        }
        return;
    default: return;
    }
}

// ---- the client: the observer -------------------------------------------------------------------------------------------

namespace {

enum class CStep : uint8_t { WaitLine, WaitMirrors, WaitGo, Flight, WaitHostEnd, Done };
CStep    g_cStep = CStep::WaitLine;
uint64_t g_cMs = 0, g_cNextMs = 0;
uint32_t g_cItemEid = 0;
uint32_t g_cRel0 = 0, g_cWhoosh0 = 0;   // the throw releases received and the swings played, at the go
constexpr int kHgMatchedMin = 3;       // fewer matched instants than this is no judgement of the mirror
ue_wrap::CachedObjRef g_cItem;
Seen     g_cSeen;

void CNext(CStep s) { g_cStep = s; g_cMs = ::GetTickCount64(); }
bool CExpired(uint64_t bound) { return ::GetTickCount64() - g_cMs > bound; }

void CFail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    Failed(kHgClientFailed, fmt, ap);
    va_end(ap);
    CNext(CStep::Done);
}

}  // namespace

void HostGrabClientTick(const coop::net::Session& session) {
    if (!g_cMs) {   // the first step's clock starts when the arm does, never at reset
        if (!cen::IsEnabled()) {
            CFail("the arm reads the census's overlap watch: run with container_route_census=1 on both peers");
            return;
        }
        HitWatchInstall();
        CNext(CStep::WaitLine);
    }
    if (g_cStep == CStep::Done) return;
    if (g_cStep == CStep::Flight) {   // every tick
        const int rtt = session.rttMsForSlot(0);
        if (SeenTick(g_cSeen, rtt)) {
            SeenEnd(g_cSeen);
            CNext(CStep::WaitHostEnd);   // the host's instants come before its end line: the mirror is judged against them
        }
        return;
    }
    const uint64_t now = ::GetTickCount64();
    if (now < g_cNextMs) return;
    g_cNextMs = now + RD::kCheckEveryMs;
    if (DC::ChatHasLive(kHgHostFailed)) {
        CFail("the host ended the arm with a FAIL");
        return;
    }
    std::string rest;
    switch (g_cStep) {
    case CStep::WaitLine: {
        int pack = 0, item = 0;
        if (DC::LiveRestOf(kHgFixture, &rest)) {
            if (!RD::IntAfter(rest, "pack", pack) || !RD::IntAfter(rest, "item", item) || pack <= 0 || item <= 0) {
                CFail("the host's fixture line does not read (%s)", rest.c_str());
                return;
            }
            g_cSeen.packEid = static_cast<uint32_t>(pack);
            g_cItemEid = static_cast<uint32_t>(item);
            CNext(CStep::WaitMirrors);
        } else if (CExpired(kPeerBoundMs)) {
            CFail("the host never said its fixture within %llu s", U(kPeerBoundMs / 1000));
        }
        return;
    }
    case CStep::WaitMirrors: {
        void* pack = coop::element::LivePropActor(g_cSeen.packEid);
        void* item = coop::element::LivePropActor(g_cItemEid);
        if (pack && item) {
            g_cItem.Set(item);
            UE_LOGI("[CI-DRILL] client %s: the fixture's mirrors are here (target eid=%u, drive eid=%u)", ArmName(), g_cSeen.packEid,
                    g_cItemEid);
            CNext(CStep::WaitGo);
        } else if (CExpired(kPeerBoundMs)) {
            CFail("the fixture's mirrors never came (target %p, drive %p)", pack, item);
        }
        return;
    }
    case CStep::WaitHostEnd: {
        if (!DC::ChatHasLive(kHgEnd)) {
            if (CExpired(kChatBoundMs)) CFail("the host never said its end within %llu s", U(kChatBoundMs / 1000));
            return;
        }
        // The host's throw reached this peer: its release carried a throw sequence (the host's own throwShit watch minted it)
        // and the swing played, and the mirror followed the host's copy within the flight bound.
        const uint32_t rel = coop::remote_prop::dev::ThrowReleasesReceived() - g_cRel0;
        const uint32_t whoosh = coop::remote_prop::dev::ThrowWhooshesPlayed() - g_cWhoosh0;
        const Gap g = JudgeInstants(g_cSeen.track, "host", 1);
        UE_LOGI("[CI-DRILL] client %s: the host's release reached this peer with a throw sequence on %u message(s), %u swing(s) played; "
                "mirror-gap=%.0f cm over %d of %d instants (bound %.0f, PROVISIONAL)", ArmName(), rel, whoosh, static_cast<double>(g.worst),
                g.matched, g.said, static_cast<double>(kFlightGapPassCm));
        if (rel == 0) {
            CFail("the host's throw reached this client with throwSeq 0 (no PropRelease carried a sequence)");
            return;
        }
        if (whoosh == 0) {
            CFail("no throw swing played on this client for the host's throw");
            return;
        }
        if (g.matched >= kHgMatchedMin && g.worst > kFlightGapPassCm) {
            CFail("the client's mirror was %.0f cm from the host's copy at k=%d, past %.0f cm", static_cast<double>(g.worst), g.worstK,
                  static_cast<double>(kFlightGapPassCm));
            return;
        }
        char gapText[48] = "n/a (too few instants matched)";
        if (g.matched >= kHgMatchedMin) std::snprintf(gapText, sizeof(gapText), "%.0f cm", static_cast<double>(g.worst));
        UE_LOGI("[CI-DRILL] client %s PASS mirror-gap=%s", ArmName(), gapText);
        DC::Say(kHgDone);
        UE_LOGI("[CI-DRILL] client %s MEASURED", ArmName());
        Finish();
        CNext(CStep::Done);
        return;
    }
    case CStep::WaitGo: {
        void* item = g_cItem.Get();
        if (!item) {
            CFail("the drive's mirror died");
            return;
        }
        if (DC::ChatHasLive(kHgGo)) {
            g_cRel0 = coop::remote_prop::dev::ThrowReleasesReceived();
            g_cWhoosh0 = coop::remote_prop::dev::ThrowWhooshesPlayed();
            SeenBegin(g_cSeen, "client", item, /*releaseKnown=*/false);
            HitWatchBegin(item);
            if (!HitWatchLive()) {
                CFail("the hit watch on the drive's component-hit handler is not live");
                return;
            }
            DC::Say(kHgTracing);
            CNext(CStep::Flight);
        } else if (CExpired(kClientTraceBoundMs)) {
            CFail("the host never said it was aimed within %llu s", U(kClientTraceBoundMs / 1000));
        }
        return;
    }
    default: return;
    }
}

void HostGrabReset() {
    if (g_hWalk && g_hWalk->state.load() == 0) DR::CancelBackgroundWalk(*g_hWalk);
    g_hWalk.reset();
    g_hGrab.reset();
    g_hStep = HStep::WaitPeer;
    g_hMs = g_hNextMs = 0;
    g_hTarget.Reset();
    g_hItem.Reset();
    g_hTargetEid = g_hItemEid = 0;
    g_hStart = g_hPack = g_hStand = g_hGoal = FVector{};
    g_hAimer = Aimer();
    g_hSeen = Seen{};
    g_cStep = CStep::WaitLine;
    g_cMs = g_cNextMs = 0;
    g_cItemEid = 0;
    g_cRel0 = g_cWhoosh0 = 0;
    g_cItem.Reset();
    g_cSeen = Seen{};
}

}  // namespace coop::dev::container_intent_drill::detail

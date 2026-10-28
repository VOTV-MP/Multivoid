// coop/dev/container_intent_drill_throw_host.cpp -- the HOST's half of the container intent drill's throw arms; see
// container_intent_drill_throw.cpp for the arms and container_intent_drill_throw.h for what they measure. It spawns the
// fixture beside the client's puppet (a backpack 600 cm ahead, a drive beside the client's standpoint, 350 cm before it), says
// the element ids on drill chat, and for each throw: takes its set of drives when the client is armed (throwhand) or names the
// grabbed element (throwgrab), traces the host's element of the thrown drive, and prints the verdict once the client has
// said what it saw. The verdict line:
//   throw <arm> n=<k> host-arc=<flew|fell|none> client-arc=<flew|fell|none> host-overlap=<n> client-overlap=<n>
//   host+<k> client-mirror+<k> final-gap=<cm> lag-at-peak=<ms> speed-at-peak=<cm/s>
// host+k and client-mirror+k are the target's record count gained on each peer; final-gap the distance between the two
// peers' final poses (-1 when a copy is gone); speed-at-peak the client's largest speed; lag-at-peak the host's clock at the
// sample of its own element nearest the client's peak pose, less the client's clock there (-1 with no sample).

#include "container_intent_drill_throw.h"
#include "container_route_drill_detail.h"

#include "coop/dev/container_route_census.h"
#include "coop/dev/drill_chat.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/actors/container_inventory.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace coop::dev::container_intent_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace ci  = ue_wrap::container_inventory;
namespace DC  = coop::dev::drill_chat;
namespace RD  = coop::dev::container_route_drill::detail;
namespace cen = coop::dev::container_route_census;
using ue_wrap::FVector;

constexpr int      kR4Route        = 3;       // container_route_census::RouteVerbs: R1..R6 are 0..5
constexpr float    kTargetAheadCm  = 600.f;   // the target, before the client's puppet: the client must walk
constexpr uint64_t kHostBindMs     = 15000;   // the host's spawn to its ids and first publications
constexpr uint64_t kArmBoundMs     = 240000;  // the fixture line to the client's arm line (its walk, its hand or grab, the chat)
constexpr uint64_t kEndBoundMs     = 120000;  // the arm line to the client's end line
constexpr uint64_t kNextBoundMs    = 240000;  // the verdict to the client's second or done line (a second walk)
constexpr uint64_t kFindEveryMs    = 50;      // the search for the host's element of a thrown hand drive

enum class HStep : uint8_t { WaitPeer, Bind, WaitArm, Trace, WaitNext, Item2Bind, Done };
HStep    g_hStep = HStep::WaitPeer;
uint64_t g_hMs = 0;        // the time the step was entered
uint64_t g_hNextMs = 0;    // the chat is read four times a second; the trace every tick
uint64_t g_hFindMs = 0;
int      g_hN = 1;         // the throw in hand
int      g_hSlot = 0;
ue_wrap::CachedObjRef g_hTarget, g_hItem;
uint32_t g_hTargetEid = 0, g_hItemEid = 0;
Track    g_hTrack;
std::vector<uint32_t> g_hSet, g_hNow;   // the live drive element ids at the arm line; the ones live now
std::vector<coop::element::Registry::ActorIdPair> g_hSnap;
uint32_t g_hElemEid = 0;   // the host's element of the thrown drive (0 until found)
int      g_hCount0 = -1;
uint64_t g_hOvSeen = 0, g_hOvAll0 = 0, g_hAdds0 = 0;
int      g_hOvCount = 0;

void HNext(HStep s) { g_hStep = s; g_hMs = ::GetTickCount64(); }
bool HExpired(uint64_t bound) { return ::GetTickCount64() - g_hMs > bound; }
unsigned long long U(uint64_t v) { return static_cast<unsigned long long>(v); }

bool EidBound(uint32_t eid) { return eid != static_cast<uint32_t>(coop::element::kInvalidId); }

// The host's end with the FAIL line; the client is told so it does not wait out its bounds.
void HFail(const char* fmt, ...) {
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    DC::Say(kThrowHostFailedLine);
    Fail("%s", buf);
    HNext(HStep::Done);
}

FVector Place(const FVector& at, const FVector& fwd, float ahead, float side) {
    return {at.X + fwd.X * ahead + fwd.Y * side, at.Y + fwd.Y * ahead - fwd.X * side, at.Z + RD::kDropCm};
}

// The target 600 cm ahead of the client's puppet and the drive beside the standpoint, 350 cm before the target.
bool SpawnFixture(const FVector& at, const FVector& fwd) {
    void* targetCls = ue_wrap::asset_load::LoadObjectByPath(RD::kPackPath);
    void* itemCls = R::FindClass(RD::kDriveClass);
    if (!targetCls || !itemCls) {
        HFail("the fixture's class does not load (target %p, drive %p)", targetCls, itemCls);
        return false;
    }
    void* target = E::SpawnActor(targetCls, Place(at, fwd, kTargetAheadCm, 0.f));
    if (!target || !ci::IsContainer(target)) {
        HFail("the target did not spawn as a container");
        return false;
    }
    g_hTarget.Set(target);
    void* item = E::SpawnActor(itemCls, Place(at, fwd, kTargetAheadCm - kStandFromTargetCm, kItemSideCm));
    if (!item) {
        HFail("the drive did not spawn");
        return false;
    }
    g_hItem.Set(item);
    return true;
}

void StepWaitPeer(const coop::net::Session& s) {
    int slot = 0;
    for (int i = 1; i < coop::net::kMaxPeers && !slot; ++i)
        if (s.IsSlotWorldReady(i)) slot = i;
    if (!slot) return;
    void* puppet = coop::puppet_drive::Puppet(slot).GetActor();   // spawned once the peer's pose arrives
    FVector at{};
    if (!puppet || !R::IsLive(puppet) || !E::TryGetActorLocation(puppet, at)) return;
    FVector fwd = E::GetActorForwardVector(puppet);
    if (fwd.X == 0.f && fwd.Y == 0.f) fwd = {1.f, 0.f, 0.f};
    const float len = std::hypot(fwd.X, fwd.Y);
    fwd = {fwd.X / len, fwd.Y / len, 0.f};
    g_hSlot = slot;
    if (!SpawnFixture(at, fwd)) return;
    HNext(HStep::Bind);
}

void StepBind() {
    void* target = g_hTarget.Get();
    void* item = g_hItem.Get();
    if (!target || !item) {
        HFail("a fixture actor died before the client took it");
        return;
    }
    const uint32_t te = RD::EidOf(target), ie = RD::EidOf(item);
    const int count = EidBound(te) && EidBound(ie) ? CountOf(te) : -1;
    if (count < 0) {
        if (HExpired(kHostBindMs)) HFail("the fixture was never bound and published (target eid %u, drive eid %u)", te, ie);
        return;
    }
    g_hTargetEid = te;
    g_hItemEid = ie;
    char say[64];
    std::snprintf(say, sizeof(say), "%spack=%u item=%u", kThrowFixtureLine, te, ie);
    DC::Say(say);
    UE_LOGI("[CI-DRILL] host %s fixture: target eid=%u %ls holds %d record(s), drive eid=%u", ArmName(), te,
            R::ClassNameOf(target).c_str(), count, ie);
    HNext(HStep::WaitArm);
}

// The live prop_drive_C element ids now, in `out`.
void LiveDriveEids(std::vector<uint32_t>& out) {
    out.clear();
    void* driveCls = R::FindClass(RD::kDriveClass);
    coop::element::Registry::Get().SnapshotActorsByType(coop::element::ElementType::Prop, g_hSnap);
    for (const auto& p : g_hSnap) {
        void* a = coop::element::LivePropActor(p.id);   // null when the element's actor is gone
        if (a && driveCls && R::ClassOf(a) == driveCls) out.push_back(p.id);
    }
}

// The host's element of the client's thrown hand drive: the first prop_drive_C element not in the set taken at the arm line.
void FindThrown() {
    LiveDriveEids(g_hNow);
    uint32_t best = static_cast<uint32_t>(coop::element::kInvalidId);
    for (uint32_t id : g_hNow)
        if (id < best && std::find(g_hSet.begin(), g_hSet.end(), id) == g_hSet.end()) best = id;
    if (!EidBound(best)) return;
    g_hElemEid = best;
    TrackSet(g_hTrack, coop::element::LivePropActor(best));
    char say[64];
    std::snprintf(say, sizeof(say), "%seid=%u", ThrowLine("host", "thrown", g_hN, true).c_str(), best);
    DC::Say(say);
    UE_LOGI("[CI-DRILL] host %s throw %d: the drop path made a drive here, eid=%u (not among the %zu armed)", ArmName(), g_hN,
            best, g_hSet.size());
}

void StepWaitArm() {
    std::string rest;
    if (!DC::LiveRestOf(ThrowLine("client", "arm", g_hN, true), &rest)) {
        if (HExpired(kArmBoundMs)) HFail("the client never armed throw %d within %llu s", g_hN, U(kArmBoundMs / 1000));
        return;
    }
    const char* dead = nullptr;
    if (cen::SeamsState(&dead) != cen::Seams::Live) {
        HFail("the census's watches are not all live (%s)", dead ? dead : "pending");
        return;
    }
    int held = 0;
    RD::IntAfter(rest, "held", held);
    TrackBegin(g_hTrack, "host", "element");
    g_hElemEid = 0;
    if (ArmOf() == Arm::ThrowGrab) {   // the element the client holds is the one thrown
        void* actor = held > 0 ? coop::element::LivePropActor(static_cast<uint32_t>(held)) : nullptr;
        if (!actor) {
            HFail("the grabbed drive has no live element here (eid %d)", held);
            return;
        }
        g_hElemEid = static_cast<uint32_t>(held);
        TrackSet(g_hTrack, actor);
    } else {
        LiveDriveEids(g_hSet);
    }
    g_hCount0 = CountOf(g_hTargetEid);
    g_hOvSeen = cen::LastR4Overlap().seq;
    g_hOvAll0 = cen::R4Overlaps();
    g_hAdds0 = cen::RouteVerbs(kR4Route, /*take=*/false);
    g_hOvCount = 0;
    DC::Say(ThrowLine("host", "armed", g_hN, false));
    UE_LOGI("[CI-DRILL] host %s throw %d armed: target holds %d record(s), %zu drive element(s) live, held eid=%d", ArmName(),
            g_hN, g_hCount0, g_hSet.size(), held);
    HNext(HStep::Trace);
}

// An overlap of the target by the host's element, whether or not addObject then ran.
void PollOverlap() {
    const cen::R4Overlap o = cen::LastR4Overlap();
    if (o.seq == g_hOvSeen) return;
    g_hOvSeen = o.seq;
    if (o.container != g_hTargetEid || !o.actor || o.actor != g_hTrack.ref.Raw()) return;
    ++g_hOvCount;
    UE_LOGI("[CI-DRILL] throw host overlap eid=%u sim=%d t=%llu", o.eid, o.sim ? 1 : 0, U(o.t));
}

// The words of the client's end line (arc=, gone=, ov=, all=, mir=, rest=x,y,z) and peak line (t= v= p=x,y,z).
struct ClientEnd {
    char arc[8] = "?";
    int gone = 0, ov = 0, all = 0, mir = 0;
    int rest[3] = {};
    unsigned long long peakT = 0;
    int peakV = 0;
    int peakP[3] = {};
    bool peakRead = false;
};

// The `n` comma-separated integers after `key` (with its '=') in `text`; false when the key is absent or the list short.
bool IntsAfter(const std::string& text, const char* key, int* out, int n) {
    const size_t at = text.find(key);
    if (at == std::string::npos) return false;
    const char* p = text.c_str() + at + std::strlen(key);
    for (int i = 0; i < n; ++i) {
        char* end = nullptr;
        out[i] = static_cast<int>(std::strtol(p, &end, 10));
        if (end == p) return false;
        p = end;
        if (i + 1 < n) {
            if (*p != ',') return false;
            ++p;
        }
    }
    return true;
}

ClientEnd ReadClientEnd(const std::string& endRest) {
    ClientEnd c;
    const size_t arc = endRest.find("arc=");
    if (arc != std::string::npos) {
        const std::string word = endRest.substr(arc + 4, endRest.find(' ', arc) == std::string::npos ? std::string::npos
                                                                                                     : endRest.find(' ', arc) - arc - 4);
        std::snprintf(c.arc, sizeof(c.arc), "%s", word.c_str());
    }
    RD::IntAfter(endRest, "gone", c.gone);
    RD::IntAfter(endRest, "ov", c.ov);
    RD::IntAfter(endRest, "all", c.all);
    RD::IntAfter(endRest, "mir", c.mir);
    IntsAfter(endRest, "rest=", c.rest, 3);
    std::string pk;
    if (DC::LiveRestOf(ThrowLine("client", "peak", g_hN, true), &pk)) {
        const size_t t = pk.find("t=");
        if (t != std::string::npos) c.peakT = std::strtoull(pk.c_str() + t + 2, nullptr, 10);
        c.peakRead = t != std::string::npos && RD::IntAfter(pk, "v", c.peakV) && IntsAfter(pk, "p=", c.peakP, 3);
    }
    return c;
}

void Verdict(const std::string& endRest) {
    const ClientEnd c = ReadClientEnd(endRest);
    const int count1 = CountOf(g_hTargetEid);
    const FVector clientRest{static_cast<float>(c.rest[0]), static_cast<float>(c.rest[1]), static_cast<float>(c.rest[2])};
    float gap = -1.f;
    if (!g_hTrack.samples.empty() && !g_hTrack.gone && !c.gone) gap = Dist3(g_hTrack.last, clientRest);
    long long lag = -1;
    float minGap = -1.f;
    if (c.peakRead && !g_hTrack.samples.empty()) {
        const FVector peak{static_cast<float>(c.peakP[0]), static_cast<float>(c.peakP[1]), static_cast<float>(c.peakP[2])};
        if (const Sample* s = NearestSample(g_hTrack, peak, minGap))
            lag = static_cast<long long>(s->t) - static_cast<long long>(c.peakT);
    }
    UE_LOGI("[CI-DRILL] host throw %s n=%d host-arc=%s client-arc=%s host-overlap=%d client-overlap=%d host%+d client-mirror%+d "
            "final-gap=%.0f lag-at-peak=%lld speed-at-peak=%d", ArmName(), g_hN, ArcOf(g_hTrack), c.arc, g_hOvCount, c.ov,
            count1 - g_hCount0, c.mir, static_cast<double>(gap), lag, c.peakV);
    UE_LOGI("[CI-DRILL] host throw %s n=%d detail: host eid=%u gone=%d client gone=%d host peak %.0f cm/s, nearest host sample to "
            "the client's peak %.0f cm away, census overlaps host %llu client %d, addObject on the host %llu, target %d -> %d",
            ArmName(), g_hN, g_hElemEid, g_hTrack.gone ? 1 : 0, c.gone, static_cast<double>(g_hTrack.peakSpeed),
            static_cast<double>(minGap), U(cen::R4Overlaps() - g_hOvAll0), c.all, U(cen::RouteVerbs(kR4Route, false) - g_hAdds0),
            g_hCount0, count1);
    g_hTrack.on = false;
    HNext(HStep::WaitNext);
}

void StepTrace(const coop::net::Session& s, bool due) {
    if (!g_hTrack.on && ArmOf() == Arm::ThrowHand) {
        const uint64_t now = ::GetTickCount64();
        if (now >= g_hFindMs) {
            g_hFindMs = now + kFindEveryMs;
            FindThrown();
        }
    }
    TrackTick(g_hTrack, s.rttMsForSlot(g_hSlot));
    PollOverlap();
    if (!due) return;
    std::string rest;
    if (DC::LiveRestOf(ThrowLine("client", "end", g_hN, true), &rest)) {
        Verdict(rest);
        return;
    }
    if (HExpired(kEndBoundMs)) HFail("the client never ended throw %d within %llu s", g_hN, U(kEndBoundMs / 1000));
}

// The second drive, beside the second standpoint the client names: throwgrab's client grabs it.
bool SpawnItem2(const FVector& stand) {
    void* target = g_hTarget.Get();
    FVector at{};
    void* cls = R::FindClass(RD::kDriveClass);
    if (!target || !cls || !E::TryGetActorLocation(target, at)) {
        HFail("the second drive's place does not read (target %p, class %p)", target, cls);
        return false;
    }
    float ux = at.X - stand.X, uy = at.Y - stand.Y;
    const float len = std::hypot(ux, uy);
    if (len < 1.f) {
        HFail("the second standpoint is on the target");
        return false;
    }
    const FVector fwd{ux / len, uy / len, 0.f};
    void* item = E::SpawnActor(cls, Place(stand, fwd, 0.f, kItemSideCm));
    if (!item) {
        HFail("the second drive did not spawn");
        return false;
    }
    g_hItem.Set(item);
    return true;
}

void StepWaitNext() {
    if (DC::ChatHasLive(kThrowDoneLine)) {
        UE_LOGI("[CI-DRILL] host %s MEASURED", ArmName());
        Finish();
        HNext(HStep::Done);
        return;
    }
    std::string rest;
    int at[3] = {};
    if (g_hN == 1 && DC::LiveRestOf(kThrowSecondLine, &rest)) {
        if (!IntsAfter(rest, "at=", at, 3)) {
            HFail("the client's second standpoint does not read (%s)", rest.c_str());
            return;
        }
        g_hN = 2;
        if (ArmOf() == Arm::ThrowHand) {
            HNext(HStep::WaitArm);
            return;
        }
        if (!SpawnItem2(FVector{static_cast<float>(at[0]), static_cast<float>(at[1]), static_cast<float>(at[2])})) return;
        HNext(HStep::Item2Bind);
        return;
    }
    if (HExpired(kNextBoundMs)) HFail("the client said neither a second throw nor its end within %llu s", U(kNextBoundMs / 1000));
}

void StepItem2Bind() {
    void* item = g_hItem.Get();
    if (!item) {
        HFail("the second drive died before the client took it");
        return;
    }
    const uint32_t ie = RD::EidOf(item);
    if (!EidBound(ie)) {
        if (HExpired(kHostBindMs)) HFail("the second drive was never bound");
        return;
    }
    char say[48];
    std::snprintf(say, sizeof(say), "%seid=%u", kThrowItem2Line, ie);
    DC::Say(say);
    UE_LOGI("[CI-DRILL] host %s: the second drive is eid=%u", ArmName(), ie);
    HNext(HStep::WaitArm);
}

}  // namespace

void ThrowHostTick(const coop::net::Session& session) {
    if (!g_hMs) {   // the first step's clock starts when the arm does, never at reset
        if (!cen::IsEnabled()) {
            HFail("the arm reads the census's overlap watch: run with container_route_census=1 on both peers");
            return;
        }
        HNext(HStep::WaitPeer);
    }
    if (g_hStep == HStep::Done) return;
    const uint64_t now = ::GetTickCount64();
    const bool due = now >= g_hNextMs;
    if (due) {
        g_hNextMs = now + RD::kCheckEveryMs;
        if (g_hStep != HStep::WaitPeer && DC::ChatHasLive(kThrowClientFailedLine)) {
            Fail("the client ended the arm with a FAIL");
            HNext(HStep::Done);
            return;
        }
    }
    if (g_hStep == HStep::Trace) {
        StepTrace(session, due);
        return;
    }
    if (!due) return;
    switch (g_hStep) {
    case HStep::WaitPeer:  StepWaitPeer(session); return;
    case HStep::Bind:      StepBind(); return;
    case HStep::WaitArm:   StepWaitArm(); return;
    case HStep::WaitNext:  StepWaitNext(); return;
    case HStep::Item2Bind: StepItem2Bind(); return;
    default:               return;
    }
}

void ThrowHostReset() {
    g_hStep = HStep::WaitPeer;
    g_hMs = g_hNextMs = g_hFindMs = 0;
    g_hN = 1;
    g_hSlot = 0;
    g_hTarget.Reset();
    g_hItem.Reset();
    g_hTargetEid = g_hItemEid = 0;
    g_hTrack = Track{};
    g_hSet.clear();
    g_hNow.clear();
    g_hElemEid = 0;
    g_hCount0 = -1;
    g_hOvSeen = g_hOvAll0 = g_hAdds0 = 0;
    g_hOvCount = 0;
}

}  // namespace coop::dev::container_intent_drill::detail

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

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/container_route_census.h"
#include "coop/dev/drill_chat.h"
#include "coop/net/throw_bounds.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_drop_intent.h"
#include "coop/props/prop_syncer_host.h"
#include "coop/props/remote_prop.h"
#include "coop/props/prop_throw.h"

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
constexpr int      kParkedSamplesMax = 3;     // PROVISIONAL: the thrower's samples before rest at which it may read not-simulating
constexpr uint64_t kJoinerEndMs    = 150000;  // throwjoin: the first throw's verdict to the joiner's end line
constexpr uint64_t kRejoinWaitMs   = 240000;  // throwdepart: the take-back to the rejoined client's seed line

enum class HStep : uint8_t { WaitPeer, Bind, WaitArm, Trace, WaitNext, Item2Bind, JoinWait, Done };
HStep    g_hStep = HStep::WaitPeer;
enum class Touch : uint8_t { Pending, Passed, NotMeasured };   // throwtouch: the host's grab of the flying prop
Touch    g_hTouch = Touch::Pending;
uint64_t g_hPlacedMs = 0;   // throwplace: the client said it placed
uint32_t g_hRevokes0 = 0;   // the host's trust revocations at the arm line
uint64_t g_hMs = 0;        // the time the step was entered
uint64_t g_hNextMs = 0;    // the chat is read four times a second; the trace every tick
uint64_t g_hFindMs = 0;
int      g_hN = 1;         // the throw in hand
int      g_hSlot = 0;
ue_wrap::CachedObjRef g_hTarget, g_hItem;
uint32_t g_hTargetEid = 0, g_hItemEid = 0;
Track    g_hTrack;
HitJudge g_hHit;
std::vector<uint32_t> g_hSet, g_hNow;   // the live drive element ids at the arm line; the ones live now
std::vector<coop::element::Registry::ActorIdPair> g_hSnap;
uint32_t g_hElemEid = 0;   // the host's element of the thrown drive (0 until found)
int      g_hCount0 = -1;
uint64_t g_hOvSeen = 0, g_hOvAll0 = 0, g_hAdds0 = 0;
int      g_hOvCount = 0;
bool     g_hArmRead = false;        // the client's arm line, read once: it is kept, never read again from the feed
std::string g_hArmRest;
bool     g_hGateLogged = false;
Instants g_hInst;                   // the thrower's instants, collected part by part
std::string g_hEndRest;             // the thrower's end line, kept when first seen (its row may retire while parts are awaited)
uint64_t g_hEndSeenMs = 0;
uint32_t g_hDrop0 = 0, g_hRel0 = 0;  // the launches the host applied (drop intent spawns, grab releases) at the arm line
char     g_hLaunch[48] = "launch=n/a";

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
    void* actor = coop::element::LivePropActor(best);
    TrackSet(g_hTrack, actor);
    HitWatchBegin(actor);
    g_hHit.Begin(/*releaseKnown=*/true);
    char say[64];
    std::snprintf(say, sizeof(say), "%seid=%u", ThrowLine("host", "thrown", g_hN, true).c_str(), best);
    DC::Say(say);
    UE_LOGI("[CI-DRILL] host %s throw %d: the drop path made a drive here, eid=%u (not among the %zu armed)", ArmName(), g_hN,
            best, g_hSet.size());
}

void StepWaitArm(const coop::net::Session& s) {
    // The arm line is kept when first read: the wait for a second client (throwjoin) can outlast the feed's row.
    if (!g_hArmRead) {
        if (!DC::SeenRestOf(ThrowLine("client", "arm", g_hN, true), &g_hArmRest)) {
            if (HExpired(kArmBoundMs)) HFail("the client never armed throw %d within %llu s", g_hN, U(kArmBoundMs / 1000));
            return;
        }
        g_hArmRead = true;
    }
    const std::string& rest = g_hArmRest;
    const char* dead = nullptr;
    if (cen::SeamsState(&dead) != cen::Seams::Live) {
        HFail("the census's watches are not all live (%s)", dead ? dead : "pending");
        return;
    }
    if (!HitWatchLive()) {
        HFail("the hit watch on the drive's component-hit handler is not live");
        return;
    }
    // throwjoin: the go waits for the hold to catch the second client's world-ready ask, so the throw flies while it joins.
    if (ArmOf() == Arm::ThrowJoin) {
        char gateWhy[200];
        switch (JoinGate(s, ::GetTickCount64() - g_hMs, gateWhy, sizeof(gateWhy))) {
        case HostJudge::Failed: HFail("%s", gateWhy); return;
        case HostJudge::Working:
            if (!g_hGateLogged) {
                g_hGateLogged = true;
                UE_LOGI("[CI-DRILL] host throwjoin: the client is armed; waiting for the second client to ask for world-ready (slot %d threw)", g_hSlot);
            }
            return;
        default: break;
        }
    }
    ArmsHostArmed();
    g_hArmRead = false;
    g_hInst = Instants{};
    g_hEndRest.clear();
    g_hEndSeenMs = 0;
    g_hDrop0 = coop::prop_drop_intent::dev::LaunchesApplied();
    g_hRel0 = coop::remote_prop::dev::ReleaseLaunchesApplied();
    g_hRevokes0 = coop::prop_syncer_host::dev::ThrowViewNow().revokes;
    if (ArmOf() == Arm::ThrowPlace) {   // a place is judged on what the host receives, not on a flight: no trace
        g_hPlacedMs = 0;
        DC::Say(ThrowLine("host", "armed", g_hN, false));
        UE_LOGI("[CI-DRILL] host %s part %d armed", ArmName(), g_hN);
        HNext(HStep::Trace);
        return;
    }
    int held = 0;
    RD::IntAfter(rest, "held", held);
    g_hTouch = Touch::Pending;
    TrackBegin(g_hTrack, "host", "element");
    g_hElemEid = 0;
    if (!HandPath(ArmOf(), g_hN)) {   // a grab path: the element the client holds is the one thrown
        void* actor = held > 0 ? coop::element::LivePropActor(static_cast<uint32_t>(held)) : nullptr;
        if (!actor) {
            HFail("the grabbed drive has no live element here (eid %d)", held);
            return;
        }
        g_hElemEid = static_cast<uint32_t>(held);
        TrackSet(g_hTrack, actor);
        HitWatchBegin(actor);
        g_hHit.Begin(/*releaseKnown=*/false);
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

// The words of the client's end line (arc=, gone=, ov=, all=, mir=, rest=x,y,z, launch=, peakt= peakv= peakp=x,y,z).
struct ClientEnd {
    int launch = 0;                // the speed the throw gave the thrower's copy, cm/s
    char arc[8] = "?";
    int gone = 0, ov = 0, all = 0, mir = 0;
    int rest[3] = {};
    int park = 0;                  // samples at which the thrower's copy was not simulating before rest
    unsigned long long tmax = 0;   // press to rest on the thrower, ms
    unsigned long long peakT = 0;
    int peakV = 0;
    int peakP[3] = {};
    bool peakRead = false;
};


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
    RD::IntAfter(endRest, "park", c.park);
    if (const size_t tm = endRest.find("tmax="); tm != std::string::npos) c.tmax = std::strtoull(endRest.c_str() + tm + 5, nullptr, 10);
    RD::IntAfter(endRest, "launch", c.launch);
    const size_t pt = endRest.find("peakt=");
    if (pt != std::string::npos) c.peakT = std::strtoull(endRest.c_str() + pt + 6, nullptr, 10);
    c.peakRead = pt != std::string::npos && RD::IntAfter(endRest, "peakv", c.peakV) && IntsAfter(endRest, "peakp=", c.peakP, 3);
    return c;
}

// The launch the host gave its copy of the throw, set against the thrower's: the drop intent's spawn velocity on the hand
// path, the release's velocity on a grab path (the counters were taken at the arm line; none applied reads as 0). Every throw
// arm needs at least half of the thrower's own launch speed (capped at the throw bound), and throwclamp no more than the bound.
bool JudgeLaunch(const ClientEnd& c, char* why, size_t whyLen) {
    const bool hand = HandPath(ArmOf(), g_hN);
    const bool applied = hand ? coop::prop_drop_intent::dev::LaunchesApplied() > g_hDrop0
                              : coop::remote_prop::dev::ReleaseLaunchesApplied() > g_hRel0;
    const float a = !applied ? 0.f : hand ? coop::prop_drop_intent::dev::LastAppliedLaunch() : coop::remote_prop::dev::LastReleaseLaunch();
    const float bound = coop::net::throw_bounds::kMaxLaunchSpeedCmS;
    const float t = static_cast<float>(c.launch);
    std::snprintf(g_hLaunch, sizeof(g_hLaunch), "launch=%.0f/%.0f", static_cast<double>(a), static_cast<double>(t));
    UE_LOGI("[CI-DRILL] host throw %s n=%d launch: the host's copy was given %.0f cm/s (%s), the thrower threw at %.0f cm/s (bound %.0f)",
            ArmName(), g_hN, static_cast<double>(a), hand ? "drop intent spawn" : "release", static_cast<double>(t), static_cast<double>(bound));
    // A launch the thrower did not say (cut from its end line, or a copy that never moved) is no floor: it fails, so a red
    // cannot pass on a missing number.
    if (t < 1.f) {
        std::snprintf(why, whyLen, "the thrower's launch speed did not read (launch=%.0f)", static_cast<double>(t));
        return false;
    }
    if (a < 0.5f * (std::min)(t, bound)) {
        std::snprintf(why, whyLen, "the host's copy launched at %.0f cm/s where the thrower threw at %.0f cm/s", static_cast<double>(a),
                      static_cast<double>(t));
        return false;
    }
    if (ArmOf() == Arm::ThrowClamp && a > bound * 1.01f) {
        std::snprintf(why, whyLen, "the host's copy launched at %.0f cm/s, past the throw bound %.0f cm/s", static_cast<double>(a),
                      static_cast<double>(bound));
        return false;
    }
    return true;
}

// The flight, judged: the host's copy against the thrower's at the instants the client said (wall clock, the same machine
// on loopback), the thrower's copy never parked, and for throwclamp the host's copy no faster than the throw bound. Returns
// the FAIL text, or null for a pass; always prints the numbers the constants are derived from.
const char* JudgeFlight(const ClientEnd& c, char* why, size_t whyLen) {
    const Gap judged = JudgeInstants(g_hTrack, g_hInst);
    const int matched = judged.matched, said = judged.said, worstK = judged.worstK;
    const float worst = judged.worst;
    UE_LOGI("[CI-DRILL] host throw %s n=%d flight: matched=%d/%d instants, in-flight gap max=%.0f cm at k=%d (green bound %.0f cm, "
            "PROVISIONAL), thrower parked in flight=%d sample(s) (bound %d), release-to-rest=%llu ms (t_max), host peak %.0f cm/s "
            "(launch bound %.0f, flight bound %.0f), throws minted here=%u, puppet throwShit ignored n=%u",
            ArmName(), g_hN, matched, said, static_cast<double>(worst), worstK, static_cast<double>(kFlightGapPassCm), c.park,
            kParkedSamplesMax, c.tmax, static_cast<double>(g_hTrack.peakSpeed),
            static_cast<double>(coop::net::throw_bounds::kMaxLaunchSpeedCmS),
            static_cast<double>(coop::net::throw_bounds::kMaxFlightSpeedCmS), coop::prop_throw::dev::Minted(),
            coop::prop_throw::dev::PuppetIgnored());
    if (ArmOf() == Arm::ThrowClamp) {
        if (g_hTrack.peakSpeed > coop::net::throw_bounds::kMaxLaunchSpeedCmS * 1.1f) {
            std::snprintf(why, whyLen, "the host's copy flew at %.0f cm/s, past the throw bound %.0f cm/s", static_cast<double>(g_hTrack.peakSpeed),
                          static_cast<double>(coop::net::throw_bounds::kMaxLaunchSpeedCmS));
            return why;
        }
        return nullptr;
    }
    if (ArmOf() == Arm::ThrowTouch) {
        // The host's pickup took the prop out of the world mid-flight (TouchHostTick): the thrower's row ends with it, and
        // the in-flight gap before the pickup is judged by the same bound as every arm. No pickup measured, no override to judge.
        return nullptr;
    }
    if (c.park > kParkedSamplesMax) {
        std::snprintf(why, whyLen, "the thrower's copy was parked in flight (%d samples)", c.park);
        return why;
    }
    if (matched < kMatchedMin) {
        std::snprintf(why, whyLen, "only %d of the thrower's instants matched a host sample (need %d)", matched, kMatchedMin);
        return why;
    }
    if (worst > kFlightGapPassCm) {
        std::snprintf(why, whyLen, "the host's copy was %.0f cm from the thrower's at k=%d, past %.0f cm", static_cast<double>(worst), worstK,
                      static_cast<double>(kFlightGapPassCm));
        return why;
    }
    return nullptr;
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
    // The trust the host kept in the thrower's rows: a row whose step disagrees with its velocity is displayed and counted, and a
    // run of them revokes the thrower's right (the stepped-rows red makes the thrower send such rows).
    const auto& tv = coop::prop_syncer_host::dev::ThrowViewNow();
    const bool revoked = tv.revokes > g_hRevokes0;
    if (revoked)
        UE_LOGI("[CI-DRILL] host throw %s n=%d trust revoked after %u rows, display followed=%d (%u failed rows shown)", ArmName(), g_hN,
                tv.revokeRows, tv.failedDisplayed > 0 ? 1 : 0, tv.failedDisplayed);
    else
        UE_LOGI("[CI-DRILL] host throw %s n=%d revoked=0 (%u failed rows shown)", ArmName(), g_hN, tv.failedDisplayed);
    char why[200];
    if (!c.peakRead) {   // the end line was cut past its peak words: the peak is not judged, so the arm cannot pass
        g_hHit.Finish(g_hTrack);
        HitWatchEnd();
        g_hTrack.on = false;
        HFail("the client's end line lost its peak words (peakv=/peakp=)");
        return;
    }
    if (!JudgeLaunch(c, why, sizeof(why))) {
        g_hHit.Finish(g_hTrack);
        HitWatchEnd();
        g_hTrack.on = false;
        HFail("%s", why);
        return;
    }
    // Under the stepped-rows red the arm's verdict is the revoke: the forged rows put the host's copy far from the thrower's
    // on purpose, so the flight's gap is not judged; display followed them, and the take-back must come.
    static const bool s_stepped = coop::config::ResolveFlag(coop::config_registry::rows::container_throw_stepped_rows);
    if (s_stepped) {
        g_hHit.Finish(g_hTrack);
        HitWatchEnd();
        g_hTrack.on = false;
        if (!revoked || tv.failedDisplayed == 0) {
            HFail("the stepped rows were %s", revoked ? "never displayed before the revoke" : "never revoked");
            return;
        }
        UE_LOGI("[CI-DRILL] host throw %s n=%d PASS stepped (trust revoked, display followed) %s", ArmName(), g_hN, g_hLaunch);
        HNext(HStep::WaitNext);
        return;
    }
    g_hHit.Finish(g_hTrack);
    HitWatchEnd();
    g_hTrack.on = false;
    if (const char* bad = JudgeFlight(c, why, sizeof(why))) {
        HFail("%s", bad);
        return;
    }
    if (ArmOf() == Arm::ThrowJoin) {   // the PASS waits for the joiner's numbers
        HNext(HStep::JoinWait);
        return;
    }
    UE_LOGI("[CI-DRILL] host throw %s n=%d PASS mirror-gap=n/a %s", ArmName(), g_hN, g_hLaunch);
    HNext(HStep::WaitNext);
}

// throwplace: the host waits for the client to say it let go (part 1) or placed (part 2), then reads what it received.
void StepPlaceTrace(bool due) {
    if (!due) return;
    const uint64_t now = ::GetTickCount64();
    if (!g_hPlacedMs) {
        if (DC::SeenHas(ThrowLine("client", "placed", g_hN, false))) g_hPlacedMs = now;
        else if (HExpired(kEndBoundMs)) HFail("the client never placed in part %d within %llu s", g_hN, U(kEndBoundMs / 1000));
        return;
    }
    char why[200];
    switch (PlaceHostJudge(g_hN, now - g_hPlacedMs, why, sizeof(why))) {
    case HostJudge::Working: return;
    case HostJudge::Failed:  HFail("%s", why); return;
    default:                 break;
    }
    g_hPlacedMs = 0;
    HNext(HStep::WaitNext);   // the client says its hand part (the second line) or its end
}

// throwdepart: the client has left; the host's trace goes on until the copy rests and the rejoined client's seed is read.
void StepDepartTrace() {
    char why[200];
    switch (DepartHostTick(g_hTrack, g_hElemEid, why, sizeof(why))) {
    case HostJudge::Failed:      HFail("%s", why); return;
    case HostJudge::Working:
        if (HExpired(kEndBoundMs + kRejoinWaitMs)) HFail("the thrower's take-back was never seen within %llu s", U((kEndBoundMs + kRejoinWaitMs) / 1000));
        return;
    default:                     break;
    }
    HitWatchEnd();
    UE_LOGI("[CI-DRILL] host throw %s n=%d PASS (the thrower left at its first row; puppet throwShit ignored n=%u)", ArmName(), g_hN,
            coop::prop_throw::dev::PuppetIgnored());
    UE_LOGI("[CI-DRILL] host %s MEASURED", ArmName());
    Finish();
    HNext(HStep::Done);
}

void StepTrace(const coop::net::Session& s, bool due) {
    if (ArmOf() == Arm::ThrowPlace) {
        StepPlaceTrace(due);
        return;
    }
    if (!g_hTrack.on && HandPath(ArmOf(), g_hN)) {
        const uint64_t now = ::GetTickCount64();
        if (now >= g_hFindMs) {
            g_hFindMs = now + kFindEveryMs;
            FindThrown();
        }
    }
    TrackTick(g_hTrack, s.rttMsForSlot(g_hSlot));
    PollOverlap();
    g_hHit.Tick(g_hTrack);
    char why[200];
    if (ArmOf() == Arm::ThrowJoin) {   // the joiner is answered at the first admitted row, then followed through the flight
        JoinHostRelease(s);
        JoinHostSay(g_hN, g_hElemEid);
        JoinHostWatchReady(s, g_hSlot, g_hN, g_hTrack.atRest);
    }
    if (ArmOf() == Arm::ThrowDepart) {
        StepDepartTrace();
        return;
    }
    if (ArmOf() == Arm::ThrowTouch && g_hTouch == Touch::Pending) {
        switch (TouchHostTick(g_hElemEid, why, sizeof(why))) {
        case HostJudge::Failed:      HFail("%s", why); return;
        case HostJudge::Passed:      g_hTouch = Touch::Passed; break;
        case HostJudge::NotMeasured: g_hTouch = Touch::NotMeasured; break;
        case HostJudge::Working:     break;
        }
    }
    if (!due) return;
    InstantsCollect(g_hInst, "client", g_hN);
    if (ArmOf() == Arm::ThrowJoin) JoinHostCollect(g_hN);
    const uint64_t now = ::GetTickCount64();
    if (g_hEndRest.empty()) {
        if (!DC::SeenRestOf(ThrowLine("client", "end", g_hN, true), &g_hEndRest)) {
            if (HExpired(kEndBoundMs)) HFail("the client never ended throw %d within %llu s", g_hN, U(kEndBoundMs / 1000));
            return;
        }
        g_hEndSeenMs = now;
    }
    // The instants come before the end line, in a few parts; the verdict waits for every one of them.
    if (!InstantsComplete(g_hInst)) {
        if (now - g_hEndSeenMs > kInstantsBoundMs)
            HFail("the thrower's instants never all arrived (%d/%d)", InstantsHeld(g_hInst), g_hInst.m);
        return;
    }
    Verdict(g_hEndRest);
}

// throwjoin: the verdict of the flight stands, and the joiner's own numbers come after. The joiner's end line is judged when
// it arrives; a joiner that was not in flight, or that never ends, fails the arm.
void StepJoinWait(const coop::net::Session& s) {
    JoinHostSay(g_hN, g_hElemEid);
    JoinHostWatchReady(s, g_hSlot, g_hN, g_hTrack.atRest);
    JoinHostCollect(g_hN);
    char why[200];
    float gap = -1.f;
    switch (JoinHostJudge(g_hTrack, g_hN, gap, why, sizeof(why))) {
    case HostJudge::Failed: HFail("%s", why); return;
    case HostJudge::Passed:
        JoinHostReport(g_hN, gap);
        UE_LOGI("[CI-DRILL] host throw %s n=%d PASS mirror-gap=%.0f %s", ArmName(), g_hN, static_cast<double>(gap), g_hLaunch);
        HNext(HStep::WaitNext);
        return;
    default: break;
    }
    if (::GetTickCount64() - g_hMs > kJoinerEndMs)
        HFail("the joiner never ended its trace within %llu s", U(kJoinerEndMs / 1000));
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
    if (DC::SeenHas(kThrowDoneLine)) {
        if (ArmOf() == Arm::ThrowPlace) PlaceHostPass();
        UE_LOGI("[CI-DRILL] host %s MEASURED", ArmName());
        Finish();
        HNext(HStep::Done);
        return;
    }
    std::string rest;
    int at[3] = {};
    if (g_hN == 1 && DC::SeenRestOf(kThrowSecondLine, &rest)) {
        if (!IntsAfter(rest, "at=", at, 3)) {
            HFail("the client's second standpoint does not read (%s)", rest.c_str());
            return;
        }
        g_hN = 2;
        if (HandPath(ArmOf(), g_hN)) {
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
    if (ArmOf() == Arm::ThrowHostGrab) {
        HostGrabHostTick(session);
        return;
    }
    if (!g_hMs) {   // the first step's clock starts when the arm does, never at reset
        if (!cen::IsEnabled()) {
            HFail("the arm reads the census's overlap watch: run with container_route_census=1 on both peers");
            return;
        }
        HitWatchInstall();
        if (ArmOf() == Arm::ThrowJoin) JoinHoldBegin(session);
        HNext(HStep::WaitPeer);
    }
    if (g_hStep == HStep::Done) return;
    const uint64_t now = ::GetTickCount64();
    const bool due = now >= g_hNextMs;
    if (due) {
        g_hNextMs = now + RD::kCheckEveryMs;
        if (g_hStep != HStep::WaitPeer && DC::SeenHas(kThrowClientFailedLine)) {
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
    case HStep::WaitArm:   StepWaitArm(session); return;
    case HStep::WaitNext:  StepWaitNext(); return;
    case HStep::Item2Bind: StepItem2Bind(); return;
    case HStep::JoinWait:  StepJoinWait(session); return;
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
    g_hHit = HitJudge();
    g_hSet.clear();
    g_hNow.clear();
    g_hElemEid = 0;
    g_hCount0 = -1;
    g_hOvSeen = g_hOvAll0 = g_hAdds0 = 0;
    g_hOvCount = 0;
    g_hTouch = Touch::Pending;
    g_hPlacedMs = 0;
    g_hArmRead = g_hGateLogged = false;
    g_hArmRest.clear();
    g_hInst = Instants{};
    g_hEndRest.clear();
    g_hEndSeenMs = 0;
    g_hDrop0 = g_hRel0 = 0;
    std::snprintf(g_hLaunch, sizeof(g_hLaunch), "launch=n/a");
}

}  // namespace coop::dev::container_intent_drill::detail

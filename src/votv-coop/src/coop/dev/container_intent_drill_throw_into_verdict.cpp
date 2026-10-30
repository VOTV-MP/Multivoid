// coop/dev/container_intent_drill_throw_into_verdict.cpp -- the HOST's half of the throwinto arm from the arm line to the verdict: the
// take's wait (n=4, n=5), the arm line, the trace of the host's copy, both peers' reads taken in one window and the judgement, the
// held lines and the end of a sub-run. The fixture's steps and the host's tick are in container_intent_drill_throw_into_host.cpp; the
// host's state is `into::g_host` (container_intent_drill_throw_into_host.h).

#include "container_intent_drill_throw_into_host.h"
#include "container_intent_drill_throw_into_judge.h"

#include "coop/dev/container_route_census.h"
#include "coop/dev/drill_chat.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/props/prop_syncer_host.h"
#include "coop/props/prop_throw.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace coop::dev::container_intent_drill::detail::into {

namespace E  = ue_wrap::engine;
namespace DC = coop::dev::drill_chat;

namespace {

constexpr uint64_t kEndBoundMs = 120000;  // the arm line to the client's end line

// n=5 needs a second client (B, by the clients' rule: the one with a connected peer of a lower slot), of a higher slot than the one the
// fixture was built for. It is connected when its slot has a connection, and ready when the host has its world-ready edge.
bool SecondConnected() {
    for (int i = g_host.slot + 1; g_host.session && i < coop::net::kMaxPeers; ++i)
        if (g_host.session->IsSlotConnected(i)) return true;
    return false;
}

bool HasSecondClient() {
    for (int i = g_host.slot + 1; g_host.session && i < coop::net::kMaxPeers; ++i)
        if (g_host.session->IsSlotWorldReady(i)) return true;
    return false;
}

// The next sub-run begins with the client's `need`, where its own basket is spawned.
void NextRun() {
    ++g_host.r;
    g_host.t = 1;
    g_host.ownBasket = false;
}

// The host's element of the client's thrown hand drive: the first prop_drive_C element not in the set taken at the arm line.
void FindThrown() {
    LiveDriveEids(g_host.live);
    uint32_t best = static_cast<uint32_t>(coop::element::kInvalidId);
    for (uint32_t id : g_host.live)
        if (id < best && std::find(g_host.set.begin(), g_host.set.end(), id) == g_host.set.end()) best = id;
    if (!EidBound(best)) return;
    g_host.elemEid = best;
    TrackSet(g_host.m.track, coop::element::LivePropActor(best));
    UE_LOGI("[CI-DRILL] host %s n=%d try %d: the drop path made a drive here, eid=%u (not among the %zu armed)", ArmName(), g_host.r,
            g_host.t, best, g_host.set.size());
}

// A sub-run is over, whatever it read: the next one begins, or the arm ends. n=5 needs two clients: when the second is connected but not
// world-ready at n=4's end its edge is waited for (WaitSecond); when none is connected n=5 is not measured, never a FAIL; the arming
// measure's tail runs first when it is on, and every verdict line stays held until then.
void AfterRun() {
    if (g_host.r == 4 && !HasSecondClient()) {
        if (SecondConnected()) {
            HNext(HStep::WaitSecond);
            return;
        }
        g_host.held[4] = Held{true, false, "[CI-DRILL] host throwinto n=5 observed NOT MEASURED (no second client was connected)"};
    } else if (g_host.r < kLastRun) {
        DC::Say(Ln("host", "run", g_host.r, g_host.t, false));
        NextRun();
        HNext(HStep::Need);
        return;
    }
    if (ArmingOn()) {
        ArmingTailBegin(/*host=*/true, g_host.target.Get(), g_host.targetEid);
        HNext(HStep::Tail);
        return;
    }
    FlushHeld();
    DC::Say(kDoneLine);
    UE_LOGI("[CI-DRILL] host %s MEASURED", ArmName());
    Finish();
    HNext(HStep::Done);
}

}  // namespace

void FlushHeld() {
    for (Held& h : g_host.held) {
        if (!h.have) continue;
        if (h.fail) UE_LOGW("%s", h.line.c_str());
        else UE_LOGI("%s", h.line.c_str());
        h.have = false;
    }
}

// A client's fixture failure before n=5 (its `prefail` line, said by ClientFail): the sub-run's FAIL is held with the other lines, whatever
// step the host is in, the sub-run is let go and the next one begins. True when the line was read.
bool HClientPrefail() {
    std::string why;
    if (g_host.r >= kLastRun || !DC::SeenRestOf(Ln("client", "prefail", g_host.r, g_host.t, true), &why)) return false;
    char line[420];
    std::snprintf(line, sizeof(line), "[CI-DRILL] host throwinto n=%d %s -> FAIL: the client's fixture stopped: %s", g_host.r, KindOf(g_host.r),
                  why.c_str());
    g_host.held[g_host.r - 1] = Held{true, true, line};
    UE_LOGI("[CI-DRILL] host %s n=%d try %d: the client's fixture stopped (%s); held with the other verdicts, the next sub-run goes on", ArmName(),
            g_host.r, g_host.t, why.c_str());
    g_host.m.track.on = false;
    g_host.through = false;
    g_host.armRead = false;
    g_host.armRest.clear();
    g_host.endRest.clear();
    g_host.endSeenMs = 0;
    g_host.newTarget = false;
    AfterRun();
    return true;
}

// n=4 is over and the second client is connected but not world-ready: its `rig: READY peer-world-ready` edge is waited for, to the bound of
// a client's world-ready; a second client that left in the meantime is not measured, as when none was connected.
void StepHWaitSecond() {
    if (HasSecondClient() || !SecondConnected()) {
        AfterRun();
        return;
    }
    if (HExpired(kPeerBoundMs)) HFail("the second client never became world-ready within %llu s", U(kPeerBoundMs / 1000));
}

// n=4 and n=5: the taker's take final arms the box through the host's dingus(); it says so once its mirror's box read armed, so the
// host's own box is read once, here.
void StepHWaitTake() {
    if (!DC::SeenHas(Ln("client", "took", g_host.r, g_host.t, false))) {
        if (HExpired(kArmBoundMs)) HFail("the client never took the basket's record within %llu s", U(kArmBoundMs / 1000));
        return;
    }
    const int state = TriggerState(g_host.target.Get());
    UE_LOGI("[CI-DRILL] host %s n=%d: the client took the record; the target holds %d record(s) and its box reads collision-enabled %d", ArmName(),
            g_host.r, CountOf(g_host.targetEid), state);
    if (state <= 0) {
        HFail("the host's take did not arm the target's box (collision enabled %d)", state);
        return;
    }
    HNext(HStep::WaitArm);
}

void StepHWaitArm() {
    // The arm line is kept when first read: the feed may retire its row.
    if (!g_host.armRead) {
        if (!DC::SeenRestOf(Ln("client", "arm", g_host.r, g_host.t, true), &g_host.armRest)) {
            if (HExpired(kArmBoundMs)) HFail("the client never armed n=%d try %d within %llu s", g_host.r, g_host.t, U(kArmBoundMs / 1000));
            return;
        }
        g_host.armRead = true;
    }
    const char* dead = nullptr;
    if (cen::SeamsState(&dead) != cen::Seams::Live) {
        HFail("the census's watches are not all live (%s)", dead ? dead : "pending");
        return;
    }
    if (void* t = g_host.target.Get()) E::TryGetActorLocation(t, g_host.packAt);
    long long held = 0;
    Num(g_host.armRest, "held", held);
    g_host.armRead = false;
    MeterBegin(g_host.m, "host", g_host.targetEid);
    g_host.elemEid = 0;
    if (HeldRun(g_host.r)) {   // the grab: the element the client holds is the one dropped
        void* actor = held > 0 ? coop::element::LivePropActor(static_cast<uint32_t>(held)) : nullptr;
        if (!actor || static_cast<uint32_t>(held) != g_host.itemEid) {
            HFail("the grabbed drive has no live element here (held eid %lld, the host spawned %u)", held, g_host.itemEid);
            return;
        }
        g_host.elemEid = g_host.itemEid;
        TrackSet(g_host.m.track, actor);
    } else {
        LiveDriveEids(g_host.set);
    }
    g_host.int0 = coop::prop_throw::dev::IntentsSeen();
    g_host.intThrown0 = coop::prop_throw::dev::IntentsThrown();
    g_host.relThrown0 = coop::prop_throw::dev::ReleasesThrown();
    g_host.elected0 = coop::prop_syncer_host::dev::ThrowViewNow().elected;
    g_host.endRest.clear();
    g_host.endSeenMs = 0;
    DC::Say(Ln("host", "armed", g_host.r, g_host.t, false));
    UE_LOGI("[CI-DRILL] host %s n=%d try %d armed: target holds %d record(s), %d drive(s) live", ArmName(), g_host.r, g_host.t, g_host.m.c0,
            g_host.m.drives0);
    HNext(HStep::Trace);
}

namespace {

const char* FirstAdd(uint64_t hostMs, long long clientMs) {
    const uint64_t cl = static_cast<uint64_t>(clientMs);
    if (!hostMs && !cl) return "none";
    if (!cl) return "host";
    if (!hostMs) return "client";
    return hostMs < cl ? "host" : cl < hostMs ? "client" : "same instant";
}

void Verdict(const ClientEnd& c) {
    const Frozen& f = g_host.f;
    if (g_host.m.c0 < 0 || f.h1 < 0 || c.c0 < 0 || c.c1 < 0) {
        HFail("a basket count does not read (host %d -> %d, client %lld -> %lld)", g_host.m.c0, f.h1, c.c0, c.c1);
        return;
    }
    const int dh = f.h1 - g_host.m.c0, dc = static_cast<int>(c.c1 - c.c0);
    const bool elected = coop::prop_syncer_host::dev::ThrowViewNow().elected > g_host.elected0;
    const uint64_t adds = Adds(g_host.m);
    if (ArmingOn()) {   // m4: does the host's copy, parked while a client flies it, enter the box, and how far it steps near it
        TriggerBox hb;
        M4Try m4;
        m4.r = g_host.r;
        m4.t = g_host.t;
        if (ReadTriggerBox(g_host.target.Get(), hb)) {
            const StepStats st = StepsNearBox(g_host.m.track, hb, 100.f);
            m4.maxStep = st.step;
            m4.nearSamples = st.nearCount;
            m4.stepDtMs = st.dtMs;
            m4.speed = st.speed;
            m4.frameMs = st.frameMs;
            m4.perFrame = st.perFrame;
            m4.boxMin = 2.f * (std::min)({hb.ext.X, hb.ext.Y, hb.ext.Z});
        }
        m4.hostOverlaps = Overlaps(g_host.m);
        m4.hostNoSim = OverlapsNoSim(g_host.m);
        m4.hostPreMs = FirstOverlapMs(g_host.m);
        m4.clientPreMs = static_cast<uint64_t>(c.cpre);
        m4.clientDestroyMs = static_cast<uint64_t>(c.cdes);
        ArmingM4Note(m4);
    }
    UE_LOGI("[CI-DRILL] host %s n=%d try %d route census: overlaps host=%llu client=%lld, addObject host=%llu client=%lld, first=%s; "
            "the intent was numbered on %u, the release on %u", ArmName(), g_host.r, g_host.t, U(Overlaps(g_host.m)), c.ov, U(adds), c.adds,
            FirstAdd(AddMs(g_host.m), c.addMs), coop::prop_throw::dev::IntentsThrown() - g_host.intThrown0,
            coop::prop_throw::dev::ReleasesThrown() - g_host.relThrown0);
    if (g_host.through) {   // the through try is measured (m4 above), never judged: the control follows
        UE_LOGI("[CI-DRILL] host %s n=1 try %d aimed through the box and on: measured, not judged", ArmName(), g_host.t);
        g_host.through = false;
        DC::Say(Ln("host", "run", g_host.r, g_host.t, false));
        NextRun();
        HNext(HStep::Need);
        return;
    }
    const JudgeIn in{g_host.r, g_host.m.c0, dh, dc, elected, coop::prop_throw::dev::IntentsSeen() - g_host.int0,
                     coop::prop_throw::dev::IntentsThrown() - g_host.intThrown0, coop::prop_throw::dev::ReleasesThrown() - g_host.relThrown0};
    const Outcome o = Judge(in, f, c);
    g_host.m.track.on = false;
    if (o.miss && g_host.t < kTries) {
        UE_LOGI("[CI-DRILL] host %s n=%d try %d: miss, the prop rests %.0f cm from the basket (host +0, client +0); trying again", ArmName(),
                g_host.r, g_host.t, static_cast<double>(f.restCm));
        char say[48];
        std::snprintf(say, sizeof(say), "rest=%d,%d,%d", static_cast<int>(f.last.X), static_cast<int>(f.last.Y), static_cast<int>(f.last.Z));
        DC::Say(Ln("host", "retry", g_host.r, g_host.t, true) + say);
        ++g_host.t;
        HNext(HStep::Need);   // every retry has a NEW drive from the host, as the first try has the fixture's
        return;
    }
    char why[220];
    std::snprintf(why, sizeof(why), "%s", o.miss ? "no try landed in the basket (3 misses)" : o.why);
    const bool pass = !o.miss && !*o.why;
    char obs[300];   // what follows the counts: the stray terms of both peers, a held drive filed while held, n=5's first client
    int on = std::snprintf(obs, sizeof(obs), " terms host alive=%d excess=%d key=%d client alive=%lld excess=%lld key=%lld", f.sa, f.se, f.sk, c.sa, c.se,
                           c.sk);
    if (c.fwh) on += std::snprintf(obs + on, sizeof(obs) - static_cast<size_t>(on), " filed while held");
    if (g_host.r == 5) std::snprintf(obs + on, sizeof(obs) - static_cast<size_t>(on), " first client %lld->%lld stray=%lld", c.obs0, c.obs1, c.obsStray);
    char line[800];
    std::snprintf(line, sizeof(line), "[CI-DRILL] host throwinto n=%d %s seq=%lld elected=%d host %d->%d client %lld->%lld stray host=%d "
                  "client=%lld addObject host=%llu@%llu client=%lld@%lld%s -> %s%s", g_host.r, KindOf(g_host.r), c.seq, elected ? 1 : 0, g_host.m.c0, f.h1,
                  c.c0, c.c1, f.strayH, c.stray, U(adds), U(AddMs(g_host.m)), c.adds, c.addMs, obs, pass ? "PASS" : "FAIL: ", pass ? "" : why);
    g_host.held[g_host.r - 1] = Held{true, !pass, line};
    if (g_host.r == 1 && ArmingOn()) {   // one more thrown try, aimed THROUGH the box and on: measured by m4, not judged
        DC::Say(Ln("host", "through", g_host.r, g_host.t, false));
        ++g_host.t;
        g_host.through = true;
        HNext(HStep::Need);
        return;
    }
    AfterRun();
}

// The host's copy has settled (or the bound passed): its own reads are taken now, and the client is told to take its second.
void Freeze() {
    if (!ReadEnd(g_host.endRest, g_host.end)) {
        HFail("the client's end line does not read (%s)", g_host.endRest.c_str());
        return;
    }
    Frozen& f = g_host.f;
    f = Frozen{};
    f.h1 = CountOf(g_host.targetEid);
    f.alive = PropAlive(g_host.m);
    f.hadElem = g_host.m.track.on;
    f.settled = !g_host.m.track.on || g_host.m.track.atRest || g_host.m.track.gone;
    const StrayTerms st = StrayTermsOf(g_host.m, HeldRun(g_host.r));
    f.strayH = st.Max();
    f.sa = st.alive;
    f.se = st.excess;
    f.sk = st.key;
    f.last = g_host.m.track.last;
    if (!g_host.m.track.samples.empty()) f.restCm = std::hypot(f.last.X - g_host.packAt.X, f.last.Y - g_host.packAt.Y);
    DC::Say(Ln("host", "read", g_host.r, g_host.t, false));
    HNext(HStep::Late);
}

}  // namespace

void StepHTrace(const coop::net::Session& s, bool due) {
    if (ThrownRun(g_host.r) && !g_host.m.track.on && ::GetTickCount64() >= g_host.findMs) {
        g_host.findMs = ::GetTickCount64() + 50;
        FindThrown();
    }
    TrackTick(g_host.m.track, s.rttMsForSlot(g_host.slot));
    MeterKey(g_host.m);
    if (!due) return;
    const uint64_t now = ::GetTickCount64();
    if (g_host.endRest.empty()) {
        if (!DC::SeenRestOf(Ln("client", "end", g_host.r, g_host.t, true), &g_host.endRest)) {
            if (HExpired(kEndBoundMs)) HFail("the client never ended n=%d try %d within %llu s", g_host.r, g_host.t, U(kEndBoundMs / 1000));
            return;
        }
        g_host.endSeenMs = now;
    }
    // The host's copy settles on its own clock; one that never came (no element) or never rests is judged as it stands.
    if (!Settled(g_host.m) && now - g_host.endSeenMs < kHostSettleMs) return;
    Freeze();
}

// The client's second read, taken after the host's: both sides of the judgement are read in one window.
void StepHLate(bool due) {
    if (!due) return;
    std::string rest;
    if (!DC::SeenRestOf(Ln("client", "late", g_host.r, g_host.t, true), &rest)) {
        if (HExpired(kEndBoundMs)) HFail("the client never answered the read of n=%d try %d within %llu s", g_host.r, g_host.t, U(kEndBoundMs / 1000));
        return;
    }
    if (!ReadLate(rest, g_host.end)) {
        HFail("the client's late line does not read (%s)", rest.c_str());
        return;
    }
    if (g_host.r == 5) {   // the first client's mirror, read in the same window as the second's
        std::string first;
        if (!DC::SeenRestOf(Ln("clientA", "late", g_host.r, g_host.t, true), &first)) {
            if (HExpired(kEndBoundMs)) HFail("the first client never answered the read of n=5 try %d within %llu s", g_host.t, U(kEndBoundMs / 1000));
            return;
        }
        if (!ReadObserver(first, g_host.end)) {
            HFail("the first client's late line does not read (%s)", first.c_str());
            return;
        }
    }
    Verdict(g_host.end);
}

}  // namespace coop::dev::container_intent_drill::detail::into

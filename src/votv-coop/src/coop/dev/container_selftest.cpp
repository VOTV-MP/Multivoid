// coop/dev/container_selftest.cpp -- see coop/dev/container_selftest.h.

#include "coop/dev/container_selftest.h"

#include "coop/config/config.h"
#include "coop/dev/container_pick.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/container_poke.h"
#include "coop/dev/director/director.h"   // PlayerContext -- where the local body is; the background walk
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/props/container_act_host.h"
#include "coop/props/container_act_request.h"
#include "coop/props/container_contents_sync.h"
#include "coop/props/container_writer_watch.h"

#include "ue_wrap/actors/container_view.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::container_selftest {
namespace {

namespace CC = coop::props::container_contents_sync;
namespace AR = coop::props::container_act_request;
namespace AH = coop::props::container_act_host;
namespace CV = ue_wrap::container_view;
namespace DR = coop::director;
namespace CP = coop::dev::container_pick;
namespace DC = coop::dev::drill_chat;
using Clock = std::chrono::steady_clock;

std::atomic<coop::net::Session*> g_session{nullptr};

bool Enabled() {
    static const bool s = coop::config::ResolveFlag(::coop::config_registry::rows::container_selftest);
    return s;
}

uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count());
}

// The schedule, measured from the first tick at which this peer is connected.
constexpr uint64_t kHostFireMs   = 10000;
constexpr uint64_t kClientFireMs = 25000;
constexpr uint64_t kDigestEveryMs = 5000;
constexpr uint64_t kPickEveryMs  = 1000;   // the client target's pick runs this often until it holds
constexpr uint64_t kPickFailMs   = 60000;  // a pick unresolved this long after the first ask is a FAIL

// A run with SEVERAL clients needs every client to fire after the LAST of them has joined, or
// a later joiner's seed already carries an earlier client's write and the peers never exercise
// each other. Raw env, no registry row, like the other per-run drill triggers.
uint64_t ClientFireMs() {
    static const uint64_t ms = [] {
        const std::string v = coop::config::ReadEnv("VOTVCOOP_CONTAINER_FIRE_MS");
        const uint64_t n = v.empty() ? 0 : std::strtoull(v.c_str(), nullptr, 10);
        return n ? n : kClientFireMs;
    }();
    return ms;
}

uint64_t g_connectedAtMs = 0;
uint64_t g_nextDigestMs  = 0;
bool g_fired = false;

// The host's verdict, printed once, fifteen seconds after its own fire -- long enough for the
// watch to have entered and the count to have settled.
constexpr uint64_t kVerdictAfterMs = 15000;
uint64_t g_verdictAtMs = 0;
bool     g_verdictDone = false;
int32_t  g_fireBefore  = -1;
int32_t  g_fireAfter   = -1;
uint32_t g_fireEid     = 0;

// The two containers under test. The host extracts from the nearest non-empty one to its own body. The client takes from
// the container drill's pick (near, a nav route reaches it within the walk budget, two records or more) and says that
// eid on drill chat when it picks: the host judges the container the client announced, never one it picked for it. The
// two may be one container, or two; the host's base re-read at its own extract keeps the halves from masking each other.
uint32_t g_eidHostTarget   = 0;
uint32_t g_eidClientTarget = 0;   // the client: its own pick; the host: the eid the client announced
uint64_t g_nextPickMs      = 0;   // the own target's pick runs once a second until it holds
bool     g_anchorUnread    = false;   // the player's location could not be read: no pick this session
bool     g_fallbackSaid    = false;   // the pick fell back to a far container, said once
bool     g_pickFailed      = false;   // the pick did not resolve within kPickFailMs
uint64_t g_pickStartMs     = 0;       // when the pick was first asked
float    g_clientRouteCm   = 0.f;     // the route to the client target, which sets the walk's budget

// The client's two lines on drill chat: its target when it picks, and its verdict when it has one.
constexpr const char* kTargetPrefix  = "container selftest client target ";
constexpr const char* kVerdictPrefix = "container selftest client verdict ";

// The client's take: it walks to its target, opens the screen and presses the first slot, so the take is an act.
// The host judges the same take from its side: the client target's count fell by one, one grant, no Gone.
constexpr uint64_t kOpenFailMs    = 10000;   // the screen shows the container within this after openPropInv
constexpr uint64_t kCommitFailMs  = 15000;   // the take's commit goes out within this after the press
constexpr uint64_t kHostSettleMs  = 3000;    // the host reads the count this long after it first answered the take
constexpr uint64_t kHostBoundMs   = 360000;  // the take reaches the host within this after the client's fire is due
enum class CStep : uint8_t { Idle, Walking, Opening, Pressed, Done };
CStep    g_cStep = CStep::Idle;
uint64_t g_cStepMs = 0;
uint32_t g_takesBefore = 0;
int32_t  g_mirrorBefore = -1;
std::shared_ptr<DR::BackgroundWalk> g_walk;
ue_wrap::CachedObjRef g_clientActor;
int32_t  g_hostClientBase = -1;   // the host: the client target's count when it was announced
uint64_t g_hostSeenMs = 0;        // the host: when it first answered a take on the client target
bool     g_hostJudged = false;
std::string g_clientVerdict;      // the host: the client's verdict line, once heard
bool     g_doneSaid = false;      // the host: DONE printed


// Pick only containers that actually HOLD something. An empty container cannot be extracted
// from: firing on one changes nothing, and an absent "callback ENTERED" line would then read
// as a lane failure rather than as an inert trigger, which is what the guard below catches.
// Choosing by CONTENT rather than by registry order is what makes the trigger real.
constexpr size_t kScan = 64;

// The host's target, by DISTANCE from its own body (the eid breaking a tie); the chosen distance is logged.
bool TryResolveHostTarget(const coop::director::PlayerContext& ctx) {
    CC::WorldContainer picks[kScan]{};
    const size_t n = CC::SnapshotWorldContainers(picks, kScan);
    struct Cand { uint32_t eid; float dist; };
    std::vector<Cand> cands;
    for (size_t i = 0; i < n; ++i) {
        int32_t cnt = -1; float vol = 0.f;
        if (!CC::ContentsDigest(picks[i].eid, cnt, vol) || cnt < 1) continue;
        ue_wrap::FVector pos{};
        if (!ue_wrap::engine::TryGetActorLocation(picks[i].actor, pos)) continue;
        const float dx = pos.X - ctx.pos.X, dy = pos.Y - ctx.pos.Y, dz = pos.Z - ctx.pos.Z;
        cands.push_back(Cand{picks[i].eid, std::sqrt(dx * dx + dy * dy + dz * dz)});
    }
    if (cands.size() < 2) return false;   // keep looking; the world may still be filling
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        // The eid breaks a tie, never the scan order: that is registry order and differs per peer.
        return a.dist != b.dist ? a.dist < b.dist : a.eid < b.eid;
    });
    g_eidHostTarget = cands[0].eid;
    UE_LOGI("container_selftest: HOST extracts from eid=%u at %.0f uu (nearest non-empty); scanned %zu world containers",
            g_eidHostTarget, cands[0].dist, n);
    return true;
}

// The client's target: the shared container pick, refusing its far fallback; announced on drill chat.
bool TryResolveClientTarget() {
    CP::Pick pick;
    if (!CP::PickContainer(CP::Want{}, pick)) return false;   // no container to take from yet; ask again
    if (pick.fallback) {   // only a far container: a walk to it would outlast the budget, which the pick exists to refuse
        if (!g_fallbackSaid) UE_LOGW("container_selftest: no container near the player is reachable within the walk budget");
        g_fallbackSaid = true;
        return false;
    }
    g_clientRouteCm = pick.routeCm;
    g_eidClientTarget = pick.eid;
    DC::Say(std::string(kTargetPrefix) + std::to_string(pick.eid));
    UE_LOGI("container_selftest: CLIENT takes from eid=%u, %d records, route %.0f cm (the drill's pick: near, a route "
            "reaches it); %d of %d world containers qualify", pick.eid, pick.records,
            static_cast<double>(pick.routeCm), pick.qualified, pick.seen);
    return true;
}

void ClientVerdict(bool green, const char* why);

// This peer's own target, asked once a second; a pick that has not resolved within kPickFailMs is the drill's FAIL,
// said once (the client also says it as its verdict).
bool ResolveTarget(bool host) {
    if (host ? g_eidHostTarget != 0 : g_eidClientTarget != 0) return true;
    if (g_anchorUnread || g_pickFailed) return false;
    const uint64_t pickNow = NowMs();
    if (pickNow < g_nextPickMs) return false;
    g_nextPickMs = pickNow + kPickEveryMs;
    if (!g_pickStartMs) g_pickStartMs = pickNow;
    coop::director::PlayerContext ctx;
    if (!ctx.Refresh()) {   // no possessed body yet: the pick has no anchor
        // An unread location is a faulted read, which repeats: the pick ends here, said once.
        if (ctx.posUnread) {
            g_anchorUnread = true;
            UE_LOGW("container_selftest: no targets -- the player's location could not be read, so the "
                    "pick has no anchor");
        }
        return false;
    }
    if (host ? TryResolveHostTarget(ctx) : TryResolveClientTarget()) return true;
    if (pickNow - g_pickStartMs >= kPickFailMs) {
        g_pickFailed = true;
        UE_LOGW("container_selftest: FAIL -- no container with two records or more, near the player and reachable "
                "within the walk budget, after %llu s", static_cast<unsigned long long>(kPickFailMs / 1000));
        if (!host) ClientVerdict(false, "no container near the player is reachable within the walk budget");
    }
    return false;
}

// The host reads the client's two lines off drill chat, each once: the target (the eid it judges, and whose count it
// takes as its base) and the verdict.
void HostHearClient() {
    if (!DC::PollDue()) return;
    std::string rest;
    if (!g_eidClientTarget && DC::LiveRestOf(kTargetPrefix, &rest)) {
        g_eidClientTarget = static_cast<uint32_t>(std::strtoul(rest.c_str(), nullptr, 10));
        float baseVol = 0.f;
        CC::ContentsDigest(g_eidClientTarget, g_hostClientBase, baseVol);
        UE_LOGI("container_selftest: HOST heard the client's target eid=%u, %d records", g_eidClientTarget,
                g_hostClientBase);
    }
    if (g_clientVerdict.empty() && DC::LiveRestOf(kVerdictPrefix, &rest)) {
        g_clientVerdict = rest;
        UE_LOGI("container_selftest: HOST heard the client's verdict: %s", rest.c_str());
    }
}

// The HOST's own mutation: the organic verb a player's UI extraction runs (the dispatch, and
// why never takeObj itself: coop/dev/container_poke). A host verb is authority, so it stays raw.
// A client never fires it: its take is an act, ClientTick below.
//
// It spawns the extracted prop into the world, exactly as a real extraction does. That is
// litter in a throwaway world and the price of an honest trigger.
void FireExtract(uint32_t eid) {
    void* const actor = coop::element::LivePropActor(eid);
    if (!actor) {
        UE_LOGW("container_selftest: HOST target eid=%u no longer resolves -- not fired", eid);
        return;
    }
    int32_t before = -1, after = -1, count = -1;
    float volBefore = 0.f, volAfter = 0.f;
    CC::ContentsDigest(eid, count, volBefore);
    if (!coop::dev::container_poke::Extract(actor, before, after, eid)) {
        UE_LOGW("container_selftest: extract did not resolve for this container -- not fired");
        return;
    }
    CC::ContentsDigest(eid, count, volAfter);
    g_fireEid = eid; g_fireBefore = before; g_fireAfter = after;
    // Both halves on one container: the client's take starts from what the extract left.
    if (eid == g_eidClientTarget) g_hostClientBase = after;
    UE_LOGI("container_selftest: HOST FIRED extract(0) on eid=%u -- records %d -> %d, "
            "currVol %.1f -> %.1f", eid, before, after, volBefore, volAfter);
    if (before == after) {
        UE_LOGW("container_selftest: HOST extract changed NOTHING on eid=%u -- the TRIGGER is inert, "
                "so an absent 'callback ENTERED' line says nothing about the lane. Fix the trigger "
                "before reading any verdict from this run.", eid);
    }
}

// The host's line a run is judged by. RED is reserved for the two failures that make every other
// line in the run meaningless: a trigger that changed nothing, and a watch that never fired.
void Verdict() {
    int32_t now = -1; float vol = 0.f;
    const bool live = CC::ContentsDigest(g_fireEid, now, vol);
    const bool entered = coop::props::container_writer_watch::VerbWatchEntered();
    const bool inert = (g_fireBefore >= 0 && g_fireBefore == g_fireAfter);
    const char* fate = !live      ? "the container stopped resolving"
                     : now == g_fireAfter ? "STUCK (the fired change stayed)"
                     : now == g_fireBefore ? "REVERTED (the contents went back)"
                     : "MOVED AGAIN (another peer edited it since)";
    UE_LOGI("container_selftest: VERDICT HOST -- %s: watch ENTERED=%d, fire eid=%u %d -> %d, now %d "
            "[%s]", (inert || !entered) ? "RED" : "GREEN",
            entered ? 1 : 0, g_fireEid, g_fireBefore, g_fireAfter, now, fate);
    if (inert)   UE_LOGW("container_selftest: RED -- the trigger changed nothing, so nothing else "
                         "in this run says anything about the lane");
    if (!entered) UE_LOGW("container_selftest: RED -- the addObject/takeObj watch never fired on "
                          "this peer; the lane is inert here");
}

// ---- the client's take: an act, from a walk to the container to the commit ----

void WalkStop() {
    if (g_walk && g_walk->state.load() == 0) DR::CancelBackgroundWalk(*g_walk);
    g_walk.reset();
}

void ClientVerdict(bool green, const char* why) {
    int32_t mirror = -1; float vol = 0.f;
    CC::ContentsDigest(g_eidClientTarget, mirror, vol);
    const AR::dev::Take t = AR::dev::LastTake();
    UE_LOGI("container_selftest: VERDICT CLIENT -- %s: %s [eid=%u committed=%u takes=%u->%u refusal=%u "
            "mirror %d -> %d]", green ? "GREEN" : "RED", why, g_eidClientTarget, static_cast<unsigned>(t.committed),
            g_takesBefore, t.count, static_cast<unsigned>(AR::dev::LastRefusal()), g_mirrorBefore, mirror);
    DC::Say(std::string(kVerdictPrefix) + (green ? "GREEN" : "RED"));   // the host's DONE waits for it
    WalkStop();
    g_cStep = CStep::Done;
}

void ClientStart(uint64_t now) {
    g_clientActor.Set(coop::element::LivePropActor(g_eidClientTarget));
    ue_wrap::FVector at{};
    if (!g_clientActor.Get() || !ue_wrap::engine::TryGetActorLocation(g_clientActor.Get(), at)) {
        ClientVerdict(false, "the client target no longer resolves");
        return;
    }
    g_walk = DR::StartBackgroundWalk(at, CP::kStandCm, CP::WalkSeconds(g_clientRouteCm));
    g_cStepMs = now;
    g_cStep = CStep::Walking;
    UE_LOGI("container_selftest: CLIENT walks to eid=%u at (%.0f,%.0f,%.0f)", g_eidClientTarget,
            static_cast<double>(at.X), static_cast<double>(at.Y), static_cast<double>(at.Z));
}

void ClientTick(uint64_t now) {
    switch (g_cStep) {
    case CStep::Walking: {
        const int st = g_walk ? g_walk->state.load() : 2;
        if (st == 0) return;
        if (st != 1) { ClientVerdict(false, "the walk to the client target did not arrive"); return; }
        if (!CV::Open(g_clientActor.Get())) { ClientVerdict(false, "openPropInv did not dispatch"); return; }
        g_cStepMs = now;
        g_cStep = CStep::Opening;
        return;
    }
    case CStep::Opening: {
        if (CV::Viewed() != g_clientActor.Get()) {
            if (now - g_cStepMs >= kOpenFailMs) ClientVerdict(false, "the screen does not show the container");
            return;
        }
        void* const slot = CV::FirstSlot(CV::InventoryScreen());
        CV::Slot s{};
        if (!slot || !CV::ReadSlot(slot, s)) { ClientVerdict(false, "the open screen has no readable first slot"); return; }
        float vol = 0.f;
        CC::ContentsDigest(g_eidClientTarget, g_mirrorBefore, vol);
        g_takesBefore = AR::dev::LastTake().count;
        const bool pressed = CV::PressSlot(slot);
        UE_LOGI("container_selftest: CLIENT pressed=%d slot id=%d pending id=%u", pressed ? 1 : 0, s.id,
                static_cast<unsigned>(AR::dev::PendingId()));
        if (!pressed) { ClientVerdict(false, "the slot's pressButton did not dispatch"); return; }
        if (!AR::dev::PendingId() && AR::dev::LastTake().count == g_takesBefore) {
            ClientVerdict(false, "the press opened no act");
            return;
        }
        g_cStepMs = now;
        g_cStep = CStep::Pressed;
        return;
    }
    case CStep::Pressed: {
        const AR::dev::Take t = AR::dev::LastTake();
        if (t.count != g_takesBefore) {
            const bool granted = t.committed == 1 && AR::dev::LastRefusal() == 0;
            ClientVerdict(granted, granted ? "a Granted reply and a done commit"
                                           : "the take ended without a done commit");
            return;
        }
        if (AR::dev::LastRefusal()) { ClientVerdict(false, "the host refused the take"); return; }
        if (now - g_cStepMs >= kCommitFailMs) ClientVerdict(false, "no commit after the press");
        return;
    }
    case CStep::Idle:
    case CStep::Done:
        return;
    }
}

// The host's side of the client's take: it answered the take (one grant, no Gone) and its count fell by one.
void HostJudgeClientTake(uint64_t now) {
    if (g_hostJudged) return;
    const bool bound = now - g_connectedAtMs >= ClientFireMs() + kHostBoundMs;
    if (!g_eidClientTarget) {   // the client never announced a target: its verdict, or the bound, ends the wait
        if (g_clientVerdict.empty() && !bound) return;
        g_hostJudged = true;
        UE_LOGI("container_selftest: VERDICT HOST-SEES-CLIENT -- RED: the client announced no target [client verdict: %s]",
                g_clientVerdict.empty() ? "none" : g_clientVerdict.c_str());
        return;
    }
    uint32_t grants = 0, gones = 0;
    AH::dev::Verdicts(g_eidClientTarget, grants, gones);
    // The client's verdict is its last word: the host reads its count a settle after the first answer or after that word.
    if (!g_hostSeenMs && (grants || gones || !g_clientVerdict.empty())) g_hostSeenMs = now;
    if (g_hostSeenMs ? now - g_hostSeenMs < kHostSettleMs : !bound) return;
    g_hostJudged = true;
    int32_t count = -1; float vol = 0.f;
    CC::ContentsDigest(g_eidClientTarget, count, vol);
    const bool green = grants == 1 && gones == 0 && g_hostClientBase >= 0 && count == g_hostClientBase - 1;
    UE_LOGI("container_selftest: VERDICT HOST-SEES-CLIENT -- %s: eid=%u grants=%u gones=%u count %d -> %d",
            green ? "GREEN" : "RED", g_eidClientTarget, grants, gones, g_hostClientBase, count);
}

void Digest() {
    for (uint32_t eid : {g_eidHostTarget, g_eidClientTarget}) {
        if (!eid) continue;
        int32_t n = -1; float vol = 0.f;
        if (CC::ContentsDigest(eid, n, vol)) {
            UE_LOGI("container_selftest: DIGEST eid=%u records=%d currVol=%.1f", eid, n, vol);
        }
    }
}

}  // namespace

void Install(coop::net::Session* session) {
    if (!Enabled()) return;
    g_session.store(session, std::memory_order_release);
}

void Tick() {
    if (!Enabled()) return;
    auto* s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;

    const uint64_t now = NowMs();
    if (!g_connectedAtMs) {
        g_connectedAtMs = now;
        g_nextDigestMs = now + kDigestEveryMs;
        UE_LOGI("container_selftest: ARMED (host fires at +%llums, client at +%llums)",
                static_cast<unsigned long long>(kHostFireMs),
                static_cast<unsigned long long>(ClientFireMs()));
    }
    const bool host = s->role() == coop::net::Role::Host;
    if (host) HostHearClient();
    const bool resolved = ResolveTarget(host);   // this peer's own target
    const uint64_t due = host ? kHostFireMs : ClientFireMs();
    if (resolved && !g_fired && now - g_connectedAtMs >= due) {
        g_fired = true;
        if (host) {
            g_verdictAtMs = now + kVerdictAfterMs;
            FireExtract(g_eidHostTarget);
        } else {
            ClientStart(now);
        }
    }
    if (host && g_fired && !g_verdictDone && now >= g_verdictAtMs) {
        g_verdictDone = true;
        Verdict();
    }
    if (host) HostJudgeClientTake(now);
    else ClientTick(now);
    // The run's end marker: the host's own verdict (or its failed pick), its judgement of the client, and the client's
    // verdict are all in.
    if (host && !g_doneSaid && (g_verdictDone || g_pickFailed || g_anchorUnread) && g_hostJudged &&
        !g_clientVerdict.empty()) {
        g_doneSaid = true;
        UE_LOGI("container_selftest: DONE");
    }
    if (now >= g_nextDigestMs) {
        g_nextDigestMs = now + kDigestEveryMs;
        Digest();
    }
}

void OnDisconnect() {
    if (!Enabled()) return;
    WalkStop();
    g_cStep = CStep::Idle;
    g_cStepMs = 0;
    g_clientActor.Reset();
    g_takesBefore = 0;
    g_mirrorBefore = -1;
    g_hostClientBase = -1;
    g_hostSeenMs = 0;
    g_hostJudged = false;
    g_clientVerdict.clear();
    g_doneSaid = false;
    g_connectedAtMs = 0;
    g_nextDigestMs = 0;
    g_fired = false;
    g_verdictDone = false;
    g_verdictAtMs = 0;
    g_fireBefore = g_fireAfter = -1;
    g_fireEid = 0;
    g_eidHostTarget = g_eidClientTarget = 0;
    g_nextPickMs = 0;
    g_anchorUnread = false;
    g_fallbackSaid = false;
    g_pickFailed = false;
    g_pickStartMs = 0;
    g_clientRouteCm = 0.f;
}

}  // namespace coop::dev::container_selftest

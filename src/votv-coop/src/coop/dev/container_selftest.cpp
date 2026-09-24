// coop/dev/container_selftest.cpp -- see coop/dev/container_selftest.h.

#include "coop/dev/container_selftest.h"

#include "coop/config/config.h"
#include "coop/dev/director/director.h"   // PlayerContext -- where the local body is
#include "coop/net/session.h"
#include "coop/props/container_contents_sync.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::container_selftest {
namespace {

namespace R  = ue_wrap::reflection;
namespace CC = coop::props::container_contents_sync;
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

// A run with SEVERAL clients needs every client to fire after the LAST of them has joined, or
// each one is judged against a baseline its own join seed had just refreshed and the peers never
// exercise each other. Raw env, no registry row, like the other per-run drill triggers.
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

// The verdict, printed once, fifteen seconds after this peer's own fire -- long enough for the
// host's answer (an apply, or a refusal and its corrective) to have landed and settled.
constexpr uint64_t kVerdictAfterMs = 15000;
uint64_t g_verdictAtMs = 0;
bool     g_verdictDone = false;
int32_t  g_fireBefore  = -1;
int32_t  g_fireAfter   = -1;
uint32_t g_fireEid     = 0;

// The two containers under test, chosen ONCE and by the same rule on both peers: the eids are
// cross-peer stable, so peer A's pick and peer B's pick name the same actors. Index 0 is the
// host's target, index 1 the client's -- deliberately DIFFERENT containers, so the two halves of
// the circle cannot mask each other (one lane working both ways would look identical to two lanes
// each working one way if both fired at the same container).
uint32_t g_eidHostTarget   = 0;
uint32_t g_eidClientTarget = 0;
bool     g_anchorUnread    = false;   // the player's location could not be read: no pick this session


// Pick only containers that actually HOLD something. An empty container cannot be extracted
// from: firing on one changes nothing, and an absent "callback ENTERED" line would then read
// as a lane failure rather than as an inert trigger, which is what the guard below catches.
// Choosing by CONTENT rather than by registry order is what makes the trigger real.
constexpr size_t kScan = 64;

// ...and by DISTANCE, because the host arbitrates a client's slice against the author's reach: a
// client firing on a container across the map now authors a write the host refuses, and the
// instrument would be measuring the refusal path while claiming to measure the circle. Both peers
// order the same candidate set by (distance from their own body, eid), which picks the same pair
// while they stand together -- and the chosen distances are logged, so a run where they did not
// can be told apart from a run where the lane broke.
bool ResolveTargets() {
    if (g_eidHostTarget && g_eidClientTarget) return true;
    if (g_anchorUnread) return false;
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
    g_eidHostTarget   = cands[0].eid;
    g_eidClientTarget = cands[1].eid;
    UE_LOGI("container_selftest: targets chosen (non-empty, nearest first) -- host extracts from "
            "eid=%u at %.0f uu, client from eid=%u at %.0f uu (scanned %zu world containers)",
            g_eidHostTarget, cands[0].dist, g_eidClientTarget, cands[1].dist, n);
    return true;
}

// Dispatch the ORGANIC mutation: `Aprop_container_C::extract(int32 Index)`, the very verb a
// player's UI extraction runs. extract's first act is `propInventory->takeObj(index, false,
// ...)`, dispatched through EX_LocalVirtualFunction, so the call WE make is the outer one and
// the mutation the lane must catch is the game's own inner dispatch. Calling `takeObj`
// ourselves would arrive by ProcessEvent -- the one path the lane does NOT rely on -- and
// would prove nothing, since a probe must never resolve through the mechanism under test.
//
// It spawns the extracted prop into the world, exactly as a real extraction does. That is
// litter in a throwaway world and the price of an honest trigger.
void FireExtract(uint32_t eid, const char* who) {
    CC::WorldContainer picks[kScan]{};
    const size_t n = CC::SnapshotWorldContainers(picks, kScan);
    void* actor = nullptr;
    for (size_t i = 0; i < n; ++i) {
        if (picks[i].eid == eid) { actor = picks[i].actor; break; }
    }
    if (!actor) {
        UE_LOGW("container_selftest: %s target eid=%u no longer resolves -- not fired", who, eid);
        return;
    }
    // The DISPATCH function for THIS actor's own class, resolved per fire and not cached in one
    // slot: FindFunction is exact-owner, so ClassOf(actor) -- always a subclass here -- yields
    // null through it, while resolving the base instead would run the base body on a player
    // inventory container, which overrides extract. The targets differ between fires, so one
    // cached pointer would carry the first target's body to every later one.
    void* extractFn = R::FindDispatchFunction(R::ClassOf(actor), L"extract", nullptr);
    if (!extractFn) {
        UE_LOGW("container_selftest: extract did not resolve for this container -- not fired");
        return;
    }
    int32_t before = -1; float volBefore = 0.f;
    CC::ContentsDigest(eid, before, volBefore);
    struct { int32_t Index; } params{0};   // always the first record
    R::CallFunction(actor, extractFn, &params);
    int32_t after = -1; float volAfter = 0.f;
    CC::ContentsDigest(eid, after, volAfter);
    g_fireEid = eid; g_fireBefore = before; g_fireAfter = after;
    UE_LOGI("container_selftest: %s FIRED extract(0) on eid=%u -- records %d -> %d, "
            "currVol %.1f -> %.1f", who, eid, before, after, volBefore, volAfter);
    if (before == after) {
        UE_LOGW("container_selftest: %s extract changed NOTHING on eid=%u -- the TRIGGER is inert, "
                "so an absent 'callback ENTERED' line says nothing about the lane. Fix the trigger "
                "before reading any verdict from this run.", who, eid);
    }
}

// One line a run is judged by. RED is reserved for the two failures that make every other line
// in the run meaningless: a trigger that changed nothing, and a watch that never fired. What the
// HOST's answer was is reported, not asserted -- a client that fired from outside the arbiter's
// reach is SUPPOSED to be reverted, and this instrument does not walk, so both outcomes are legal
// here. The accepted client path has an owner that does walk: `mp.py ctakerace`.
void Verdict(bool host) {
    int32_t now = -1; float vol = 0.f;
    const bool live = CC::ContentsDigest(g_fireEid, now, vol);
    const bool entered = CC::VerbWatchEntered();
    const bool inert = (g_fireBefore >= 0 && g_fireBefore == g_fireAfter);
    const char* fate = !live      ? "the container stopped resolving"
                     : now == g_fireAfter ? "STUCK (the arbiter accepted it, or this peer is the host)"
                     : now == g_fireBefore ? "REVERTED (the host refused and re-published its truth)"
                     : "MOVED AGAIN (another peer edited it since)";
    UE_LOGI("container_selftest: VERDICT %s -- %s: watch ENTERED=%d, fire eid=%u %d -> %d, now %d "
            "[%s]", host ? "HOST" : "CLIENT",
            (inert || !entered) ? "RED" : "GREEN",
            entered ? 1 : 0, g_fireEid, g_fireBefore, g_fireAfter, now, fate);
    if (inert)   UE_LOGW("container_selftest: RED -- the trigger changed nothing, so nothing else "
                         "in this run says anything about the lane");
    if (!entered) UE_LOGW("container_selftest: RED -- the addObject/takeObj watch never fired on "
                          "this peer; the lane is inert here");
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
    if (!ResolveTargets()) return;

    const bool host = s->role() == coop::net::Role::Host;
    const uint64_t due = host ? kHostFireMs : ClientFireMs();
    if (!g_fired && now - g_connectedAtMs >= due) {
        g_fired = true;
        g_verdictAtMs = now + kVerdictAfterMs;
        FireExtract(host ? g_eidHostTarget : g_eidClientTarget, host ? "HOST" : "CLIENT");
    }
    if (g_fired && !g_verdictDone && now >= g_verdictAtMs) {
        g_verdictDone = true;
        Verdict(host);
    }
    if (now >= g_nextDigestMs) {
        g_nextDigestMs = now + kDigestEveryMs;
        Digest();
    }
}

void OnDisconnect() {
    if (!Enabled()) return;
    g_connectedAtMs = 0;
    g_nextDigestMs = 0;
    g_fired = false;
    g_verdictDone = false;
    g_verdictAtMs = 0;
    g_fireBefore = g_fireAfter = -1;
    g_fireEid = 0;
    g_eidHostTarget = g_eidClientTarget = 0;
    g_anchorUnread = false;
}

}  // namespace coop::dev::container_selftest

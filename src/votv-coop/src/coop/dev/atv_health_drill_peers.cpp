// coop/dev/atv_health_drill_peers.cpp -- the ATV health drill's peers arms: see coop/dev/atv_health_drill.h. Each arm
// runs on an ATV the host spawns beside the players (the spawn and park steps of atv_health_drill_spawned.cpp) and
// judges one rule of the lane that had no arm: carry_still (a carried ATV standing still is not handed back, and is
// once released), synced_peer (a second client's grab of a carried ATV is refused and dropped), join (a peer joining
// under a seated client takes the seat and the pose from its connect snapshot) and stream_pace (a syncer's burst of
// blocks is paced by the host's bucket). The first two grab, the last two sit. synced_peer, join and stream_pace run
// on three peers: slot 2 is the observer, a client that only watches or grabs. The peers sequence on lobby-chat lines,
// each latched on sight; every wait ends on the line or the state it waits for, and a bound only says INVALID.

#include "atv_health_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/drill_chat.h"
#include "coop/interactables/atv_authority.h"
#include "coop/interactables/atv_condition_sync.h"
#include "coop/interactables/atv_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/player/players_registry.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace coop::dev::atv_health_drill::detail {

namespace {

namespace A  = ue_wrap::atv;
namespace AU = coop::atv_authority;
namespace DC = coop::dev::drill_chat;
namespace E  = ue_wrap::engine;
using coop::atv_sync::TableView;
using coop::net::AtvRefusal;

constexpr uint8_t  kObserverSlot = 2;
// Counted not clocked: the stream sends a block every 50 ms (kDriveSendMs), the rest rule needs kRestMs = 1 s of rest and
// the host's pass runs once a second, so a hand-back with the grab bit ignored lands 1.05-2.05 s after the first still
// block. 70 blocks (about 3.5 s) cover that whole span, so the carry_back red fails in every phase of the pass and a
// hand-back broken to fire late still shows inside the window.
constexpr uint64_t kCarryBlocks = 70;
constexpr int      kBurst = 100;        // blocks the stream_pace client sends in one tick
constexpr uint32_t kBucket = 16;        // the stream bucket's burst: what one tick's blocks may carry through
// The host's tolerance on the burst's paced count: the bucket refills one token per 25 ms (kStreamBudget, 40/s) while
// the burst is received, so a few more than kBucket pass; 10 covers 250 ms of refill. A missing bucket paces none.
constexpr uint32_t kHostPaceSlack = 10;
// The observer's tolerance on its estimate of the normal stream over the burst's window; an unpaced burst clears it by
// kBurst - kBucket - kPaceSlack blocks.
constexpr uint32_t kPaceSlack = 15;
// The observer's baseline is a run of syncer blocks it counts itself (the seated client streams one every 50 ms), long
// enough that the rate it reads is the stream's and not one tick's.
constexpr uint32_t kBaselineBlocks = 20;

unsigned long long U(uint64_t v) { return static_cast<unsigned long long>(v); }

bool NeedsObserver(Arm a) { return a == Arm::SyncedPeer || a == Arm::StreamPace; }
bool IsGrabArm(Arm a) { return a == Arm::CarryStill || a == Arm::SyncedPeer; }
bool ThreePeers(Arm a) { return a != Arm::CarryStill; }

std::string ObserverLine(const char* what) { return std::string("ATVHD observer ") + what; }

// A line the other peers say, latched on sight: a live chat row retires after seconds.
void Latch(bool& seen, const std::string& line) {
    if (!seen) seen = DC::ChatHasLive(line);
}

// ---- the host ----

enum class PH : int { Wait, Spawn, Park, Held, Release, Carry, Judge, Rejoin, Seated, Paced };

struct PHost {
    Run                run;
    SpawnStep          sp;
    bool               clientReady = false, observerReady = false;
    bool               saw = false;       // the phase's client line, latched
    bool               observerBaselined = false;   // stream_pace: the observer's normal-rate baseline is taken
    AU::Counters       base{};            // the lane's counters at the phase's baseline
    Clock::time_point  heldAt{};
    uint64_t           idleAtRelease = 0; // the lane's idle-stream blocks when the release began
    RestRun            rest;              // the idle-stream blocks at rest since then
};
PHost g_ph;

void HostPhase(PH p, const char* name) {
    Enter(g_ph.run, static_cast<int>(p), name);
    g_ph.saw = false;
}

void HostFail(Arm a, const std::string& what, const std::string& got, const std::string& want) {
    Fail("host", a, g_ph.run, what, got, want);
}

// A bound that ran out with no event to decide on.
void HostInvalid(Arm a, const std::string& what, const std::string& got, const std::string& want) {
    Invalid("host", a, g_ph.run, what, got, want);
}

// The start: the client's ready line, and for the arms where the observer acts at once its own.
void HostWait(Arm a, bool due) {
    PHost& h = g_ph;
    if (due) {
        Latch(h.clientReady, ReadyLine(a));
        Latch(h.observerReady, ReadyLine(a, "observer"));
    }
    if (h.clientReady && (!NeedsObserver(a) || h.observerReady)) return HostPhase(PH::Spawn, "spawn");
    if (Past(h.run, kHostFirstBound))
        HostInvalid(a, h.clientReady ? "the observer never said it was ready" : "the client never said it was ready",
                    "no line", ReadyLine(a, h.clientReady ? "observer" : "client"));
}

void AfterPark(Arm a) {
    switch (a) {
    case Arm::CarryStill: return HostPhase(PH::Held, "held");
    case Arm::SyncedPeer: return HostPhase(PH::Carry, "carry");
    case Arm::Join:       return HostPhase(PH::Rejoin, "rejoin");
    default:              return HostPhase(PH::Seated, "seated");
    }
}

// carry_still: from the client's `holding` line, 70 blocks of the carry standing still and no hand-back while it holds.
void HostHeld(Arm a, const TableView* t, bool due) {
    PHost& h = g_ph;
    if (!h.saw) {
        if (due) Latch(h.saw, ClientLine("holding"));
        if (h.saw) {
            h.base = AU::ReadCounters();
            h.run.at = Clock::now();   // the bound runs from the hold
        } else if (Past(h.run, kHostFirstBound)) {
            HostInvalid(a, "the client never said it held the ATV", "no line", ClientLine("holding"));
        }
        return;
    }
    const AU::Counters now = AU::ReadCounters();
    const uint64_t back = now.handedBack - h.base.handedBack;
    const uint64_t still = now.carriedStillBlocks - h.base.carriedStillBlocks;
    if (back != 0)
        return HostFail(a, "the carried ATV was handed back while the client held it",
                        "handedBack " + Plus(back) + " carried_still " + Plus(still), "handedBack +0");
    if (still >= kCarryBlocks && t && t->syncerSlot == kSlot) {
        h.heldAt = Clock::now();
        h.idleAtRelease = now.idleStreamBlocks;
        DC::Say(PhaseLine("held"));
        return HostPhase(PH::Release, "release");
    }
    if (Past(h.run, kFailBound)) {
        char got[96];
        std::snprintf(got, sizeof(got), "carried_still +%llu syncer %u", U(still),
                      t ? static_cast<unsigned>(t->syncerSlot) : 0xFFFFu);
        HostInvalid(a, "the carry never showed 70 still blocks to decide on", got, "carried_still +70 syncer 1");
    }
}

// The release: the ATV goes back to the host at rest, by one hand-back.
void HostRelease(Arm a, const TableView* t) {
    PHost& h = g_ph;
    const AU::Counters now = AU::ReadCounters();
    const uint64_t back = now.handedBack - h.base.handedBack;
    if (t && t->syncerSlot == 0) {
        if (back != 1)
            return HostFail(a, "the ATV went back to the host by no at-rest hand-back", "handedBack " + Plus(back),
                            "handedBack +1");
        DC::Say(PhaseLine("back"));
        char nums[64];
        std::snprintf(nums, sizeof(nums), "release_to_back_ms=%lld", Ms(h.heldAt, Clock::now()));
        return Pass("host", a, h.run, nums);
    }
    // The hand-back at rest lands within about two seconds of the release: 70 STILL idle-stream blocks (speed under the lane's
    // rest bound, the idle arm's count, HostBack) admitted with the ATV still the client's are a hand-back that did not
    // happen, the event. Blocks that were moving count for nothing. Having no stream to count says INVALID at the bound.
    const uint64_t idle = now.idleStreamBlocks - h.idleAtRelease;
    if (t) CountRest(h.rest, idle, t->lastBlockSpeed);
    const bool decided = t && h.rest.blocks >= kRestBlocks;
    if (!decided && !Past(h.run, kFailBound)) return;
    const std::string got = "handedBack " + Plus(back) + " syncer " + (t ? std::to_string(t->syncerSlot) : "?") +
                            " idle_blocks " + Plus(idle) + " rest_blocks " + std::to_string(h.rest.blocks);
    if (decided)
        return HostFail(a, "the released ATV was not handed back at rest", got, "handedBack +1 syncer 0");
    HostInvalid(a, "the idle stream never held long enough to decide the hand-back", got, "handedBack +1 syncer 0");
}

// synced_peer: from the client's `carrying` line with the client the syncer, the baseline.
void HostCarry(Arm a, const TableView* t, bool due) {
    PHost& h = g_ph;
    if (due) Latch(h.saw, ClientLine("carrying"));
    if (h.saw && t && t->syncerSlot == kSlot) {
        h.base = AU::ReadCounters();
        h.run.at = Clock::now();
        return HostPhase(PH::Judge, "judge");
    }
    if (Past(h.run, kHostFirstBound))
        HostInvalid(a, "the client never carried the ATV", t ? "syncer " + std::to_string(t->syncerSlot) : "no table",
                    "the client the syncer");
}

// The observer's grab is judged: refused as synced, the carry kept, nothing admitted since the baseline.
void HostJudge(Arm a, const TableView* t) {
    PHost& h = g_ph;
    const AU::Counters now = AU::ReadCounters();
    const uint64_t admitted = now.admitted - h.base.admitted;
    const unsigned s = static_cast<unsigned>(AtvRefusal::Synced) & 15u;
    const uint64_t synced = now.refused[s] - h.base.refused[s];
    const std::string got = "admitted " + Plus(admitted) + " refused_synced " + Plus(synced) + " syncer " +
                            (t ? std::to_string(t->syncerSlot) : "?");
    if (admitted != 0)
        return HostFail(a, "the second client's takeover of the carried ATV was granted", got,
                        "admitted +0 refused_synced +1 syncer 1");
    if (synced != 0) {
        if (synced != 1 || !t || t->syncerSlot != kSlot)
            return HostFail(a, "the second client's takeover was not refused once with the carry kept", got,
                            "admitted +0 refused_synced +1 syncer 1");
        DC::Say(PhaseLine("back"));
        return Pass("host", a, h.run, "refused_synced=1");
    }
    if (Past(h.run, kHostFirstBound))
        HostInvalid(a, "the second client's takeover was never judged", got, "admitted +0 refused_synced +1 syncer 1");
}

// join: once the observer is in and the client seated, the host says the key again (a joiner holds no chat history).
void HostRejoin(Arm a, const TableView* t, bool due) {
    PHost& h = g_ph;
    if (due) Latch(h.observerReady, ReadyLine(a, "observer"));
    const bool seated = t && t->occupantSlot == kSlot && t->syncerSlot == kSlot;
    if (h.observerReady && seated) {
        SayKey(h.sp.key);
        return Pass("host", a, h.run, "");
    }
    if (Past(h.run, kHostFirstBound))
        HostInvalid(a, h.observerReady ? "the table never named the client seated" : "the observer never joined",
                    h.observerReady ? "not seated" : "no line", h.observerReady ? "occupant 1 syncer 1" : ReadyLine(a, "observer"));
}

// stream_pace: the client seated and the syncer and the observer's baseline taken, the host's baseline, and the word to
// burst (the observer measures the normal stream between the client's `sat` and its own `baselined`).
void HostSeated(Arm a, const TableView* t, bool due) {
    PHost& h = g_ph;
    if (due) {
        Latch(h.saw, ClientLine("sat"));
        Latch(h.observerBaselined, ObserverLine("baselined"));
    }
    if (h.saw && h.observerBaselined && t && t->occupantSlot == kSlot && t->syncerSlot == kSlot) {
        h.base = AU::ReadCounters();
        h.run.at = Clock::now();
        DC::Say(PhaseLine("seated"));
        return HostPhase(PH::Paced, "paced");
    }
    if (Past(h.run, kHostFirstBound))
        HostInvalid(a, h.saw ? "the observer never took its stream baseline" : "the client never sat in the spawned ATV",
                    "no line", h.saw ? ObserverLine("baselined") : ClientLine("sat"));
}

// The burst is paced: the bucket refused all but its burst, and no block was refused as not the syncer's. The client's
// `burst` line follows its blocks on the same lane, so a count short of the burst once it is seen is the verdict.
void HostPaced(Arm a, bool due) {
    PHost& h = g_ph;
    const AU::Counters now = AU::ReadCounters();
    const uint64_t paced = now.streamPaced - h.base.streamPaced;
    const uint64_t notSyncer = now.notSyncer - h.base.notSyncer;
    const std::string got = "stream_paced " + Plus(paced) + " not_syncer " + Plus(notSyncer);
    if (notSyncer != 0)
        return HostFail(a, "a block of the seated syncer was refused as not its own", got, "not_syncer +0");
    if (paced >= static_cast<uint64_t>(kBurst) - kBucket - kHostPaceSlack) {
        DC::Say(PhaseLine("paced"));
        char nums[48];
        std::snprintf(nums, sizeof(nums), "stream_paced=%llu", U(paced));
        return Pass("host", a, h.run, nums);
    }
    // The client's `burst` line follows its blocks on the lane, so once it is seen the count is whole and short of the burst
    // is the FAIL; without the line the bound says INVALID.
    if (due) Latch(h.saw, ClientLine("burst"));
    if (h.saw) return HostFail(a, "the burst was not paced by the bucket", got, "stream_paced +74");
    if (Past(h.run, kFailBound)) HostInvalid(a, "the client never said its burst was sent", got, "stream_paced +74");
}

// ---- the client (slot 1) and the observer (slot 2) ----

enum class PC : int { Wait, Walk, Act, Hold, Release, Back };

struct PPeer : SeatedClient {
    bool                                       readySaid = false, haveKey = false;
    std::wstring                               key;                // the host's ATV key, from its chat line
    bool                                       sawParked = false, sawHeld = false, sawBack = false, sawSeated = false;
    bool                                       sawPaced = false, sawCarrying = false, sawSat = false;
    std::unique_ptr<coop::director::AimedGrab> grab;
    Clock::time_point                          nextTry{}, bootAt{};
    bool                                       haveA0 = false, haveBase = false, haveA1 = false;   // the observer's stream samples
    uint32_t                                   ab = 0;   // and the count at the end of the baseline run
    Clock::time_point                          tb{};
    uint32_t                                   a0 = 0, a1 = 0;
    Clock::time_point                          t0{}, t1{};
};
PPeer g_pc;
PPeer MakeObserver() {
    PPeer p;
    p.role = "observer";
    return p;
}
PPeer g_po = MakeObserver();
bool IsObserver(const PPeer& p) { return &p == &g_po; }

void PEnter(PPeer& p, PC ph, const char* name) { Enter(p.run, static_cast<int>(ph), name); }

void PEnd(PPeer& p) {
    WalkCancel(p);
    p.grab.reset();
}

void PFail(PPeer& p, Arm a, const std::string& what, const std::string& got, const std::string& want) {
    Fail(p.role, a, p.run, what, got, want);
    PEnd(p);
}

// A bound that ran out with no event to decide on.
void PInvalid(PPeer& p, Arm a, const std::string& what, const std::string& got, const std::string& want) {
    Invalid(p.role, a, p.run, what, got, want);
    PEnd(p);
}

// The lines this peer reads, polled together on a due tick.
void PollLines(PPeer& p) {
    Latch(p.sawParked, PhaseLine("parked"));
    Latch(p.sawHeld, PhaseLine("held"));
    Latch(p.sawBack, PhaseLine("back"));
    Latch(p.sawSeated, PhaseLine("seated"));
    Latch(p.sawPaced, PhaseLine("paced"));
    Latch(p.sawCarrying, ClientLine("carrying"));
    Latch(p.sawSat, ClientLine("sat"));
}

void PassWalk(PPeer& p, Arm a) {
    char nums[48];
    std::snprintf(nums, sizeof(nums), "walk_ms=%lld", Ms(p.walkAt, p.arrivedAt));
    Pass(p.role, a, p.run, nums);
    PEnd(p);
}

// Ready once the streams run, then the host's key line.
bool ReadyAndKey(PPeer& p, Arm a, bool due) {
    if (!p.readySaid) {
        if (StreamsRun()) {
            p.readySaid = true;
            DC::Say(ReadyLine(a, p.role));
        }
    } else if (!p.haveKey && due && KeyFromChat(&p.key)) {
        p.haveKey = true;
        UE_LOGI("[ATV-HEALTH-DRILL] %s arm=%s the host's ATV key is '%ls'", p.role, SpecOf(a).name, p.key.c_str());
    }
    return p.haveKey;
}

// The walking peers: the key, the host's go (its `parked` for the client, the client's `carrying` for the observer),
// the table showing the ATV, then the director's walk.
void PeerWait(PPeer& p, Arm a, bool due) {
    const bool observer = IsObserver(p);
    const bool go = observer ? p.sawCarrying : p.sawParked;
    if (ReadyAndKey(p, a, due) && go) {
        coop::atv_sync::TableView t;
        ue_wrap::FRotator rot{};
        AtvAt at;
        if (coop::atv_sync::ReadTable(p.key, t) && A::GetRootTransform(t.actor, at.at, rot)) {
            at.actor = t.actor;
            WalkStart(p, a, at, IsGrabArm(a) ? kGrabReachCm : kSitReachCm);
            return PEnter(p, PC::Walk, "walk");
        }
    }
    if (Past(p.run, kHostFirstBound))
        PInvalid(p, a, "the host's ATV never came to this peer's table", "no row", "a row under the host's key");
}

void PeerWalk(PPeer& p, Arm a, void* player) {
    const Stage s = WalkStep(p, a);
    if (s == Stage::Failed) return PEnd(p);
    if (s == Stage::Wait) return;
    if (IsGrabArm(a)) p.grab = std::make_unique<coop::director::AimedGrab>(player, p.atv.Get());
    PEnter(p, PC::Act, SpecOf(a).act);
}

// The grav hand's grab until the player's own grabbing actor is the ATV.
void PeerGrab(PPeer& p, Arm a) {
    switch (p.grab->Tick()) {
    case coop::director::GrabState::Working:
        if (Past(p.run, kFailBound)) PInvalid(p, a, "the grab never finished", "working", "grabbed");
        return;
    case coop::director::GrabState::Failed:
        return PFail(p, a, GrabWhy(*p.grab), "failed", "grabbed");
    case coop::director::GrabState::Grabbed:
        p.grab.reset();
        DC::Say(IsObserver(p) ? ObserverLine("grabbed") : ClientLine(a == Arm::CarryStill ? "holding" : "carrying"));
        return PEnter(p, PC::Hold, "hold");
    }
}

// The seat through the game's own verb, then the host's grant.
void PeerSit(PPeer& p, Arm a, void* player, uint8_t self) {
    void* atv = p.atv.Get();
    if (!atv) return PFail(p, a, "the spawned ATV is gone before the sit", "no ATV", "the spawned ATV");
    const Stage s = SitStep(p, a, player, atv, self);
    if (s == Stage::Failed) return PEnd(p);
    if (s == Stage::Wait) return;
    if (a == Arm::Join) {
        UE_LOGI("[ATV-HEALTH-DRILL] client SEATED arm=%s", SpecOf(a).name);   // the rig's readiness for the observer
        return PassWalk(p, a);
    }
    DC::Say(ClientLine("sat"));
    PEnter(p, PC::Hold, "burst");
}

// stream_pace: 100 blocks of this client's own copy in one tick, raw on the reliable lane.
void PeerBurst(coop::net::Session* s, PPeer& p, Arm a) {
    void* atv = p.atv.Get();
    ue_wrap::FVector loc{};
    ue_wrap::FRotator rot{};
    if (!atv || !A::GetRootTransform(atv, loc, rot))
        return PFail(p, a, "the seated ATV does not read", "no transform", "a transform");
    coop::net::AtvStatePayload pl{};
    coop::net::WireKeyFromString(p.key, pl.key);
    pl.x = loc.X; pl.y = loc.Y; pl.z = loc.Z;
    pl.pitch = rot.Pitch; pl.yaw = rot.Yaw; pl.roll = rot.Roll;
    pl.occupantSlot = kSlot;
    pl.syncerSlot = kSlot;
    pl.stateBits = A::IsDriven(atv) ? 0x1 : 0x0;
    coop::atv_condition_sync::FillPayload(atv, pl);
    int sent = 0;
    for (int i = 0; i < kBurst; ++i)
        if (s->SendReliable(coop::net::ReliableKind::AtvState, &pl, sizeof(pl))) ++sent;
    if (sent != kBurst) return PFail(p, a, "the burst did not leave", "sent " + std::to_string(sent), "sent 100");
    DC::Say(ClientLine("burst"));
    PEnter(p, PC::Back, "paced");
}

// On the host's word, the grab let go of: tried once a second until the player is no longer grabbing the ATV.
void PeerRelease(PPeer& p, Arm a, void* player) {
    void* atv = p.atv.Get();
    if (!atv) return PFail(p, a, "the ATV is gone before the release", "no ATV", "the spawned ATV");
    if (!E::IsMainPlayerGrabbing(player, atv)) {
        if (a == Arm::SyncedPeer) return PassWalk(p, a);
        return PEnter(p, PC::Back, "back");
    }
    if (Past(p.run, kFailBound)) return PInvalid(p, a, "the release never let go", "grabbing", "not grabbing");
    const Clock::time_point now = Clock::now();
    if (now < p.nextTry) return;
    p.nextTry = now + std::chrono::seconds(1);
    E::ReleaseMainPlayerGrabIfHolding(player, atv);
}

// The hold: the client waits for the host's word, the observer for its refused grab to be dropped.
void PeerHold(coop::net::Session* s, PPeer& p, Arm a, void* player) {
    const bool observer = IsObserver(p);
    if (observer) {
        void* atv = p.atv.Get();
        if (!atv) return PFail(p, a, "the ATV is gone before the grab was judged", "no ATV", "the spawned ATV");
        if (!E::IsMainPlayerGrabbing(player, atv) && p.sawBack) return PassWalk(p, a);
        if (Past(p.run, kFailBound))
            PInvalid(p, a, "the refused grab was not dropped", p.sawBack ? "grabbing" : "no host verdict", "dropped");
        return;
    }
    const bool go = a == Arm::CarryStill ? p.sawHeld : a == Arm::SyncedPeer ? p.sawBack : p.sawSeated;
    if (go) {
        if (a == Arm::StreamPace) return PeerBurst(s, p, a);
        return PEnter(p, PC::Release, "release");
    }
    if (Past(p.run, kHostFirstBound)) PInvalid(p, a, "the host never judged the hold", "no line", "the host's phase line");
}

void PeerBack(PPeer& p, Arm a) {
    if (a == Arm::StreamPace ? p.sawPaced : p.sawBack) return PassWalk(p, a);
    if (Past(p.run, kFailBound)) PInvalid(p, a, "the host never said it was done", "no line", "the host's phase line");
}

// join, on the observer: the key line (the host's second, said when this peer is in), the table, and the snapshot.
void ObserverJoin(PPeer& p, Arm a, bool due) {
    if (ReadyAndKey(p, a, due)) {
        TableView t;
        if (coop::atv_sync::ReadTable(p.key, t)) {
            char got[96];
            std::snprintf(got, sizeof(got), "occupant %u syncer %u snapshot_reached %d",
                          static_cast<unsigned>(t.occupantSlot), static_cast<unsigned>(t.syncerSlot),
                          t.snapshotReached ? 1 : 0);
            if (!t.snapshotReached || t.occupantSlot != kSlot || t.syncerSlot != kSlot)
                return PFail(p, a, "the connect snapshot did not give the spawned ATV's seat and pose", got,
                             "snapshot_reached 1 occupant 1 syncer 1");
            char nums[64];
            std::snprintf(nums, sizeof(nums), "boot_to_judge_ms=%lld", Ms(p.bootAt, Clock::now()));
            return Pass(p.role, a, p.run, nums);
        }
    }
    if (Past(p.run, kHostFirstBound))
        PInvalid(p, a, "the host's ATV never came to the joiner's table", "no row", "a row under the host's key");
}

// stream_pace, on the observer: the normal stream's rate over a run of kBaselineBlocks syncer blocks counted from the
// client's `sat` (then `baselined` is said, which the host waits for before `seated`), then the blocks placed from the
// host's `seated` to its `paced` against that rate and the bucket's burst.
void ObserverPace(PPeer& p, Arm a, bool due) {
    if (!p.haveKey) {
        if (ReadyAndKey(p, a, due)) PEnter(p, PC::Hold, "pace");
        if (Past(p.run, kHostFirstBound)) PInvalid(p, a, "the host's key never came", "no line", "the host's key line");
        return;
    }
    const Clock::time_point now = Clock::now();
    if (!p.haveA0 && p.sawSat) { p.a0 = coop::atv_sync::AppliedFromSyncer(); p.t0 = now; p.haveA0 = true; }
    if (p.haveA0 && !p.haveBase && coop::atv_sync::AppliedFromSyncer() - p.a0 >= kBaselineBlocks) {
        p.ab = coop::atv_sync::AppliedFromSyncer();
        p.tb = now;
        p.haveBase = true;
        DC::Say(ObserverLine("baselined"));
    }
    if (p.haveBase && !p.haveA1 && p.sawSeated) { p.a1 = coop::atv_sync::AppliedFromSyncer(); p.t1 = now; p.haveA1 = true; }
    if (p.haveA1 && p.sawPaced) {
        const uint32_t window = coop::atv_sync::AppliedFromSyncer() - p.a1;
        const uint32_t before = p.ab - p.a0;
        const double perMs = static_cast<double>(before) / static_cast<double>(std::max<long long>(1, Ms(p.t0, p.tb)));
        const uint32_t normal = static_cast<uint32_t>(std::ceil(perMs * static_cast<double>(Ms(p.t1, now))));
        char got[96];
        std::snprintf(got, sizeof(got), "applied %u over a normal stream of about %u", window, normal);
        if (window > kBucket + normal + kPaceSlack)
            return PFail(p, a, "the observer saw the unpaced burst", got, "applied <= bucket 16 + normal stream");
        char nums[96];
        std::snprintf(nums, sizeof(nums), "burst_applied=%u normal_est=%u", window, normal);
        return Pass(p.role, a, p.run, nums);
    }
    if (!Past(p.run, kHostFirstBound)) return;
    if (!p.haveBase)
        return PInvalid(p, a, "the observer counted too few syncer blocks for its baseline",
                        std::to_string(p.haveA0 ? coop::atv_sync::AppliedFromSyncer() - p.a0 : 0u) + " blocks",
                        std::to_string(kBaselineBlocks) + " blocks");
    if (!p.sawSeated)
        return PInvalid(p, a, "the host never said the baseline was taken and the client seated", "no line",
                        PhaseLine("seated"));
    PInvalid(p, a, "the host never said the burst was judged", "no line", PhaseLine("paced"));
}

// One peer's tick, after its head: the observer of join and stream_pace has its own wait, every other peer walks.
void PeerTick(coop::net::Session* s, PPeer& p, Arm a, void* player, uint8_t self) {
    if (p.run.at == Clock::time_point{}) PEnter(p, PC::Wait, "wait");   // the bounds run from the world-ready edge
    const bool due = DC::PollDue();
    if (due) PollLines(p);
    const bool observer = IsObserver(p);
    switch (static_cast<PC>(p.run.phase)) {
    case PC::Wait:
        if (observer && a == Arm::Join) return ObserverJoin(p, a, due);
        if (observer && a == Arm::StreamPace) return ObserverPace(p, a, due);
        return PeerWait(p, a, due);
    case PC::Walk:    return PeerWalk(p, a, player);
    case PC::Act:
        if (!p.atv.Get()) return PFail(p, a, "the ATV is gone before the act", "no ATV", "the spawned ATV");
        return IsGrabArm(a) ? PeerGrab(p, a) : PeerSit(p, a, player, self);
    case PC::Hold:
        if (observer && a == Arm::StreamPace) return ObserverPace(p, a, due);
        return PeerHold(s, p, a, player);
    case PC::Release: return PeerRelease(p, a, player);
    case PC::Back:    return PeerBack(p, a);
    }
}

}  // namespace

void PeersHostTick(coop::net::Session* s, Arm a) {
    PHost& h = g_ph;
    if (h.run.done) return;
    if (h.run.at == Clock::time_point{}) {   // the bounds run from the edge of slot 1 being in the world
        if (!s->IsSlotWorldReady(kSlot)) return;
        HostPhase(PH::Wait, "wait");
    }
    const bool due = DC::PollDue();
    TableView t;
    const bool read = !h.sp.key.empty() && coop::atv_sync::ReadTable(h.sp.key, t);
    const TableView* view = read ? &t : nullptr;
    switch (static_cast<PH>(h.run.phase)) {
    case PH::Wait:    return HostWait(a, due);
    case PH::Spawn:
        if (SpawnAtvStep(h.sp, h.run, a) == Stage::Next) HostPhase(PH::Park, "park");
        return;
    case PH::Park:
        if (ParkAtvStep(h.sp, h.run, a) == Stage::Next) AfterPark(a);
        return;
    case PH::Held:    return HostHeld(a, view, due);
    case PH::Release: return HostRelease(a, view);
    case PH::Carry:   return HostCarry(a, view, due);
    case PH::Judge:   return HostJudge(a, view);
    case PH::Rejoin:  return HostRejoin(a, view, due);
    case PH::Seated:  return HostSeated(a, view, due);
    case PH::Paced:   return HostPaced(a, due);
    }
}

void PeersClientTick(coop::net::Session* s, Arm a) {
    const bool observer = coop::players::Registry::Get().LocalPeerId() == kObserverSlot && ThreePeers(a);
    void* player = nullptr;
    uint8_t self = 0;
    if (observer) {
        PPeer& p = g_po;
        if (p.bootAt == Clock::time_point{}) p.bootAt = Clock::now();   // boot_to_judge_ms counts from the first tick
        if (p.run.done) return;
        player = coop::players::Registry::Get().Local();
        self = kObserverSlot;
        if (!coop::net_pump::HasAnnouncedWorldReady() || !player) return;
        return PeerTick(s, p, a, player, self);
    }
    if (!SeatedActs(g_pc, &player, &self)) return;
    PeerTick(s, g_pc, a, player, self);
}

void PeersReset() {
    PEnd(g_pc);
    PEnd(g_po);
    g_ph = PHost{};
    g_pc = PPeer{};
    g_po = MakeObserver();
}

}  // namespace coop::dev::atv_health_drill::detail

// coop/dev/atv_health_drill_arms.cpp -- the ATV health drill's arms where the client acts, `sit`, `grab` and `silent`,
// and the client's side of `occupied` and `synced`, for both roles: see coop/dev/atv_health_drill.h. The client walks
// to the ATV the host spawns (parked, its key said in the lobby chat) with the director and acts on it (the seat through
// the game's own verb, the grav hand's grab, or a raw request) and undoes it on the host's word; the host reads the
// table it judged and the lane's counters as deltas against a baseline taken once the ATV is parked, says each edge
// in the lobby chat, and prints the times the hand-back took. The host's side of `occupied` and `synced` is
// atv_health_drill_host.cpp.

#include "atv_health_drill_detail.h"  // co-located private header (src tree, not include/)

#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat.h"
#include "coop/interactables/atv_authority.h"
#include "coop/interactables/atv_sync.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/net/wire_key_util.h"
#include "coop/props/join_membership_sweep.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/devices/atv.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace coop::dev::atv_health_drill::detail {

Host g_h;

std::string Plus(uint64_t n) { return "+" + std::to_string(n); }

// The grab's failure: why, and which calls of the use key's chain ran.
std::string GrabWhy(const coop::director::AimedGrab& g) {
    char what[200];
    std::snprintf(what, sizeof(what), "the grab did not take: %s, chain pre/use/post %d/%d/%d", g.Why(),
                  g.ChainPre() ? 1 : 0, g.ChainUse() ? 1 : 0, g.ChainPost() ? 1 : 0);
    return what;
}

// The client acts once its own streams run, as a player in a settled world does.
bool StreamsRun() {
    return coop::net_pump::HasAnnouncedWorldReady() &&
           coop::join_progress::CurrentPhase() == coop::join_progress::Phase::Idle &&
           coop::net_pump::IsWorldSettled() && coop::join_membership_sweep::HasLoadTailQuiesced();
}

namespace {

namespace A  = ue_wrap::atv;
namespace AU = coop::atv_authority;
namespace DC = coop::dev::drill_chat;
namespace E  = ue_wrap::engine;
using coop::atv_sync::TableView;

unsigned long long U(uint64_t v) { return static_cast<unsigned long long>(v); }

// ---- the host ----

uint64_t Admitted() { return AU::ReadCounters().admitted - g_h.base.admitted; }
uint64_t HandedBack() { return AU::ReadCounters().handedBack - g_h.base.handedBack; }

// The times the hand-back took, then the stream's cost: the idle blocks admitted after the act and the speed of the
// last block, the numbers the next increments are written on.
std::string Numbers(Arm a, Clock::time_point end, float lastBlockSpeed) {
    char buf[200];
    int  n = 0;
    if (a == Arm::Sit)
        n = std::snprintf(buf, sizeof(buf), "seated_ms=%lld leave_to_back_ms=%lld ", Ms(g_h.firstAt, g_h.leftAt),
                          Ms(g_h.leftAt, end));
    else
        n = std::snprintf(buf, sizeof(buf), "release_to_back_ms=%lld ", Ms(g_h.firstAt, end));
    std::snprintf(buf + n, sizeof(buf) - static_cast<size_t>(n), "idle_stream_blocks=%llu last_block_speed=%.2f",
                  U(AU::ReadCounters().idleStreamBlocks - g_h.base.idleStreamBlocks), lastBlockSpeed);
    return buf;
}

// The client's ready line starts the spawn (the ATV then reaches its table by the lane's own path); once the ATV is
// parked the baseline is taken and the arm's first phase begins.
void HostWait(Arm a) {
    if (DC::PollDue() && DC::ChatHasLive(ReadyLine(a))) return HostEnter(HP::Spawn, "spawn");
    if (Past(g_h.run, kFailBound))
        Invalid("host", a, g_h.run, "the client never said it was ready", "no line", ReadyLine(a));
}

void HostPark(Arm a) {
    if (ParkAtvStep(g_h.sp, g_h.run, a) != Stage::Next) return;
    g_h.base = AU::ReadCounters();
    if (a == Arm::Occupied || a == Arm::Synced) return HostEnter(HP::Walk, "walk");
    HostEnter(HP::First, SpecOf(a).first);
}

// The edge the client's act makes: the sit (and the silent arm's) names the client occupant and syncer, the grab names
// it syncer with the seat free (and `admitted` +1, the takeover). The host then says the edge, which the client undoes
// its act on.
void HostFirst(Arm a, const TableView* t) {
    const bool sit = a == Arm::Sit || a == Arm::Silent;
    if (t && t->syncerSlot == kSlot && t->occupantSlot == (sit ? kSlot : kNoSeat)) {
        if (!sit && Admitted() != 1)
            return Fail("host", a, g_h.run, "the grant", "admitted " + Plus(Admitted()), "admitted +1");
        g_h.firstAt = Clock::now();
        DC::Say(PhaseLine(SpecOf(a).first));
        if (a == Arm::Silent) return HostEnter(HP::Silenced, "silenced");
        return HostEnter(sit ? HP::Left : HP::Back, sit ? "left" : "back");
    }
    if (!Past(g_h.run, kHostFirstBound)) return;
    char got[64];
    std::snprintf(got, sizeof(got), "occupant %u syncer %u", t ? static_cast<unsigned>(t->occupantSlot) : 0xFFFFu,
                  t ? static_cast<unsigned>(t->syncerSlot) : 0xFFFFu);
    Invalid("host", a, g_h.run, sit ? "the table never named the client occupant and syncer"
                                     : "the table never named the client syncer with the seat free",
             t ? got : "no table", sit ? "occupant 1 syncer 1" : "occupant 255 syncer 1");
}

// The leave: the seat is free again, the sit and the leave being the two admitted requests. The lane frees the seat in
// the call that admits the leave, so the second admission is the event: the seat read after it is free, or it is a
// FAIL. A leave that is never admitted has no event, and the bound says INVALID.
void HostLeft(Arm a, const TableView* t) {
    if (t && t->occupantSlot == kNoSeat) {
        if (Admitted() != 2)
            return Fail("host", a, g_h.run, "the sit and the leave", "admitted " + Plus(Admitted()), "admitted +2");
        g_h.leftAt = Clock::now();
        return HostEnter(HP::Back, "back");
    }
    if (t && Admitted() >= 2)
        return Fail("host", a, g_h.run, "the admitted leave did not free the seat",
                    "occupant " + std::to_string(t->occupantSlot), "occupant 255");
    if (Past(g_h.run, kFailBound))
        Invalid("host", a, g_h.run, "no leave request was admitted", "admitted " + Plus(Admitted()), "admitted +2");
}

// The ATV back with the host after the act is undone: the syncer is the host and the lane counted one hand-back at rest.
// A red that skips the hand-back leaves the client the syncer: the verdict is the stream's own count of blocks at rest
// past what the hand-back needed. Having no stream to count says INVALID at the bound.
void HostBack(Arm a, const TableView* t) {
    const Clock::time_point now = Clock::now();
    if (t && t->syncerSlot == 0) {
        if (HandedBack() != 1)
            return Fail("host", a, g_h.run, "the ATV went back to the host by no at-rest hand-back",
                        "handedBack " + Plus(HandedBack()), "handedBack +1");
        DC::Say(PhaseLine("back"));
        return Pass("host", a, g_h.run, Numbers(a, now, t->lastBlockSpeed));
    }
    if (t) CountRest(g_h.rest, AU::ReadCounters().idleStreamBlocks - g_h.base.idleStreamBlocks, t->lastBlockSpeed);
    const bool decided = t && g_h.rest.blocks >= kRestBlocks;
    if (!decided && !Past(g_h.run, kFailBound)) return;
    const std::string got = "handedBack " + Plus(HandedBack()) + " syncer " + (t ? std::to_string(t->syncerSlot) : "?") +
                            " rest_blocks " + std::to_string(g_h.rest.blocks) + ", " +
                            Numbers(a, now, t ? t->lastBlockSpeed : 0.f);
    if (decided)
        return Fail("host", a, g_h.run, "the ATV was not handed back to the host at rest", got, "handedBack +1 syncer 0");
    Invalid("host", a, g_h.run, "the idle stream never held at rest long enough to decide the hand-back", got,
            "handedBack +1 syncer 0");
}

// The silent syncer: from the client's `muted` line the lane must take the seat and the ATV back by its silence rule
// (the seat free, the host the syncer, one `silenced`); the bound runs from the muting.
void HostSilenced(Arm a, const TableView* t) {
    if (!g_h.sawMuted) {
        if (DC::PollDue() && DC::ChatHasLive(ClientLine("muted"))) {
            g_h.sawMuted = true;
            g_h.mutedAt = Clock::now();
            return HostEnter(HP::Silenced, "silenced");
        }
        if (Past(g_h.run, kFailBound))
            Invalid("host", a, g_h.run, "the client never said it muted its stream", "no line", ClientLine("muted"));
        return;
    }
    const uint64_t silenced = AU::ReadCounters().silenced - g_h.base.silenced;
    if (t && t->occupantSlot == kNoSeat && t->syncerSlot == 0) {
        if (silenced != 1)
            return Fail("host", a, g_h.run, "the seat and the ATV went back to the host by no silence rule",
                        "silenced " + Plus(silenced), "silenced +1");
        g_h.silencedAt = Clock::now();
        return HostEnter(HP::Yielded, "yielded");
    }
    // The rule's own pass is the event: it finds the syncer silent past its bound (silentSeen) and takes the ATV back at the
    // first such pass, so a second that found it silent with none taken is a rule that did not act, the FAIL. A run that
    // never had a pass find it silent could not decide, and the bound says INVALID.
    const uint64_t seen = AU::ReadCounters().silentSeen - g_h.base.silentSeen;
    if (seen < 2 && !Past(g_h.run, kFailBound)) return;
    char got[112];
    std::snprintf(got, sizeof(got), "occupant %u syncer %u silenced +%llu silent_seen +%llu",
                  t ? static_cast<unsigned>(t->occupantSlot) : 0xFFFFu, t ? static_cast<unsigned>(t->syncerSlot) : 0xFFFFu,
                  U(silenced), U(seen));
    if (seen >= 2)
        return Fail("host", a, g_h.run, "the silent syncer was not taken back", t ? got : "no table",
                    "occupant 255 syncer 0 silenced +1");
    Invalid("host", a, g_h.run, "no pass of the authority lane found the syncer silent", t ? got : "no table",
            "silent_seen +1");
}

// The client's own copy gives up the seat on the host's stamp: its `yielded` line ends the run.
void HostYielded(Arm a) {
    if (DC::PollDue() && DC::ChatHasLive(ClientLine("yielded"))) {
        DC::Say(PhaseLine("back"));
        char nums[64];
        std::snprintf(nums, sizeof(nums), "mute_to_silenced_ms=%lld", Ms(g_h.mutedAt, g_h.silencedAt));
        return Pass("host", a, g_h.run, nums);
    }
    if (Past(g_h.run, kFailBound))
        Invalid("host", a, g_h.run, "the client's own copy never gave up the seat", "no line", ClientLine("yielded"));
}

// ---- the client ----

enum class CP : int { Wait, Walk, Act, Undo, Yield, Back };

// The raw requests' numbers. Honest requests cycle NextSeq 1..255, so 250 and 251 are free only on the drill's
// fresh world, where this client sends a handful of requests before the raw one: the drill's contract.
constexpr uint8_t kSeatSeq = 250, kTakeoverSeq = 251;

struct Client : SeatedClient {
    bool                                            sawFirst = false, sawBack = false;  // the host's edges, latched
    std::unique_ptr<coop::director::AimedGrab>      grab;
    Clock::time_point                               nextTry{};
    Clock::time_point                               rawFrom{};   // the raw send's first try: its bound counts from here
    SpawnedWait                                     spawn;       // the host's ATV: its key and its `parked`
};
Client g_c;

void ClientEnter(CP p, const char* name) { Enter(g_c.run, static_cast<int>(p), name); }

// A verdict ends the run: a walk still walking is cancelled, the grab let go of and the stream's mute lifted.
void ClientEnd() {
    WalkCancel(g_c);
    g_c.grab.reset();
    coop::atv_sync::SetDrillMute(false);
}

void ClientFail(Arm a, const std::string& what, const std::string& got, const std::string& want) {
    Fail("client", a, g_c.run, what, got, want);
    ClientEnd();
}

// A bound that ran out with no event to decide on.
void ClientInvalid(Arm a, const std::string& what, const std::string& got, const std::string& want) {
    Invalid("client", a, g_c.run, what, got, want);
    ClientEnd();
}

// Once the host's spawned ATV is parked and the table holds it: the director's walk to its root, and the host told.
void ClientWait(Arm a, bool due) {
    AtvAt t;
    if (SpawnedAtvReady(g_c.spawn, a, due, &t)) {
        WalkStart(g_c, a, t, a == Arm::Grab ? kGrabReachCm : kSitReachCm);
        return ClientEnter(CP::Walk, "walk");
    }
    if (WaitBound(g_c, a, &g_c.spawn) == Stage::Failed) ClientEnd();
}

void ClientWalk(Arm a, void* player) {
    const Stage s = WalkStep(g_c, a);
    if (s == Stage::Failed) return ClientEnd();
    if (s == Stage::Wait) return;
    if (a == Arm::Grab) g_c.grab = std::make_unique<coop::director::AimedGrab>(player, g_c.atv.Get());
    ClientEnter(CP::Act, SpecOf(a).act);
}

// The seat through the game's own verb, then the host's grant: the seat is the host's table, so one check follows.
void ClientSit(Arm a, void* player, void* atv, uint8_t self) {
    const Stage s = SitStep(g_c, a, player, atv, self);
    if (s == Stage::Failed) return ClientEnd();
    if (s == Stage::Next) ClientEnter(CP::Undo, SpecOf(a).undo);
}

// The grav hand's grab until the player's own grabbing actor is the ATV.
void ClientGrab() {
    switch (g_c.grab->Tick()) {
    case coop::director::GrabState::Working:
        if (Past(g_c.run, kFailBound)) ClientInvalid(Arm::Grab, "the grab never finished", "working", "grabbed");
        return;
    case coop::director::GrabState::Failed:
        return ClientFail(Arm::Grab, GrabWhy(*g_c.grab), "failed", "grabbed");
    case coop::director::GrabState::Grabbed:
        g_c.grab.reset();
        return ClientEnter(CP::Undo, SpecOf(Arm::Grab).undo);
    }
}

// On the host's edge line, the act undone: the dismount, or the grav hand's release, tried at most once a second until
// the seat or the grab is gone.
void ClientUndo(Arm a, void* player, void* atv, uint8_t self) {
    if (!g_c.sawFirst) {
        if (Past(g_c.run, kFailBound))
            ClientInvalid(a, "the host never said its edge", "no line", PhaseLine(SpecOf(a).first));
        return;
    }
    const bool sit = a == Arm::Sit;
    if (sit ? coop::atv_sync::ActorSeatedBy(self) != atv : !E::IsMainPlayerGrabbing(player, atv))
        return ClientEnter(CP::Back, "back");
    if (Past(g_c.run, kFailBound))
        return ClientInvalid(a, sit ? "the dismount never freed the seat" : "the release never let go",
                              sit ? "seated" : "grabbing", sit ? "not seated" : "not grabbing");
    const Clock::time_point now = Clock::now();
    if (now < g_c.nextTry) return;
    g_c.nextTry = now + std::chrono::seconds(1);
    if (sit) { if (A::IsDriven(atv)) A::Dismount(atv); }
    else E::ReleaseMainPlayerGrabIfHolding(player, atv);
}

// The silent arm: on the host's edge line the stream goes quiet, and the client's own copy must then give up the seat
// by the host's stamp (the table's occupant alone does not say so; the body does).
void ClientMute(Arm a) {
    if (!g_c.sawFirst) {
        if (Past(g_c.run, kFailBound))
            ClientInvalid(a, "the host never said its edge", "no line", PhaseLine(SpecOf(a).first));
        return;
    }
    coop::atv_sync::SetDrillMute(true);
    DC::Say(ClientLine("muted"));
    UE_LOGI("[ATV-HEALTH-DRILL] client arm=%s the stream is muted", SpecOf(a).name);
    ClientEnter(CP::Yield, "yield");
}

void ClientYield(Arm a) {
    TableView t;
    const bool read = coop::atv_sync::ReadTable(g_c.spawn.key, t);
    if (read && !t.localSeated) {
        DC::Say(ClientLine("yielded"));
        return ClientEnter(CP::Back, "back");
    }
    if (Past(g_c.run, kFailBound))
        ClientInvalid(a, "the client's own copy never gave up the seat", read ? "seated" : "no table", "not seated");
}

// The arms where the host acts: one raw request once the host says its edge, retried once a second until the send took.
// The client's local verb may refuse first, so the host's judgement is what is under test: an AtvSeat for the seat the
// host holds, or an AtvTakeover of the ATV the host carries.
void ClientRaw(coop::net::Session* s, Arm a) {
    if (!g_c.sawFirst) {
        if (Past(g_c.run, kHostFirstBound))
            ClientInvalid(a, "the host never said its edge", "no line", PhaseLine(SpecOf(a).first));
        return;
    }
    const Clock::time_point now = Clock::now();
    if (now < g_c.nextTry) return;
    g_c.nextTry = now + std::chrono::seconds(1);
    if (g_c.rawFrom == Clock::time_point{}) g_c.rawFrom = now;
    if (now - g_c.rawFrom > kFailBound) return ClientInvalid(a, "the raw request never went out", "no send", "sent");
    bool sent = false;
    if (a == Arm::Occupied) {
        coop::net::AtvSeatPayload p{};
        coop::net::WireKeyFromString(g_c.spawn.key, p.key);
        p.seat = 0;
        p.action = 1;
        p.seq = kSeatSeq;
        sent = s->SendReliable(coop::net::ReliableKind::AtvSeat, &p, sizeof(p));
    } else {
        coop::net::AtvTakeoverPayload p{};
        coop::net::WireKeyFromString(g_c.spawn.key, p.key);
        p.seq = kTakeoverSeq;
        sent = s->SendReliable(coop::net::ReliableKind::AtvTakeover, &p, sizeof(p));
    }
    if (!sent) return;
    DC::Say(ClientLine("sent"));
    UE_LOGI("[ATV-HEALTH-DRILL] client arm=%s sent its raw request", SpecOf(a).name);
    ClientEnter(CP::Back, "back");
}

void ClientBack(Arm a) {
    if (g_c.sawBack) {
        char nums[64];
        std::snprintf(nums, sizeof(nums), "walk_ms=%lld", Ms(g_c.walkAt, g_c.arrivedAt));
        Pass("client", a, g_c.run, nums);
        return ClientEnd();
    }
    if (Past(g_c.run, kFailBound))
        ClientInvalid(a, "the host never said it had the ATV back", "no line", PhaseLine("back"));
}

}  // namespace

// The run of still blocks the lane admitted from the client syncer with the seat free and no carry: the stream's counter says
// how many came since the last read, the table's last block speed says whether they were at rest.
void CountRest(RestRun& r, uint64_t idleSinceBase, float lastBlockSpeed) {
    if (idleSinceBase == r.seen) return;
    const uint64_t fresh = idleSinceBase - r.seen;
    r.seen = idleSinceBase;
    const bool still = lastBlockSpeed < kRestCmS;
    r.blocks = !still ? 0 : r.blocks + (r.last ? fresh : 1);
    r.last = still;
}

void HostTick(coop::net::Session* s, Arm a) {
    if (g_h.run.done) return;
    if (g_h.run.at == Clock::time_point{}) {   // the bounds run from the edge of slot 1 being in the world
        if (!s->IsSlotWorldReady(kSlot)) return;
        HostEnter(HP::Wait, "wait");
    }
    if (static_cast<HP>(g_h.run.phase) == HP::Wait) return HostWait(a);
    TableView t;
    const bool read = !g_h.sp.key.empty() && coop::atv_sync::ReadTable(g_h.sp.key, t);
    const TableView* view = read ? &t : nullptr;
    switch (static_cast<HP>(g_h.run.phase)) {
    case HP::Spawn:
        if (SpawnAtvStep(g_h.sp, g_h.run, a) == Stage::Next) HostEnter(HP::Park, "park");
        return;
    case HP::Park:  return HostPark(a);
    case HP::First: return HostFirst(a, view);
    case HP::Left:  return HostLeft(a, view);
    case HP::Back:  return HostBack(a, view);
    case HP::Silenced: return HostSilenced(a, view);
    case HP::Yielded:  return HostYielded(a);
    case HP::Walk:
    case HP::Act:
    case HP::Judge: return ActHostTick(a, view);
    case HP::Wait:  return;
    }
}

void ClientTick(coop::net::Session* s, Arm a) {
    void* player = nullptr;
    uint8_t self = 0;
    if (!SeatedActs(g_c, &player, &self)) return;
    if (g_c.run.at == Clock::time_point{}) ClientEnter(CP::Wait, "wait");   // the bounds run from the world-ready edge
    // The host's edge lines, latched on sight: a live chat row retires after seconds.
    const bool due = DC::PollDue();
    if (due) {
        if (!g_c.sawFirst) g_c.sawFirst = DC::ChatHasLive(PhaseLine(SpecOf(a).first));
        if (!g_c.sawBack) g_c.sawBack = DC::ChatHasLive(PhaseLine("back"));
    }
    void* atv = g_c.atv.Get();
    switch (static_cast<CP>(g_c.run.phase)) {
    case CP::Wait: return ClientWait(a, due);
    case CP::Walk: return ClientWalk(a, player);
    case CP::Act:
        if (!atv) return ClientFail(a, "the ATV is gone before the act", "no ATV", "the ATV");
        if (a == Arm::Grab) return ClientGrab();
        if (a == Arm::Occupied || a == Arm::Synced) return ClientRaw(s, a);
        return ClientSit(a, player, atv, self);
    case CP::Undo:
        if (a == Arm::Silent) return ClientMute(a);
        if (!atv) return ClientFail(a, "the ATV is gone before the undo", "no ATV", "the ATV");
        return ClientUndo(a, player, atv, self);
    case CP::Yield: return ClientYield(a);
    case CP::Back: return ClientBack(a);
    }
}

void HostReset() {
    HostEnd();
    g_h = Host{};
}

void ClientReset() {
    ClientEnd();
    g_c = Client{};
}

}  // namespace coop::dev::atv_health_drill::detail

// coop/moderation/cremate.cpp -- see coop/moderation/cremate.h.
//
// A trip moves through these states, and every one has an exit:
//   Ordered       the order is out, no pose is near the firebox yet
//   Arrived       a pose is near the firebox, the burn volume has not taken the puppet
//   RunningWait   the lever is pulled (or the handle is moving), the run's start is not seen
//   Running       the host's run is on
//   Cooling       the run ended with the player alive; waiting for its flames to go out
// A trip ends with exactly one line, to the caller and to the told seats. The exits are the target's leave, its death,
// the cremator's loss, and a bound in fresh poses on every state that waits for the target or the machine; a run that
// is on is bounded by the machine, and a player who keeps burning by Cooling's own count.

#include "coop/moderation/cremate.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/interactables/cremator_occupancy.h"
#include "coop/interactables/cremator_state.h"
#include "coop/net/end_reason.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_events.h"
#include "coop/player/remote_player.h"
#include "coop/player/roster_ledger.h"
#include "coop/session/teleport_client.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/cremator.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <utility>

namespace coop::moderation::cremate {
namespace {

namespace C = ue_wrap::cremator;

// A pose this close to the ordered destination is the order's arrival. Past the pose bounds below, an order that
// never arrived, a puppet the burn volume never took, a machine that never started or a player who never stopped
// burning is a miss: counts of fresh poses on an event stream, not clocks. The ordered bound sits well past a
// level-streaming hitch; the arrival logs how many poses it took. The run-wait bound is far past the lever handle's
// 1 to 3 s; the cooling bound is far past a burn's own length (~14 s to the kill).
//
// MTA's setElementPosition is the server's word and is not acknowledged (CStaticFunctionDefinitions.cpp:1303). Here the
// client owns its pawn, so the landing is judged from its own poses and the burn volume, and a miss is reported.
constexpr float kArriveCm = 150.f;
constexpr int   kOrderedPoseBound = 120;
constexpr int   kArrivedPoseBound = 30;
constexpr int   kRunWaitPoseBound = 600;
constexpr int   kCoolingPoseBound = 1800;

enum class State : uint8_t { Ordered, Arrived, RunningWait, Running, Cooling };
enum class Outcome : uint8_t {
    Cremated, Survived, SurvivedBurning, Left, DidNotLand, DiedBeforeLanding, DiedNotByFire, WouldNotStart, CrematorGone
};

struct Trip {
    bool                      active = false;
    PlayerToken               target;
    std::string               nick;
    std::string               kickWhy;
    ReplySeat                 caller;
    std::vector<ReplySeat>    told;
    ReplyFn                   reply = nullptr;
    LogFn                     log = nullptr;
    coop::permissions::Action kickRecord;
    ue_wrap::CachedObjRef     cremator;
    ue_wrap::FVector          dest{};
    State                     state = State::Ordered;
    int                       poses = 0;         // fresh poses since the order, the arrival, the lever, or the run's end
    // Set in OnPose, on a pose that arrived with the burning bit while the drained state was past the arrival; it is
    // set before the drain judges any life edge.
    bool                      burnSeen = false;
};

std::array<Trip, coop::players::kMaxPeers> g_trips;
int g_active = 0;
std::atomic<coop::net::Session*> g_session{nullptr};
bool g_listening = false;
bool g_watchWarned = false;

// What the listeners recorded, in arrival order. Two buffers swap at the drain, so a handler that starts or ends a trip
// never grows the list it walks, and the capacity is kept. A slot event carries the generation of the trip it was
// recorded for: a slot handed to another player in the meantime never feeds the new occupant's trip.
enum class Kind : uint8_t { Life, Pose, Overlap, HostRun };
struct Event {
    Kind     kind;
    uint8_t  slot = 0;
    bool     flag = false;  // Life: alive. Pose: burning. HostRun: active.
    uint32_t gen = 0;
    float    x = 0, y = 0, z = 0;
    void*    cremator = nullptr;
    void*    other = nullptr;
};
std::vector<Event> g_queue;
std::vector<Event> g_work;

void Record(const Event& e) { g_queue.push_back(e); }

const char* StateName(State s) {
    switch (s) {
        case State::Ordered: return "Ordered";
        case State::Arrived: return "Arrived";
        case State::RunningWait: return "RunningWait";
        case State::Running: return "Running";
        case State::Cooling: return "Cooling";
    }
    return "?";
}

// A life edge is rare (a death, a revive), so each one a running verb sees is said, taken or dropped with the reason:
// a trip that missed its target's death must show where the edge went.
// The line names the EDGE's slot and generation; the slot's trip is told only while one is active, since an ended trip
// is cleared and a successor's number and state are not the edge's.
void SayLifeEdge(uint8_t slot, uint32_t gen, bool alive, const char* verdict) {
    const Trip& t = g_trips[slot];
    char trip[48] = "none";
    if (t.active)
        std::snprintf(trip, sizeof(trip), "#%u state=%s", static_cast<unsigned>(t.target.playerNo), StateName(t.state));
    UE_LOGI("cremate: slot %u life edge alive=%d gen=%u, the slot's trip %s -- %s", static_cast<unsigned>(slot),
            alive ? 1 : 0, static_cast<unsigned>(gen), trip, verdict);
}

void OnLife(uint8_t slot, bool alive) {
    if (g_active == 0) return;
    if (!g_trips[slot].active) {
        UE_LOGI("cremate: slot %u life edge alive=%d -- dropped (no trip for the slot)", static_cast<unsigned>(slot),
                alive ? 1 : 0);
        return;
    }
    Event e{Kind::Life};
    e.slot = slot;
    e.flag = alive;
    e.gen = g_trips[slot].target.generation;
    Record(e);
}

void OnPose(uint8_t slot, const coop::net::PoseSnapshot& pose) {
    if (g_active == 0 || !g_trips[slot].active) return;
    Event e{Kind::Pose};
    e.slot = slot;
    e.flag = (pose.stateBits & coop::net::kStateBitBurning) != 0;
    // The burning latch is set as the pose arrives, not at the drain: puppet_events raises a pose's life edge before
    // its pose listeners, so a target under 10 health -- ignited and killed by one burn tick -- has its death recorded
    // ahead of the burning pose that carries it, and a latch set at the drain would judge that death "not by its fire".
    // A burning pose after the landing is the cremator's: a client's copy burns nothing itself (cremator_client_guard),
    // its burn is the host's CrematorHits (cremator_hit), which the host sends after its own volume took the puppet,
    // and that begin-overlap is the Overlap event drained first. A target already burning from another fire before it
    // lands is not latched, rightly.
    Trip& t = g_trips[slot];
    if (e.flag && !t.burnSeen && t.state != State::Ordered && t.state != State::Arrived) {
        t.burnSeen = true;
        UE_LOGI("cremate: #%u burning seen (state=%s)", static_cast<unsigned>(t.target.playerNo), StateName(t.state));
    }
    e.gen = g_trips[slot].target.generation;
    e.x = pose.x; e.y = pose.y; e.z = pose.z;
    Record(e);
}

void OnOverlap(void* cremator, void* other) {
    if (g_active == 0) return;
    Event e{Kind::Overlap};
    e.cremator = cremator;
    e.other = other;
    Record(e);
}

void OnHostRun(void* cremator, bool active) {
    if (g_active == 0) return;
    Event e{Kind::HostRun};
    e.flag = active;
    e.cremator = cremator;
    Record(e);
}

coop::net::Session* HostedSession() {
    auto* s = g_session.load(std::memory_order_acquire);
    return (s && s->running() && s->role() == coop::net::Role::Host) ? s : nullptr;
}

// The drill's red arm: the verb never listens to the burn volume, so a player it took in is never seen to land.
bool LandingOff() {
    static const bool sOff = coop::config::ResolveFlag(coop::config_registry::rows::cremate_landing_off);
    return sOff;
}

void* PuppetActor(int slot) {
    coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(static_cast<uint8_t>(slot));
    return p && p->valid() ? p->GetActor() : nullptr;
}

void Say(const ReplySeat& to, ReplyFn reply, const std::string& line) {
    if (reply) reply(to, line);
}

std::string LineFor(Outcome o, const std::string& nick) {
    switch (o) {
        case Outcome::Cremated: return nick + " was cremated.";
        case Outcome::Survived: return nick + " survived the crematorium.";
        case Outcome::SurvivedBurning: return nick + " survived the crematorium (still burning).";
        case Outcome::Left: return nick + " left before the end.";
        case Outcome::DidNotLand: return nick + " did not land in the cremator.";
        case Outcome::DiedBeforeLanding: return nick + " died before landing.";
        case Outcome::DiedNotByFire: return nick + " died in the cremator, but not by its fire.";
        case Outcome::WouldNotStart: return "The cremator would not start for " + nick + ".";
        case Outcome::CrematorGone: return "The cremator is gone, so " + nick + " was not cremated.";
    }
    return std::string();
}

const char* KickFailure(ModResult r) {
    switch (r) {
        case ModResult::Gone: return "they had already left";
        case ModResult::NoSession: return "the session ended";
        default: return "the kick did not run";
    }
}

// Ends a trip with its one line: to the caller, then to the told seats; for a cremation, then the kick (the order is the
// death edge, the line, the kick, so the kick lands well before the game's own revive). The trip is cleared first, so
// nothing the kick starts finds it. A kick that did not act is said after the cremation line; one that did is written
// to the action log, as the /kick command's is.
//
// The door: a run that never started opens it again, so the player can walk out. A miss never shut it. When the target
// left, died before landing, or the cremator is gone, the door is left as it is: open or shut is the game's own state
// then.
void End(Trip& t, Outcome o) {
    const std::string line = LineFor(o, t.nick);
    const std::string nick = t.nick;
    const std::string kickWhy = t.kickWhy;
    const ReplySeat caller = t.caller;
    const std::vector<ReplySeat> told = std::move(t.told);
    const ReplyFn reply = t.reply;
    const LogFn log = t.log;
    const coop::permissions::Action kickRecord = t.kickRecord;
    const PlayerToken target = t.target;
    void* const cremator = t.cremator.Get();
    t = Trip{};
    --g_active;
    UE_LOGI("cremate: #%u (slot %d) -- %s", static_cast<unsigned>(target.playerNo), target.slot, line.c_str());
    if (o == Outcome::WouldNotStart && cremator) {
        bool opened = false;
        C::OpenDoor(cremator, opened);
    }
    Say(caller, reply, line);
    for (const ReplySeat& seat : told) Say(seat, reply, line);
    if (o != Outcome::Cremated) return;
    // MTA's KickPlayer names the responsible party and a reason (CStaticFunctionDefinitions.cpp:11876); ours is the
    // typed end-reason row plus the caller as the detail.
    const ModResult kicked =
        coop::moderation::KickPlayerWithReason(target, kickWhy.c_str(), coop::net::EndReason::Cremated);
    if (kicked == ModResult::Done) {
        if (log) log(kickRecord);
        return;
    }
    UE_LOGW("cremate: #%u was cremated but the kick did not run (%s)", static_cast<unsigned>(target.playerNo),
            KickFailure(kicked));
    const std::string failed = nick + " was cremated, but the kick failed (" + KickFailure(kicked) + ").";
    Say(caller, reply, failed);
    for (const ReplySeat& seat : told) Say(seat, reply, failed);
}

float Between(const ue_wrap::FVector& a, float x, float y, float z) {
    const float dx = a.X - x, dy = a.Y - y, dz = a.Z - z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The burn volume took the puppet: shut the door and start the run. A copy the machine already runs is not touched (the
// handle's pull toggles); one whose handle is moving is waited on as a starting one.
void Landed(Trip& t) {
    if (coop::players::Registry::Get().LifeOf(static_cast<uint8_t>(t.target.slot)) ==
        coop::players::PuppetLife::Dead) {
        End(t, Outcome::DiedBeforeLanding);
        return;
    }
    void* const cremator = t.cremator.Get();
    bool wasOpen = false, active = false, anim = false;
    if (!cremator || !C::ShutDoor(cremator, wasOpen) || !C::ReadActive(cremator, active) ||
        !C::ReadAnim(cremator, anim)) {
        End(t, Outcome::WouldNotStart);
        return;
    }
    t.poses = 0;
    if (active) {
        t.state = State::Running;
        return;
    }
    t.state = State::RunningWait;
    if (anim) return;
    if (!C::CallUseLever(cremator)) End(t, Outcome::WouldNotStart);
}

// A death after the landing is a cremation only when the target's own burning bit was seen set first: the machine's
// ignite reached it. MTA's wasted packet names the killer and weapon (CGame.cpp:2141); the pose stream carries no cause,
// so this bit is the evidence there is.
Outcome DeathAfterLanding(const Trip& t) { return t.burnSeen ? Outcome::Cremated : Outcome::DiedNotByFire; }

void OnLifeEvent(Trip& t, bool alive) {
    if (alive) return;
    if (t.state == State::Ordered || t.state == State::Arrived) End(t, Outcome::DiedBeforeLanding);
    else End(t, DeathAfterLanding(t));
}

void OnPoseEvent(Trip& t, const Event& e) {
    switch (t.state) {
        case State::Ordered:
            ++t.poses;
            if (Between(t.dest, e.x, e.y, e.z) <= kArriveCm) {
                UE_LOGI("cremate: #%u arrived at the firebox after %d fresh pose(s)",
                        static_cast<unsigned>(t.target.playerNo), t.poses);
                t.state = State::Arrived;
                t.poses = 0;
            } else if (t.poses >= kOrderedPoseBound) {
                End(t, Outcome::DidNotLand);
            }
            break;
        case State::Arrived:
            if (++t.poses >= kArrivedPoseBound) End(t, Outcome::DidNotLand);
            break;
        case State::RunningWait:
            if (++t.poses >= kRunWaitPoseBound) End(t, Outcome::WouldNotStart);
            break;
        case State::Cooling:
            if (!e.flag) End(t, Outcome::Survived);
            else if (++t.poses >= kCoolingPoseBound) End(t, Outcome::SurvivedBurning);
            break;
        default: break;
    }
}

void OnOverlapEvent(Trip& t, const Event& e) {
    if (t.state != State::Ordered && t.state != State::Arrived) return;
    if (e.cremator != t.cremator.Get() || !e.other || e.other != PuppetActor(t.target.slot)) return;
    UE_LOGI("cremate: #%u taken by the burn volume (%s, %d fresh pose(s) there)",
            static_cast<unsigned>(t.target.playerNo), t.state == State::Arrived ? "after the arrival" : "before the arrival",
            t.poses);
    Landed(t);
}

void OnHostRunEvent(Trip& t, const Event& e) {
    if (e.cremator != t.cremator.Get()) return;
    if (t.state == State::RunningWait) {
        if (e.flag) t.state = State::Running;
        else End(t, Outcome::WouldNotStart);
    } else if (t.state == State::Running && !e.flag) {
        switch (coop::players::Registry::Get().LifeOf(static_cast<uint8_t>(t.target.slot))) {
            case coop::players::PuppetLife::Alive:
                t.state = State::Cooling;
                t.poses = 0;
                break;
            case coop::players::PuppetLife::Dead: End(t, DeathAfterLanding(t)); break;
            default: End(t, Outcome::Left); break;  // no puppet: the target is gone, which is a leave, not a death
        }
    }
}

void Handle(const Event& e) {
    switch (e.kind) {
        case Kind::Life:
        case Kind::Pose: {
            Trip& t = g_trips[e.slot];
            if (!t.active || t.target.generation != e.gen) {
                if (e.kind == Kind::Life) SayLifeEdge(e.slot, e.gen, e.flag, "dropped (the trip ended or changed hands first)");
                return;
            }
            if (e.kind == Kind::Life) {
                SayLifeEdge(e.slot, e.gen, e.flag, "taken");
                OnLifeEvent(t, e.flag);
            }
            else OnPoseEvent(t, e);
            return;
        }
        case Kind::Overlap:
        case Kind::HostRun:
            for (Trip& t : g_trips) {
                if (!t.active) continue;
                if (e.kind == Kind::Overlap) OnOverlapEvent(t, e);
                else OnHostRunEvent(t, e);
            }
            return;
    }
}

}  // namespace

Result Start(const Order& order, ReplyFn reply, LogFn log) {
    UE_ASSERT_GAME_THREAD("moderation::cremate::Start");
    coop::net::Session* s = HostedSession();
    if (!s) return Result::NoSession;
    const PlayerToken& token = order.target;
    if (!token.valid() || token.slot >= coop::players::kMaxPeers ||
        s->peerGenerationForSlot(token.slot) != token.generation ||
        coop::roster_ledger::Get(token.slot).playerNo != token.playerNo)
        return Result::Gone;
    Trip& t = g_trips[token.slot];
    if (t.active) return Result::Busy;
    const uint8_t slot = static_cast<uint8_t>(token.slot);
    if (coop::players::Registry::Get().LifeOf(slot) != coop::players::PuppetLife::Alive) return Result::NotAlive;
    coop::RemotePlayer* puppet = coop::players::Registry::Get().Puppet(slot);
    ue_wrap::FVector from{};
    if (!puppet || !puppet->TryGetLocation(from)) {
        UE_LOGW("cremate: #%u refused -- the puppet's position did not read", static_cast<unsigned>(token.playerNo));
        return Result::NotSent;
    }
    C::FireboxMove move;
    const C::MoveResult found = C::FindFireboxMove(from, move);
    if (found == C::MoveResult::NoCremator) return Result::NoCremator;
    if (found == C::MoveResult::RotationUnread) {
        UE_LOGW("cremate: #%u refused -- the cremator's rotation did not read", static_cast<unsigned>(token.playerNo));
        return Result::NotSent;
    }
    // A second order would open the door of a machine another player is burning behind.
    for (const Trip& other : g_trips)
        if (other.active && other.cremator.Get() == move.actor) return Result::CrematorBusy;
    // Without the burn volume's begin-overlap watch the landing could never be seen: the player would be moved into an
    // open cremator and then reported as not landed.
    if (!coop::cremator_occupancy::WatchLive()) {
        UE_LOGW("cremate: #%u refused -- the burn volume's begin-overlap watch is not live",
                static_cast<unsigned>(token.playerNo));
        return Result::NotWatched;
    }
    const bool inside = coop::cremator_occupancy::IsOverlapping(move.actor, puppet->GetActor());
    if (!inside) {
        bool wasOpen = false, opened = false;
        C::ReadDoorOpened(move.actor, wasOpen);
        if (!C::OpenDoor(move.actor, opened)) return Result::DoorStuck;
        if (!coop::teleport_client::OrderToPose(*s, token.slot, move.at, move.rot)) {
            UE_LOGW("cremate: #%u refused -- the teleport order was not sent", static_cast<unsigned>(token.playerNo));
            bool shut = false;
            if (!wasOpen) C::ShutDoor(move.actor, shut);
            return Result::NotSent;
        }
    }
    t = Trip{};
    t.active = true;
    t.target = token;
    t.nick = order.targetNick;
    t.kickWhy = order.callerNick + " sent you to the crematorium.";
    t.caller = order.caller;
    t.told = order.told;
    t.reply = reply;
    t.log = log;
    t.kickRecord = order.kickRecord;
    t.cremator.Set(move.actor);
    t.dest = move.at;
    ++g_active;
    UE_LOGI("cremate: #%u (slot %d) sent to the cremator at (%.0f,%.0f,%.0f)%s", static_cast<unsigned>(token.playerNo),
            token.slot, move.at.X, move.at.Y, move.at.Z, inside ? " (already inside its burn volume)" : "");
    if (inside) {
        // The puppet already stands in the volume: no begin-overlap will come, so the verb's next tick takes the
        // landing from the same event.
        t.state = State::Arrived;
        Event e{Kind::Overlap};
        e.cremator = move.actor;
        e.other = puppet->GetActor();
        Record(e);
    }
    return Result::Started;
}

void Install(coop::net::Session* session) {
    UE_ASSERT_GAME_THREAD("moderation::cremate::Install");
    g_session.store(session, std::memory_order_release);
    // The fixed listener lists are small: a full one would leave the verb deaf, so it is said.
    const bool added = coop::puppet_events::AddLifeListener(&OnLife) &&
                       coop::puppet_events::AddPoseListener(&OnPose) &&
                       coop::cremator_state::AddHostRunListener(&OnHostRun);
    if (!added) UE_LOGW("cremate: a listener list is full -- the /cremate verb cannot see every event of a trip");
}

void Tick() {
    UE_ASSERT_GAME_THREAD("moderation::cremate::Tick");
    if (!g_listening && !LandingOff() && HostedSession()) {
        g_listening = coop::cremator_occupancy::AddListener(&OnOverlap);
        if (!g_listening && !g_watchWarned) {
            g_watchWarned = true;
            UE_LOGW("cremate: the cremator's begin-overlap cannot be watched yet (retried every tick)");
        }
    }
    // A cremator that died or left with its world ends its trip whatever state it is in: nothing will report for it.
    if (g_active > 0)
        for (Trip& t : g_trips)
            if (t.active && !t.cremator.Get()) End(t, Outcome::CrematorGone);
    if (g_queue.empty()) return;
    g_work.swap(g_queue);
    for (const Event& e : g_work) Handle(e);
    g_work.clear();
}

void OnDisconnectSlot(int slot) {
    UE_ASSERT_GAME_THREAD("moderation::cremate::OnDisconnectSlot");
    if (slot < 0 || slot >= coop::players::kMaxPeers || !g_trips[slot].active) return;
    // The session's teardown runs this for every slot after the session stopped: that is the session ending, not a leave.
    if (!HostedSession()) {
        g_trips[slot] = Trip{};
        --g_active;
        return;
    }
    End(g_trips[slot], Outcome::Left);
}

void OnDisconnect() {
    UE_ASSERT_GAME_THREAD("moderation::cremate::OnDisconnect");
    for (Trip& t : g_trips) t = Trip{};
    g_active = 0;
    g_queue.clear();
    g_work.clear();
}

}  // namespace coop::moderation::cremate

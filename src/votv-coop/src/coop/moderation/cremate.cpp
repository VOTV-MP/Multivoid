// coop/moderation/cremate.cpp -- see coop/moderation/cremate.h.
//
// A trip moves through these states, and every one has an exit:
//   Ordered       the order is out, no pose is near the firebox yet
//   Arrived       a pose is near the firebox, the burn volume has not taken the puppet
//   RunningWait   the lever is pulled (or the handle is moving), the run's start is not seen
//   Running       the host's run is on
//   Cooling       the run ended with the player alive; waiting for its flames to go out
// A trip ends with exactly one line, to the caller and to the told seats.

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

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/hot_path_guard.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/devices/cremator.h"
#include "ue_wrap/engine/engine.h"

#include <array>
#include <atomic>
#include <cmath>
#include <utility>

namespace coop::moderation::cremate {
namespace {

namespace C = ue_wrap::cremator;
namespace E = ue_wrap::engine;

// A pose this close to the ordered destination is the order's arrival. Past the pose bounds below, an order that
// never arrived, or a puppet the burn volume never took, is a miss: counts of fresh poses on an event stream, not
// clocks. The ordered bound sits well past a level-streaming hitch; the arrival logs how many poses it took.
constexpr float kArriveCm = 150.f;
constexpr int   kOrderedPoseBound = 120;
constexpr int   kArrivedPoseBound = 30;

constexpr const char* kCrematedWhy = "You were sent to the crematorium.";

enum class State : uint8_t { Ordered, Arrived, RunningWait, Running, Cooling };
enum class Outcome : uint8_t { Cremated, Survived, Left, DidNotLand, DiedBeforeLanding, WouldNotStart };

struct Trip {
    bool                   active = false;
    PlayerToken            target;
    std::string            nick;
    ReplySeat              caller;
    std::vector<ReplySeat> told;
    ReplyFn                reply = nullptr;
    ue_wrap::CachedObjRef  cremator;
    ue_wrap::FVector       dest{};
    State                  state = State::Ordered;
    int                    poses = 0;  // fresh poses since the order (Ordered), or since the arrival (Arrived)
};

std::array<Trip, coop::players::kMaxPeers> g_trips;
int g_active = 0;
std::atomic<coop::net::Session*> g_session{nullptr};
bool g_listening = false;
bool g_watchWarned = false;

// What the listeners recorded, in arrival order. Two buffers swap at the drain, so a handler that starts or ends a trip
// never grows the list it walks, and the capacity is kept.
enum class Kind : uint8_t { Life, Pose, Overlap, HostRun };
struct Event {
    Kind    kind;
    uint8_t slot = 0;
    bool    flag = false;  // Life: alive. Pose: burning. HostRun: active.
    float   x = 0, y = 0, z = 0;
    void*   cremator = nullptr;
    void*   other = nullptr;
};
std::vector<Event> g_queue;
std::vector<Event> g_work;

void Record(const Event& e) { g_queue.push_back(e); }

void OnLife(uint8_t slot, bool alive) {
    if (g_active == 0 || !g_trips[slot].active) return;
    Event e{Kind::Life};
    e.slot = slot;
    e.flag = alive;
    Record(e);
}

void OnPose(uint8_t slot, const coop::net::PoseSnapshot& pose) {
    if (g_active == 0 || !g_trips[slot].active) return;
    Event e{Kind::Pose};
    e.slot = slot;
    e.flag = (pose.stateBits & coop::net::kStateBitBurning) != 0;
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
        case Outcome::Left: return nick + " left before the end.";
        case Outcome::DidNotLand: return nick + " did not land in the cremator.";
        case Outcome::DiedBeforeLanding: return nick + " died before landing.";
        case Outcome::WouldNotStart: return "The cremator would not start for " + nick + ".";
    }
    return std::string();
}

// Ends a trip with its one line: to the caller, then to the told seats; for a cremation, then the kick (the order is the
// death edge, the line, the kick, so the kick lands well before the game's own revive). The trip is cleared first, so
// nothing the kick starts finds it.
void End(Trip& t, Outcome o) {
    const std::string line = LineFor(o, t.nick);
    const ReplySeat caller = t.caller;
    const std::vector<ReplySeat> told = std::move(t.told);
    const ReplyFn reply = t.reply;
    const PlayerToken target = t.target;
    void* const cremator = t.cremator.Get();
    t = Trip{};
    --g_active;
    UE_LOGI("cremate: #%u (slot %d) -- %s", static_cast<unsigned>(target.playerNo), target.slot, line.c_str());
    // A run that never started opens the door again, so the player can walk out; a miss never shut it.
    if (o == Outcome::WouldNotStart && cremator) {
        bool opened = false;
        C::OpenDoor(cremator, opened);
    }
    Say(caller, reply, line);
    for (const ReplySeat& seat : told) Say(seat, reply, line);
    if (o == Outcome::Cremated)
        coop::moderation::KickPlayerWithReason(target, kCrematedWhy, coop::net::EndReason::Cremated);
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
    if (active) {
        t.state = State::Running;
        return;
    }
    t.state = State::RunningWait;
    if (anim) return;
    if (!C::CallUseLever(cremator)) End(t, Outcome::WouldNotStart);
}

void OnLifeEvent(Trip& t, bool alive) {
    if (alive) return;
    if (t.state == State::Ordered || t.state == State::Arrived) End(t, Outcome::DiedBeforeLanding);
    else End(t, Outcome::Cremated);
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
        case State::Cooling:
            if (!e.flag) End(t, Outcome::Survived);
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
        if (coop::players::Registry::Get().LifeOf(static_cast<uint8_t>(t.target.slot)) ==
            coop::players::PuppetLife::Alive)
            t.state = State::Cooling;
        else End(t, Outcome::Cremated);
    }
}

void Handle(const Event& e) {
    switch (e.kind) {
        case Kind::Life:
        case Kind::Pose: {
            Trip& t = g_trips[e.slot];
            if (!t.active) return;
            if (e.kind == Kind::Life) OnLifeEvent(t, e.flag);
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

Result Start(const Order& order, ReplyFn reply) {
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
    const C::Nearest n = C::FindNearestFirebox(from);
    if (!n.actor) return Result::NoCremator;
    ue_wrap::FRotator rot{};
    if (!E::TryGetActorRotation(n.actor, rot)) {
        UE_LOGW("cremate: #%u refused -- the cremator's rotation did not read", static_cast<unsigned>(token.playerNo));
        return Result::NotSent;
    }
    const bool inside = coop::cremator_occupancy::IsOverlapping(n.actor, puppet->GetActor());
    if (!inside) {
        bool wasOpen = false, opened = false;
        C::ReadDoorOpened(n.actor, wasOpen);
        if (!C::OpenDoor(n.actor, opened)) return Result::DoorStuck;
        coop::net::TeleportClientPayload pose{};
        pose.locX = n.at.X; pose.locY = n.at.Y; pose.locZ = n.at.Z;
        pose.rotPitch = rot.Pitch; pose.rotYaw = rot.Yaw; pose.rotRoll = rot.Roll;
        if (!s->SendReliableToSlot(token.slot, coop::net::ReliableKind::TeleportClient, &pose, sizeof(pose))) {
            UE_LOGW("cremate: #%u refused -- the teleport order was not sent", static_cast<unsigned>(token.playerNo));
            bool shut = false;
            if (!wasOpen) C::ShutDoor(n.actor, shut);
            return Result::NotSent;
        }
    }
    t = Trip{};
    t.active = true;
    t.target = token;
    t.nick = order.targetNick;
    t.caller = order.caller;
    t.told = order.told;
    t.reply = reply;
    t.cremator.Set(n.actor);
    t.dest = n.at;
    ++g_active;
    UE_LOGI("cremate: #%u (slot %d) sent to the cremator at (%.0f,%.0f,%.0f)%s", static_cast<unsigned>(token.playerNo),
            token.slot, n.at.X, n.at.Y, n.at.Z, inside ? " (already inside its burn volume)" : "");
    if (inside) {
        // The puppet already stands in the volume: no begin-overlap will come, so the verb's next tick takes the
        // landing from the same event.
        t.state = State::Arrived;
        Event e{Kind::Overlap};
        e.cremator = n.actor;
        e.other = puppet->GetActor();
        Record(e);
    }
    return Result::Started;
}

void Install(coop::net::Session* session) {
    g_session.store(session, std::memory_order_release);
    coop::puppet_events::AddLifeListener(&OnLife);
    coop::puppet_events::AddPoseListener(&OnPose);
    coop::cremator_state::AddHostRunListener(&OnHostRun);
}

void Tick() {
    // The drill's red arm: the verb never listens to the burn volume, so a player it took in is never seen to land.
    static const bool sLandingOff = coop::config::ResolveFlag(coop::config_registry::rows::cremate_landing_off);
    if (!g_listening && !sLandingOff && HostedSession()) {
        g_listening = coop::cremator_occupancy::AddListener(&OnOverlap);
        if (!g_listening && !g_watchWarned) {
            g_watchWarned = true;
            UE_LOGW("cremate: the cremator's begin-overlap cannot be watched yet (retried every tick)");
        }
    }
    if (g_queue.empty()) return;
    g_work.swap(g_queue);
    for (const Event& e : g_work) Handle(e);
    g_work.clear();
}

void OnDisconnectSlot(int slot) {
    if (slot < 0 || slot >= coop::players::kMaxPeers || !g_trips[slot].active) return;
    End(g_trips[slot], Outcome::Left);
}

void OnDisconnect() {
    for (Trip& t : g_trips) t = Trip{};
    g_active = 0;
    g_queue.clear();
    g_work.clear();
}

}  // namespace coop::moderation::cremate

// coop/dev/body_push_drill_client.cpp -- the client's half of the body-push drill; see coop/dev/body_push_drill.h.
//
// At its join's end say what its character steps up, read from its own CharacterMovement. After the host's ready line, find
// its own copy of the woodchipper, and for each push: let the copy rest, have the director walk this pawn to a point 100 cm
// past the prop's far side, say the walk's end, let the copy rest, say its rest pose and how far it moved, and wait for the
// host's decided line before the next. The director blocks, so a walk runs on a worker.

#include "body_push_drill_detail.h"

#include "coop/dev/collision_probe.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_ground.h"
#include "coop/dev/rest_compare.h"
#include "coop/net/session.h"
#include "coop/element/registry.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_syncer.h"
#include "coop/player/players_registry.h"
#include "coop/session/join_progress.h"
#include "coop/session/net_pump.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/puppet.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace coop::dev::body_push_drill::detail::client {
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace DC = coop::dev::drill_chat;
namespace DG = coop::dev::drill_ground;
namespace RC = coop::dev::rest_compare;
using ue_wrap::FRotator;
using ue_wrap::FVector;

enum class Step { WaitJoin, WaitReady, Find, WaitControl, SettleBefore, Walk, SettleAfter, WaitDecided, HostClosed, HostLeg, Done };

Step g_step = Step::WaitJoin;
int  g_leg = 1;
FVector g_spot{};   // the host's measured floor, where the copy stands
uint64_t g_stepMs = 0;
uint64_t g_legMs = 0;   // when the leg began: an End counts only from here
int  g_findTicks = 0;
constexpr float kBackCm = 150.f;   // the pusher steps this far back from the copy after its End, before the rest compare
bool g_backDone = false;            // this leg's step back has finished
std::shared_ptr<coop::director::BackgroundWalk> g_backWalk;
int  g_closedSaidLeg = 0;   // the leg whose wait for the open row to close was said
uint32_t g_restEnds0 = 0;   // this client's own rest Ends sent when the host's leg began
bool g_armed = false;
ue_wrap::CachedObjRef g_prop;
Settle g_rest;
FVector g_before{};
std::shared_ptr<coop::director::BackgroundWalk> g_walk;

void Next(Step s) {
    g_step = s;
    g_stepMs = ::GetTickCount64();
}
bool Expired(uint64_t boundMs) { return ::GetTickCount64() - g_stepMs > boundMs; }

void StepFind(void* player) {
    if (++g_findTicks % 30 != 1) return;   // a walk of the object array a few times a second at most
    FVector me{};
    if (!E::TryGetActorLocation(player, me)) return;
    void* prop = DG::NearestOf(kChipperClass, g_spot, kFindCm);
    if (!prop) {
        Wait('C', "this client's copy of the spawned woodchipper to arrive");
        if (Expired(kBindBoundMs)) Invalid('C', "this client's copy of the woodchipper was not found");
        return;
    }
    g_prop.Set(prop);
    hits::Track(prop, coop::players::Registry::Get().Local(), nullptr);
    hits::NotifyFlag('C', prop, /*setOn=*/true);
    Footprint f;
    ReadFootprint(prop, f);
    UE_LOGI("[BODY-PUSH-DRILL] [C] found the woodchipper %.0f cm from this pawn, bounds half-extent (%.1f, %.1f, %.1f)",
            Dist(f.origin, me), f.extent.X, f.extent.Y, f.extent.Z);
    hits::BeginLeg(0);
    DC::Say(FoundLine());
    Next(Step::WaitControl);
}

// The drive Ends this peer applied since `sinceMs` include one for the copy: the host's push ended, and its End has landed.
bool EndSince(void* prop, uint64_t sinceMs, FVector* pose = nullptr) {
    const uint32_t eid = static_cast<uint32_t>(coop::element::Registry::Get().EidForActor(prop));
    coop::prop_drive_stream::AppliedEnd ends[coop::prop_drive_stream::kRecentEnds];
    const size_t n = coop::prop_drive_stream::RecentEnds(ends, coop::prop_drive_stream::kRecentEnds);
    for (size_t i = 0; i < n; ++i)
        if (ends[i].eid == eid && ends[i].atMs >= sinceMs) {
            if (pose) *pose = ends[i].pose;
            return true;
        }
    return false;
}

// This copy has no open row of its own: this client no longer syncs it and no stream parks it.
bool ClosedHere(void* prop) { return !coop::prop_drive_stream::IsParked(prop) && !coop::prop_syncer::AuthorsActorHere(prop); }

bool StartWalk(void* player) {
    void* prop = g_prop.Get();
    FVector me{}, c{};
    Footprint f;
    if (!prop || !E::TryGetActorLocation(player, me) || !E::TryGetActorLocation(prop, c) || !ReadFootprint(prop, f)) {
        Invalid('C', "the pawn or the copy could not be read before push %d", g_leg);
        return false;
    }
    const float dx = c.X - me.X, dy = c.Y - me.Y;
    const float len = std::hypot(dx, dy);
    if (len < 1.f) { Invalid('C', "this pawn stands on the copy before push %d", g_leg); return false; }
    const FVector goal{c.X + dx / len * kPastCm, c.Y + dy / len * kPastCm, me.Z};
    g_before = c;
    g_legMs = ::GetTickCount64();
    g_backDone = false;
    g_backWalk.reset();
    hits::Track(prop, player, nullptr);
    hits::BeginLeg(g_leg);
    g_walk = coop::director::StartBackgroundWalk(goal, kReachCm, kWalkDeadlineS, /*carry=*/false, prop);   // the prop is walked into, never deleted
    UE_LOGI("[BODY-PUSH-DRILL] [C] PUSH %d begins: this pawn (%.0f, %.0f, %.0f), the copy (%.1f, %.1f, %.1f), walking to "
            "(%.0f, %.0f, %.0f)", g_leg, me.X, me.Y, me.Z, c.X, c.Y, c.Z, goal.X, goal.Y, goal.Z);
    return true;
}

}  // namespace

void Tick(coop::net::Session& /*s*/) {
    if (!g_armed) {
        g_armed = true;
        hits::Install();
        DC::SeenHas(ReadyPrefix());   // the first read starts the feed's history
        UE_LOGI("[BODY-PUSH-DRILL] [C] started: the client's half is running and reading the chat history");
        Next(Step::WaitJoin);
    }
    if (g_step == Step::Done) return;
    void* player = coop::players::Registry::Get().Local();
    if (g_step == Step::WaitJoin) {
        if (!coop::net_pump::HasAnnouncedWorldReady() ||
            coop::join_progress::CurrentPhase() != coop::join_progress::Phase::Idle || !player || !R::IsLive(player)) {
            Wait('C', "this client's join to end and its pawn");
            return;
        }
        float step = 0.f;
        if (!ue_wrap::puppet::ReadCharacterMaxStepHeight(player, step)) {
            Invalid('C', "this character's MaxStepHeight could not be read");
            return;
        }
        UE_LOGI("[BODY-PUSH-DRILL] [C] this character steps up %.1f cm (CharacterMovement MaxStepHeight), capsule radius %.1f cm",
                step, E::GetActorCharacterRadius(player));
        DC::Say(StepLine(step));
        Next(Step::WaitReady);
        return;
    }
    if (g_step == Step::WaitReady) {
        std::string rest;
        if (DC::SeenRestOf(ReadyPrefix(), &rest)) {
            // "<x> <y> <z>": the host's measured floor spot, where the copy stands.
            float sx = 0.f, sy = 0.f, sz = 0.f;
            if (sscanf_s(rest.c_str(), "%f %f %f", &sx, &sy, &sz) != 3) {
                Invalid('C', "the host's ready line could not be read: '%s'", rest.c_str());
                return;
            }
            g_spot = {sx, sy, sz};
            UE_LOGI("[BODY-PUSH-DRILL] [C] the host's woodchipper stands at (%.0f, %.0f, %.0f)", sx, sy, sz);
            Next(Step::Find);
        } else {
            Wait('C', "the host's ready chat line");
            if (Expired(kPushBoundMs)) Invalid('C', "the host's ready line never came");
        }
        return;
    }
    if (!player || !R::IsLive(player)) return;
    switch (g_step) {
    case Step::Find: StepFind(player); return;
    case Step::WaitControl:   // the host's box has landed on the woodchipper: this copy's counts for it are said, then the pushes
        if (DC::SeenHas(ControlDoneLine())) {
            hits::SayLeg('C', 0);
            hits::BeginLeg(g_leg);
            Restart(g_rest);
            Next(Step::SettleBefore);
        } else {
            Wait('C', "the host's control done line");
            if (Expired(kPushBoundMs)) Invalid('C', "the host's control done line never came");
        }
        return;
    case Step::SettleBefore: {
        void* prop = g_prop.Get();
        FVector c{};
        if (!prop || !E::TryGetActorLocation(prop, c)) { Invalid('C', "the copy died before push %d", g_leg); return; }
        const bool closed = ClosedHere(prop);
        if (!closed && g_closedSaidLeg != g_leg) {
            g_closedSaidLeg = g_leg;
            UE_LOGI("[BODY-PUSH-DRILL] [C] leg %d waits for the prop's open row to close", g_leg);
        }
        if (Stable(g_rest, c, closed)) {
            if (StartWalk(player)) Next(Step::Walk);
        } else {
            Wait('C', "the copy to rest before push %d", g_leg);
            if (Expired(kBindBoundMs)) Invalid('C', "the copy never rested before push %d", g_leg);
        }
        return;
    }
    case Step::Walk: {
        const int state = g_walk ? g_walk->state.load() : 2;
        if (state == 0) {
            Wait('C', "the director's walk of push %d", g_leg);
            return;
        }
        DC::Say(EndPrefix(g_leg) + std::to_string(state));
        UE_LOGI("[BODY-PUSH-DRILL] [C] PUSH %d: the walk ended in state %d (1 reached, 2 failed)", g_leg, state);
        {
            char label[32];
            std::snprintf(label, sizeof(label), "push %d", g_leg);
            coop::dev::collision_probe::LogPair(label, player, ue_wrap::prop::GetStaticMesh(g_prop.Get()));
        }
        g_walk.reset();
        Restart(g_rest);
        Next(Step::SettleAfter);
        return;
    }
    case Step::SettleAfter: {
        void* prop = g_prop.Get();
        FVector c{};
        if (!prop || !E::TryGetActorLocation(prop, c)) { Invalid('C', "the copy died after push %d", g_leg); return; }
        // Marked after the push's End: the drive no longer parks the copy, this client no longer syncs it, and on the host's leg
        // the host's End was applied here; the rotation is still as well as the place.
        FRotator rotNow{};
        E::TryGetActorRotation(prop, rotNow);
        // The host's leg: the row's End may be the host's (applied here) or this client's own (its claim took the push, the
        // bumper-syncs shape); either ends it, and both copies then rest at the claimant's pose.
        const bool ownEnd = coop::prop_syncer::dev::CountOf(coop::prop_syncer::dev::Count::RestEnds) > g_restEnds0;
        const bool ended = !coop::prop_drive_stream::IsParked(prop) && !coop::prop_syncer::AuthorsActorHere(prop) &&
                           (g_leg <= kPushes || EndSince(prop, g_legMs) || ownEnd);
        // The pusher steps back from the copy once its End has landed, so a prop resting against this pawn is not held awake by
        // it when the rest is marked.
        if (ended && g_leg <= kPushes && !g_backDone) {
            if (!g_backWalk) {
                FVector me{};
                if (!E::TryGetActorLocation(player, me)) { Invalid('C', "this pawn could not be read to step back after push %d", g_leg); return; }
                const float len = std::hypot(me.X - c.X, me.Y - c.Y);
                const FVector dir = len > 1.f ? FVector{(me.X - c.X) / len, (me.Y - c.Y) / len, 0.f} : FVector{1.f, 0.f, 0.f};
                const FVector goal{me.X + dir.X * kBackCm, me.Y + dir.Y * kBackCm, me.Z};
                g_backWalk = coop::director::StartBackgroundWalk(goal, kReachCm, kWalkDeadlineS, /*carry=*/false, prop);
                UE_LOGI("[BODY-PUSH-DRILL] [C] PUSH %d: the End has landed; this pawn steps back %.0f cm to (%.0f, %.0f, %.0f)",
                        g_leg, static_cast<double>(kBackCm), goal.X, goal.Y, goal.Z);
            }
            const int back = g_backWalk->state.load();
            if (back == 0) { Wait('C', "the step back after push %d", g_leg); return; }
            UE_LOGI("[BODY-PUSH-DRILL] [C] PUSH %d: the step back ended in state %d (1 reached, 2 failed)", g_leg, back);
            g_backWalk.reset();
            g_backDone = true;
            Restart(g_rest);
            return;
        }
        if (Stable(g_rest, c, DG::Quiet(prop) && ended, kMarkTicks, &rotNow)) {
            RC::Pose pose;
            if (!RC::Read(prop, pose)) { Invalid('C', "the copy's pose could not be read at push %d's rest", g_leg); return; }
            const float moved = Dist(pose.loc, g_before);
            hits::SayLeg('C', g_leg);
            UE_LOGI("[BODY-PUSH-DRILL] [C] PUSH %d: this client's copy RESTED at (%.1f, %.1f, %.1f), moved %.1f cm", g_leg,
                    pose.loc.X, pose.loc.Y, pose.loc.Z, moved);
            char head[32];
            std::snprintf(head, sizeof(head), "%.1f ", moved);
            std::string line = RestedPrefix(g_leg) + head + RC::Encode(pose);
            if (g_leg > kPushes) {   // the host's own push: the pose of the host's End this copy applied, or that this client's End ended it
                FVector endAt{};
                if (EndSince(prop, g_legMs, &endAt)) {
                    char tail[64];
                    std::snprintf(tail, sizeof(tail), " %.2f %.2f %.2f", endAt.X, endAt.Y, endAt.Z);
                    line += tail;
                } else {
                    line += " own";
                }
            }
            DC::Say(line);
            Next(Step::WaitDecided);
        } else {
            Wait('C', "the copy to rest after push %d", g_leg);
            if (Expired(kBindBoundMs)) Invalid('C', "the copy never rested after push %d", g_leg);
        }
        return;
    }
    case Step::HostClosed: {   // before the host's own push: this copy's row is closed, said to the host
        void* prop = g_prop.Get();
        if (!prop) { Invalid('C', "the copy died before the host's own push"); return; }
        if (ClosedHere(prop)) {
            DC::Say(ClosedLine(g_leg));
            g_legMs = ::GetTickCount64();
            g_restEnds0 = coop::prop_syncer::dev::CountOf(coop::prop_syncer::dev::Count::RestEnds);
            Next(Step::HostLeg);
        } else {
            Wait('C', "this copy's open row to close before the host's own push");
            if (Expired(kBindBoundMs)) Invalid('C', "this copy's row never closed before the host's own push");
        }
        return;
    }
    case Step::HostLeg:   // the host walks into the prop; its end line starts this copy's rest
        if (DC::SeenHas(EndPrefix(g_leg))) {
            UE_LOGI("[BODY-PUSH-DRILL] [C] the host's own push ended; this client's copy is watched to its rest");
            Restart(g_rest);
            Next(Step::SettleAfter);
        } else {
            Wait('C', "the host's end line of its own push");
            if (Expired(kPushBoundMs)) Invalid('C', "the host's end line of its own push never came");
        }
        return;
    case Step::WaitDecided:
        if (DC::SeenHas(DecidedLine(g_leg))) {
            if (g_leg == kLegs) {
                UE_LOGI("[BODY-PUSH-DRILL] [C] DONE");
                Next(Step::Done);
                return;
            }
            ++g_leg;
            Restart(g_rest);
            if (g_leg > kPushes) {   // the last leg is the host's own push: this client only watches its copy
                if (void* prop = g_prop.Get()) E::TryGetActorLocation(prop, g_before);
                hits::BeginLeg(g_leg);
                g_legMs = ::GetTickCount64();
                g_restEnds0 = coop::prop_syncer::dev::CountOf(coop::prop_syncer::dev::Count::RestEnds);
                UE_LOGI("[BODY-PUSH-DRILL] [C] leg %d waits for the prop's open row to close", g_leg);
                Next(Step::HostClosed);
            } else {
                Next(Step::SettleBefore);
            }
        } else {
            Wait('C', "the host's decided line for push %d", g_leg);
            if (Expired(kPushBoundMs)) Invalid('C', "the host's decided line for push %d never came", g_leg);
        }
        return;
    default: return;
    }
}

void Reset() {
    g_step = Step::WaitJoin;
    g_leg = 1;
    g_findTicks = 0;
    g_closedSaidLeg = 0;
    g_backDone = false;
    g_backWalk.reset();
    g_armed = false;
    g_prop.Reset();
    Restart(g_rest);
    g_walk.reset();
    hits::Reset();
}

}  // namespace coop::dev::body_push_drill::detail::client

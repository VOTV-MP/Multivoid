// coop/dev/body_push_drill_host.cpp -- the host's half of the body-push drill; see coop/dev/body_push_drill.h.
//
// Once the client has said what it steps up, spawn the woodchipper before the client's puppet on measured open ground,
// say ready when it rests, then for each push: from the last rest, the puppet's nearest approach to the prop's bounds while
// the client walks; at the client's end line, wait for the host's copy to rest, and for the client's rested line; compare the
// two rest poses, and say decided. A gap past the bound is the FAIL, said at that push; the fifth push's agreement is the PASS.

#include "body_push_drill_detail.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/dev/collision_probe.h"
#include "coop/dev/director/director.h"
#include "coop/dev/drill_chat.h"
#include "coop/dev/drill_ground.h"
#include "coop/dev/rest_compare.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"
#include "coop/props/prop_drive_host.h"
#include "coop/props/prop_drive_stream.h"
#include "coop/props/prop_element_tracker.h"
#include "coop/props/prop_snapshot.h"
#include "coop/save/join_window_baseline.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"
#include "ue_wrap/engine/world_identity.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace coop::dev::body_push_drill::detail::host {
namespace {

namespace R  = ue_wrap::reflection;
namespace PR = ue_wrap::prop;
namespace E  = ue_wrap::engine;
namespace DC = coop::dev::drill_chat;
namespace DG = coop::dev::drill_ground;
namespace RC = coop::dev::rest_compare;
using ue_wrap::FRotator;
using ue_wrap::FVector;

enum class Step { WaitPeer, Bind, WaitFound, ControlRest, Window, WaitClosed, HostWalk, Settle, WaitClientRest, Done };

Step g_step = Step::WaitPeer;
bool g_armed = false;
int  g_slot = 0;
float g_maxStep = 0.f;
int  g_leg = 1;
uint64_t g_stepMs = 0;
ue_wrap::CachedObjRef g_prop;
ue_wrap::CachedObjRef g_box;   // the control's box
Settle g_rest;
FVector g_before{};
FVector g_spot{};      // the measured floor the prop stands on, the client's search point
float g_minGap = 1.0e9f;
bool g_hostBackDone = false;   // the host's own leg: its pawn has stepped back from the copy after the End
std::shared_ptr<coop::director::BackgroundWalk> g_hostBack;
constexpr float kBackCm = 150.f;
float g_minPuppetCm = 1.0e9f;   // the puppet's centre's closest approach to the prop's collision this leg
float g_puppetRadius = 0.f;
int  g_contacts = 0, g_moved = 0;   // pushes with contact; those among them that moved a copy
int  g_traceTicks = 0;              // the ticks traced since the End landed on this leg
bool g_legMoved = false;
std::shared_ptr<coop::director::BackgroundWalk> g_hostWalk;   // the host's own player walking into the prop, the last leg
uint32_t g_eid = 0;                       // the host's element id of the prop
uint8_t g_clientRow = 0;                  // the client slot whose row stood on the prop during this push, 0 if none
bool g_hostRow = false;                   // ... and whether a host coast row stood on it
RC::Pose g_hostRest;

void Next(Step s) {
    g_step = s;
    g_stepMs = ::GetTickCount64();
}

// The host's red arm (body_push_off): the host streams no push, which the host's own leg must fail. Resolved once.
bool RedHostArm() {
    static const bool on = coop::config::ResolveFlag(coop::config_registry::rows::body_push_off);
    return on;
}
bool Expired(uint64_t boundMs) { return ::GetTickCount64() - g_stepMs > boundMs; }

// Which peer holds the push's drive row: sampled every tick of a push, since a row lives from the push to half a second past
// its rest. A client's row is the pusher's syncing; a host coast row is the host flying it (the host's own push, or a
// claim the host refused and answered with its pose).
void SampleRow() {
    if (!g_eid) return;
    if (const uint8_t slot = coop::prop_drive_host::ClientSyncerOf(g_eid)) { g_clientRow = slot; return; }
    coop::prop_drive_host::CoastCause c;
    if (coop::prop_drive_host::OpenCoastCause(g_eid, c)) g_hostRow = true;
}

void* Puppet();

// The hit count's pawns: the client's puppet on this host, and the host's own pawn for the host's leg. Asked at each leg, since
// a puppet can be made again.
void TrackHits() { hits::Track(g_prop.Get(), Puppet(), coop::players::Registry::Get().Local()); }

void* Puppet() { return coop::puppet_drive::Puppet(g_slot).GetActor(); }

bool PuppetAt(FVector& at, float& radius);

// The first ticks after a push's End landed: how far the copy stands from the pose the End put it at, whether its body reads at
// rest, and the puppet's gap to it. Says where a copy that left the End's pose went and what stood against it. No verdict reads it.
void TraceAfterEnd(void* prop, const FVector& at) {
    constexpr int kTraceTicks = 8;
    if (g_traceTicks >= kTraceTicks || !g_eid) return;
    coop::prop_drive_stream::AppliedEnd ends[coop::prop_drive_stream::kRecentEnds];
    const size_t n = coop::prop_drive_stream::RecentEnds(ends, coop::prop_drive_stream::kRecentEnds);
    for (size_t i = 0; i < n; ++i) {
        if (ends[i].eid != g_eid) continue;
        FVector pAt{};
        float radius = 0.f;
        Footprint f;
        const float gap = PuppetAt(pAt, radius) && ReadFootprint(prop, f) ? GapCm(pAt, radius, f) : 1.0e9f;
        UE_LOGI("[BODY-PUSH-DRILL] [H] push %d after the End, tick %d: the copy stands %.2f cm from the End's pose (%.1f, %.1f, "
                "%.1f), body at rest %d, the puppet's gap to it %.1f cm", g_leg, g_traceTicks, Dist(at, ends[i].pose), ends[i].pose.X,
                ends[i].pose.Y, ends[i].pose.Z, E::IsActorRootBodyAtRest(prop) ? 1 : 0, gap);
        ++g_traceTicks;
        return;
    }
}

// The puppet's place and capsule radius; false when it cannot be read.
bool PuppetAt(FVector& at, float& radius) {
    void* puppet = Puppet();
    if (!puppet || !R::IsLive(puppet) || !E::TryGetActorLocation(puppet, at)) return false;
    radius = E::GetActorCharacterRadius(puppet);
    return radius > 0.f;
}

// Every other prop within kClearCm of the drill's ground is destroyed through the host's own destroy (the death seam expresses it to
// every peer), so the pushes meet the woodchipper alone: a neighbour a push shoved moved a copy with no row of its own.
constexpr float kClearCm = 300.f;
constexpr float kPollMoveCm = 3.f;   // a move under this is below what the push poll reads (coop/props/body_push, kMoveCm)
struct ClearCtx {
    FVector at;
    void* world;
    std::vector<void*> found;
};
void EachNearProp(void* ctx, void* obj, int32_t) {
    auto* c = static_cast<ClearCtx*>(ctx);
    if (ue_wrap::world_identity::WorldOf(obj) != c->world) return;   // another world's, and the class default objects
    FVector at{};
    if (E::TryGetActorLocation(obj, at) && Dist(at, c->at) <= kClearCm) c->found.push_back(obj);
}
void ClearGround(const FVector& ground) {
    ClearCtx ctx{ground, ue_wrap::world_identity::CurrentWorld(), {}};
    ue_wrap::object_index::ForEachClass(
        [](void* c, void* cls, void*) {
            if (PR::IsClassDescendantOfProp(cls)) ue_wrap::object_index::ForEachInstance(cls, &EachNearProp, c);
        },
        &ctx);
    int cleared = 0, held = 0;
    for (void* prop : ctx.found) {
        if (coop::prop_drive_host::InAHand(prop)) { ++held; continue; }
        FVector at{};
        E::TryGetActorLocation(prop, at);
        UE_LOGI("[BODY-PUSH-DRILL] [H] cleared '%ls' %p %.0f cm from the ground (eid %u)", R::ClassNameOf(prop).c_str(), prop,
                Dist(at, ground), static_cast<unsigned>(coop::prop_element_tracker::GetPropElementIdForActor(prop)));
        E::DestroyActor(prop);
        ++cleared;
    }
    UE_LOGI("[BODY-PUSH-DRILL] [H] cleared %d prop(s) within %.0f cm of the ground (%d held by a hand left)", cleared,
            static_cast<double>(kClearCm), held);
}

void StepWaitPeer(coop::net::Session& s) {
    int slot = 0;
    for (int i = 1; i < coop::net::kMaxPeers && !slot; ++i)
        if (s.IsSlotWorldReady(i) && coop::prop_snapshot::IsBracketClosed(i) &&
            !coop::join_window_baseline::IsLateWindowOpen(i))
            slot = i;
    if (!slot) { Wait('H', "a client slot that is world-ready with its join window closed"); return; }
    g_slot = slot;
    std::string rest;
    if (!DC::SeenRestOf(StepPrefix(), &rest)) {   // what the client's character steps up, read from its own movement
        Wait('H', "slot %d's maxstep chat line", slot);
        return;
    }
    g_maxStep = static_cast<float>(std::atof(rest.c_str()));
    void* puppet = Puppet();
    FVector at{};
    if (!puppet || !R::IsLive(puppet) || !E::TryGetActorLocation(puppet, at)) {
        Wait('H', "slot %d's puppet to spawn and its place to read", slot);
        return;
    }
    // The prop is set on measured open ground, not where the client stands: its start area has buildings and raised floors,
    // and a machine spawned there lodges in them. The director walks the client to the spot, staged if far.
    void* host = coop::players::Registry::Get().Local();
    if (!host || !R::IsLive(host)) { Wait('H', "this host's own pawn, for the ground traces"); return; }
    if (!DG::FindSpot(host, at, kHalfCm, kChipperClass, kCrowdCm, "BODY-PUSH-DRILL", g_spot)) {
        Invalid('H', "no level open ground with a clear footprint and no woodchipper within %.0f cm", kCrowdCm);
        return;
    }
    ClearGround(g_spot);
    const FVector at0{g_spot.X, g_spot.Y, g_spot.Z + kLiftCm};
    void* cls = ue_wrap::asset_load::LoadObjectByPath(kChipperPath);
    void* prop = cls ? E::SpawnActor(cls, at0) : nullptr;
    if (!prop) { Invalid('H', "the woodchipper did not spawn (class %p)", cls); return; }
    g_prop.Set(prop);
    hits::NotifyFlag('H', prop, /*setOn=*/true);
    UE_LOGI("[BODY-PUSH-DRILL] [H] the client steps up %.1f cm; spawned a woodchipper (class %p, actor %p) at (%.0f, %.0f, "
            "%.0f) on open ground whose floor is (%.0f, %.0f, %.0f), %.0f cm from slot %d's puppet", g_maxStep, cls, prop,
            at0.X, at0.Y, at0.Z, g_spot.X, g_spot.Y, g_spot.Z, Dist(g_spot, at), slot);
    Restart(g_rest);
    Next(Step::Bind);
}

void StepBind() {
    void* prop = g_prop.Get();
    if (!prop) { Invalid('H', "the woodchipper died before it rested"); return; }
    const uint32_t eid = static_cast<uint32_t>(coop::prop_element_tracker::GetPropElementIdForActor(prop));
    const bool bound = eid != static_cast<uint32_t>(coop::element::kInvalidId);
    FVector at{};
    const bool read = E::TryGetActorLocation(prop, at);
    const bool quiet = DG::Quiet(prop);
    const bool stable = read && Stable(g_rest, at, quiet);
    if (!(bound && stable)) {
        Wait('H', "the woodchipper's element id (%d) and rest (quiet %d, still %d)", bound ? 1 : 0, quiet ? 1 : 0,
             stable ? 1 : 0);
        if (Expired(kBindBoundMs))
            Invalid('H', "the woodchipper never bound and rested (bound %d, quiet %d, still %d)", bound ? 1 : 0,
                    quiet ? 1 : 0, stable ? 1 : 0);
        return;
    }
    Footprint f;
    if (!ReadFootprint(prop, f)) { Invalid('H', "the woodchipper's bounds could not be read"); return; }
    UE_LOGI("[BODY-PUSH-DRILL] [H] the woodchipper rests at (%.1f, %.1f, %.1f), %.1f cm tall against %.1f cm the character "
            "steps up", f.origin.X, f.origin.Y, f.origin.Z, HeightCm(f), g_maxStep);
    if (HeightCm(f) <= g_maxStep) {
        Invalid('H', "the woodchipper is %.1f cm tall, no taller than the %.1f cm the character steps up", HeightCm(f),
                g_maxStep);
        return;
    }
    E::TryGetActorLocation(prop, g_before);
    g_eid = eid;
    TrackHits();
    hits::BeginLeg(g_leg);
    char spot[96];
    std::snprintf(spot, sizeof(spot), "%.0f %.0f %.0f", g_spot.X, g_spot.Y, g_spot.Z);
    DC::Say(ReadyPrefix() + spot);
    UE_LOGI("[BODY-PUSH-DRILL] [H] READY");
    coop::dev::collision_probe::LogActor("woodchipper ready", prop);
    g_minGap = 1.0e9f;
    hits::BeginLeg(0);
    Next(Step::WaitFound);
}

// The control: a box dropped from kControlDropCm above the sleeping woodchipper, a known hit on it.
void StepWaitFound() {
    if (!DC::SeenHas(FoundLine())) {
        Wait('H', "the client's found line");
        if (Expired(kPushBoundMs)) Invalid('H', "the client's found line never came");
        return;
    }
    void* prop = g_prop.Get();
    Footprint f;
    if (!prop || !ReadFootprint(prop, f)) { Invalid('H', "the woodchipper's bounds could not be read for the control"); return; }
    void* cls = ue_wrap::asset_load::LoadObjectByPath(kBoxPath);
    const FVector at{f.origin.X, f.origin.Y, f.origin.Z + f.extent.Z + kControlDropCm};
    void* box = cls ? E::SpawnActor(cls, at) : nullptr;
    if (!box) { Invalid('H', "the control's box did not spawn (class %p)", cls); return; }
    g_box.Set(box);
    UE_LOGI("[BODY-PUSH-DRILL] [H] CONTROL: a box dropped on the woodchipper from %.0f cm above its top (%.0f, %.0f, %.0f)",
            kControlDropCm, at.X, at.Y, at.Z);
    Restart(g_rest);
    Next(Step::ControlRest);
}

void StepControlRest() {
    void* box = g_box.Get();
    FVector at{};
    if (!box || !E::TryGetActorLocation(box, at)) { Invalid('H', "the control's box died before it rested"); return; }
    if (!Stable(g_rest, at, DG::Quiet(box))) {
        Wait('H', "the control's box to rest");
        if (Expired(kBindBoundMs)) Invalid('H', "the control's box never rested");
        return;
    }
    E::DestroyActor(box);
    g_box.Reset();
    hits::SayLeg('H', 0);
    DC::Say(ControlDoneLine());
    hits::BeginLeg(g_leg);
    g_minGap = 1.0e9f;
    Next(Step::Window);
}

// The host's own player walks into the prop after the client's pushes: the host's body push coasts it (the host holds the row)
// and the client's copy follows its stream (the host's puppet on the client applies no force). With body_push_off nothing
// streams it and each peer's copy rests by its own physics.
void StartHostLeg() {
    void* prop = g_prop.Get();
    void* me = coop::players::Registry::Get().Local();
    FVector p{}, m{};
    if (!prop || !me || !E::TryGetActorLocation(prop, p) || !E::TryGetActorLocation(me, m)) {
        Invalid('H', "the host's pawn or the prop could not be read before the host's push");
        return;
    }
    // Through the prop's centre (its bounds' centre, the actor's origin may sit off it) and kPastCm beyond, as the client legs go.
    Footprint f;
    const FVector c = ReadFootprint(prop, f) ? f.origin : p;
    const float len = std::hypot(c.X - m.X, c.Y - m.Y);
    if (len < 1.f) { Invalid('H', "the host's pawn stands on the prop before the host's push"); return; }
    const FVector goal{c.X + (c.X - m.X) / len * kPastCm, c.Y + (c.Y - m.Y) / len * kPastCm, m.Z};
    g_hostWalk = coop::director::StartBackgroundWalk(goal, kReachCm, kWalkDeadlineS, /*carry=*/false, prop);
    UE_LOGI("[BODY-PUSH-DRILL] [H] the host's own push begins: the host's pawn (%.0f, %.0f, %.0f) walks to (%.0f, %.0f, %.0f)",
            m.X, m.Y, m.Z, goal.X, goal.Y, goal.Z);
    Next(Step::HostWalk);
}

void Verdict() {
    if (g_moved < kMinPushes) {
        Invalid('H', "only %d of %d pushes, with contact, moved a copy over %.0f cm (%d with contact); at least %d are needed",
                g_moved, kPushes, kMovedCm, g_contacts, kMinPushes);
    } else {
        coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Pass,
                               "all %d client pushes and the host's own push rested within %.0f cm and %.0f deg on both peers, "
                               "each with its pusher's row; %d client pushes with contact moved a copy",
                               kPushes, RC::kBoundCm, RC::kBoundDeg, g_moved);
    }
    UE_LOGI("[BODY-PUSH-DRILL] [H] DONE");
}

// Both copies have rested: judge them, then let the client go on.
void Decide(const RC::Pose& client, float clientMovedCm, const FVector& clientEnd, bool clientEndOwn) {
    const float hostMoved = Dist(g_hostRest.loc, g_before);
    const bool touched = g_minGap <= kContactCm;
    const bool hostLeg = g_leg > kPushes;
    g_legMoved = hostMoved > kMovedCm || clientMovedCm > kMovedCm;
    if (touched && !hostLeg) {
        ++g_contacts;
        if (g_legMoved) ++g_moved;
    }
    char what[110];
    std::snprintf(what, sizeof(what), "%s %d (host copy moved %.1f cm, client's %.1f; the %s came within %.1f cm)",
                  hostLeg ? "the host's own push, leg" : "push", g_leg, hostMoved, clientMovedCm,
                  hostLeg ? "host's pawn" : "puppet", g_minGap);
    g_before = g_hostRest.loc;
    hits::SayLeg('H', g_leg);
    UE_LOGI("[BODY-PUSH-DRILL] [H] push %d: the drive row was held by %s", g_leg,
            g_clientRow ? "the client (the pusher's peer)" : g_hostRow ? "the host" : "nobody");
    // The red arm is built to fail the host's own leg: a client push that fails first says INVALID, since the lever the red
    // turns (the host's push) was not what failed it.
    const bool earlyRed = RedHostArm() && !hostLeg;
    {   // the probe: which pawn component the engine's responses say blocks the prop this leg
        char label[32];
        std::snprintf(label, sizeof(label), "push %d", g_leg);
        coop::dev::collision_probe::LogPair(label, hostLeg ? coop::players::Registry::Get().Local() : Puppet(),
                                            ue_wrap::prop::GetStaticMesh(g_prop.Get()));
    }
    if (!hostLeg) {
        UE_LOGI("[BODY-PUSH-DRILL] [H] push %d: the puppet's centre came %.1f cm from the prop's collision (capsule radius %.1f cm)",
                g_leg, g_minPuppetCm, g_puppetRadius);
    }
    if (!RC::Judge(kTag, what, g_hostRest, client, /*sayFail=*/!earlyRed)) {   // the FAIL is said: nothing further runs
        if (earlyRed)
            Invalid('H', "red arm body_push_off: push %d rested apart before the host's own leg, which this red is built to fail",
                    g_leg);
        UE_LOGI("[BODY-PUSH-DRILL] [H] DONE");
        Next(Step::Done);
        return;
    }
    if (hostLeg) {   // the host's own push: its row is the host's, and the leg must have pushed
        if (!touched || !g_legMoved) {
            Invalid('H', "the host's own walk did not push the prop (contact %d, moved %d)", touched ? 1 : 0, g_legMoved ? 1 : 0);
            return;
        }
        UE_LOGI("[BODY-PUSH-DRILL] [H] the host's own push: the drive row was held by %s",
                g_clientRow ? "the client (its claim took the push)" : g_hostRow ? "the host" : "nobody");
        if (!g_clientRow && !g_hostRow) {
            coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Fail,
                                   "the host's own push moved a copy, and no drive row was ever opened");
            UE_LOGI("[BODY-PUSH-DRILL] [H] DONE");
            Next(Step::Done);
            return;
        }
        // Whoever held the row ended it, and both copies rest at the claimant's pose: the two rests are compared above. When the
        // host's End is the one the client applied, the client's copy also rests at that End's pose.
        const bool hostEnd = !clientEndOwn;
        const float atEnd = hostEnd ? Dist(clientEnd, g_hostRest.loc) : 0.f, atRest = hostEnd ? Dist(client.loc, clientEnd) : 0.f;
        UE_LOGI("[BODY-PUSH-DRILL] [H] the End that closed the row was %s's; %s", hostEnd ? "the host's" : "the client's",
                hostEnd ? "as the client applied it, it is that far from the host's copy's rest and the client's copy's rest" : "the copies' rests are compared above");
        if (hostEnd && (atEnd > RC::kBoundCm || atRest > RC::kBoundCm)) {
            coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Fail,
                                   "the host's own push: the client's copy rests %.1f cm from the host's End pose it applied "
                                   "(that End is %.1f cm from the host's copy; bound %.0f cm)", atRest, atEnd, RC::kBoundCm);
            UE_LOGI("[BODY-PUSH-DRILL] [H] DONE");
            Next(Step::Done);
            return;
        }
    } else if (touched && g_legMoved && !g_clientRow) {   // the pusher must be the syncer: one physics source per push
        if (earlyRed)
            Invalid('H', "red arm body_push_off: push %d's drive row was %s before the host's own leg, which this red is built to fail",
                    g_leg, g_hostRow ? "the host's" : "never opened");
        else
            coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Fail,
                                   "push %d touched the prop and moved a copy, and its drive row was %s, not the pusher's peer's",
                                   g_leg, g_hostRow ? "the host's" : "never opened");
        UE_LOGI("[BODY-PUSH-DRILL] [H] DONE");
        Next(Step::Done);
        return;
    }
    DC::Say(DecidedLine(g_leg));
    if (g_leg == kLegs) {
        Verdict();
        Next(Step::Done);
        return;
    }
    ++g_leg;
    g_minGap = 1.0e9f;
    g_minPuppetCm = 1.0e9f;
    g_traceTicks = 0;
    g_clientRow = 0;
    g_hostRow = false;
    TrackHits();
    hits::BeginLeg(g_leg);
    if (g_leg > kPushes) {
        // The host's leg begins only once no row of the prop is open on either peer: a client's row that lives on until its body
        // sleeps would otherwise own the host's push and the End the client waits for would be the client's own.
        UE_LOGI("[BODY-PUSH-DRILL] [H] leg %d waits for the prop's open row to close", g_leg);
        Next(Step::WaitClosed);
    }
    else Next(Step::Window);
}

}  // namespace

void Tick(coop::net::Session& s) {
    if (!g_armed) {
        // The first Seen* read starts the chat history: the client's lines reach the host while its join window is still
        // open, and the feed retires a row after about ten seconds, so a first read after the window would find none.
        g_armed = true;
        hits::Install();
        DC::SeenHas(StepPrefix());
        UE_LOGI("[BODY-PUSH-DRILL] [H] started: the host's half is running and reading the chat history");
    }
    if (g_step == Step::Window || g_step == Step::HostWalk || g_step == Step::Settle || g_step == Step::WaitClientRest) SampleRow();
    switch (g_step) {
    case Step::WaitPeer: StepWaitPeer(s); return;
    case Step::Bind: StepBind(); return;
    case Step::WaitFound: StepWaitFound(); return;
    case Step::ControlRest: StepControlRest(); return;
    case Step::Window: {
        FVector puppetAt{};
        float radius = 0.f;
        Footprint f;
        if (PuppetAt(puppetAt, radius) && ReadFootprint(g_prop.Get(), f))
            g_minGap = std::fmin(g_minGap, GapCm(puppetAt, radius, f));
        if (PuppetAt(puppetAt, radius)) {
            const float d = E::GetComponentDistanceToCollision(ue_wrap::prop::GetStaticMesh(g_prop.Get()), puppetAt);
            if (d >= 0.f) g_minPuppetCm = std::fmin(g_minPuppetCm, d);
            g_puppetRadius = radius;
        }
        if (DC::SeenHas(EndPrefix(g_leg))) {
            Restart(g_rest);
            Next(Step::Settle);
            return;
        }
        Wait('H', "the client's end of push %d", g_leg);
        if (Expired(kPushBoundMs)) Invalid('H', "the client's end of push %d never came", g_leg);
        return;
    }
    case Step::WaitClosed: {
        void* prop = g_prop.Get();
        if (!prop) { Invalid('H', "the prop died before the host's own push"); return; }
        const bool hostOpen = g_eid && coop::prop_drive_host::ContextOf(prop, g_eid).open;
        if (hostOpen || !DC::SeenHas(ClosedLine(g_leg))) {
            Wait('H', "the prop's open row to close (host row open %d)", hostOpen ? 1 : 0);
            if (Expired(kBindBoundMs)) Invalid('H', "the prop's row never closed before the host's own push");
            return;
        }
        StartHostLeg();
        return;
    }
    case Step::HostWalk: {   // the last leg: the host's own player walks into the prop
        void* me = coop::players::Registry::Get().Local();
        FVector mAt{};
        Footprint f;
        if (me && E::TryGetActorLocation(me, mAt) && ReadFootprint(g_prop.Get(), f))
            g_minGap = std::fmin(g_minGap, GapCm(mAt, E::GetActorCharacterRadius(me), f));
        const int state = g_hostWalk ? g_hostWalk->state.load() : 2;
        if (state == 0) { Wait('H', "the host's own walk into the prop"); return; }
        UE_LOGI("[BODY-PUSH-DRILL] [H] the host's own push: the walk ended in state %d (1 reached, 2 failed)", state);
        g_hostWalk.reset();
        // A walk that ended without the copy moving more than the poll's own threshold pushed nothing: the leg measures nothing,
        // so it says so now and the client does not wait for an End that cannot come.
        FVector copyAt{};
        if (!E::TryGetActorLocation(g_prop.Get(), copyAt)) { Invalid('H', "the prop could not be read after the host's walk"); return; }
        const float movedByWalk = Dist(copyAt, g_before);
        if (movedByWalk <= kPollMoveCm) {
            Invalid('H', "the host's walk did not push the prop: moved %.1f cm (the poll takes over %.0f cm)",
                    static_cast<double>(movedByWalk), static_cast<double>(kPollMoveCm));
            return;
        }
        DC::Say(EndPrefix(g_leg) + std::to_string(state));
        Restart(g_rest);
        Next(Step::Settle);
        return;
    }
    case Step::Settle: {
        void* prop = g_prop.Get();
        FVector at{};
        if (!prop || !E::TryGetActorLocation(prop, at)) { Invalid('H', "the prop died before push %d's rest", g_leg); return; }
        // The push's End has landed before the mark: its row is closed (a client's row ends with its rest End, the host's with
        // its own), and the rotation is still as well as the place.
        const bool rowOpen = g_eid && coop::prop_drive_host::ContextOf(prop, g_eid).open;
        if (!rowOpen) TraceAfterEnd(prop, at);
        // The host's own leg: the pusher is the host's pawn, which steps back once the End has landed, so a prop resting against
        // it is not held awake by it when the rest is marked.
        if (!rowOpen && g_leg > kPushes && !g_hostBackDone) {
            void* me = coop::players::Registry::Get().Local();
            if (!g_hostBack) {
                FVector m{};
                if (!me || !E::TryGetActorLocation(me, m)) { Invalid('H', "the host's pawn could not be read to step back"); return; }
                const float len = std::hypot(m.X - at.X, m.Y - at.Y);
                const FVector dir = len > 1.f ? FVector{(m.X - at.X) / len, (m.Y - at.Y) / len, 0.f} : FVector{1.f, 0.f, 0.f};
                const FVector goal{m.X + dir.X * kBackCm, m.Y + dir.Y * kBackCm, m.Z};
                g_hostBack = coop::director::StartBackgroundWalk(goal, kReachCm, kWalkDeadlineS, /*carry=*/false, prop);
                UE_LOGI("[BODY-PUSH-DRILL] [H] the host's End has landed; the host's pawn steps back %.0f cm to (%.0f, %.0f, %.0f)",
                        static_cast<double>(kBackCm), goal.X, goal.Y, goal.Z);
            }
            const int back = g_hostBack->state.load();
            if (back == 0) { Wait('H', "the host's step back after its push"); return; }
            UE_LOGI("[BODY-PUSH-DRILL] [H] the host's step back ended in state %d (1 reached, 2 failed)", back);
            g_hostBack.reset();
            g_hostBackDone = true;
            Restart(g_rest);
            return;
        }
        FRotator rotNow{};
        E::TryGetActorRotation(prop, rotNow);
        if (Stable(g_rest, at, DG::Quiet(prop) && !rowOpen, kMarkTicks, &rotNow)) {
            if (!RC::Read(prop, g_hostRest)) { Invalid('H', "the prop's pose could not be read at push %d's rest", g_leg); return; }
            UE_LOGI("[BODY-PUSH-DRILL] [H] push %d: the host's copy RESTED at (%.1f, %.1f, %.1f)", g_leg, g_hostRest.loc.X,
                    g_hostRest.loc.Y, g_hostRest.loc.Z);
            if (RedHostArm() && g_leg > kPushes && !g_hostRow && !g_clientRow && g_minGap <= kContactCm &&
                Dist(g_hostRest.loc, g_before) > kMovedCm) {
                // Decided here, not at the client's rest line: with the host's push streamed by nobody the client's copy gets
                // no End and never reports one. The row cannot open after the host's copy has rested.
                coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Fail,
                                       "the host's own push moved the host's copy by %.1f cm and its drive row was never opened",
                                       Dist(g_hostRest.loc, g_before));
                UE_LOGI("[BODY-PUSH-DRILL] [H] DONE");
                Next(Step::Done);
                return;
            }
            Next(Step::WaitClientRest);
            return;
        }
        Wait('H', "the host's copy to rest after push %d", g_leg);
        if (Expired(kBindBoundMs)) Invalid('H', "the host's copy never rested after push %d", g_leg);
        return;
    }
    case Step::WaitClientRest: {
        std::string rest;
        if (DC::SeenRestOf(RestedPrefix(g_leg), &rest)) {
            float moved = 0.f;
            RC::Pose client;
            char* end = nullptr;
            moved = std::strtof(rest.c_str(), &end);
            if (end == rest.c_str() || *end != ' ' || !RC::Decode(std::string(end + 1), client)) {
                Invalid('H', "the client's rested line for push %d could not be read: '%s'", g_leg, rest.c_str());
                return;
            }
            FVector clientEnd{};
            bool clientEndOwn = false;
            if (g_leg > kPushes) {
                if (sscanf_s(end + 1, "%*f %*f %*f %*f %*f %*f %f %f %f", &clientEnd.X, &clientEnd.Y, &clientEnd.Z) != 3) {
                    clientEndOwn = rest.size() >= 4 && rest.compare(rest.size() - 4, 4, " own") == 0;
                    if (!clientEndOwn) {
                        Invalid('H', "the client's rested line for the host's push names no End: '%s'", rest.c_str());
                        return;
                    }
                }
            }
            Decide(client, moved, clientEnd, clientEndOwn);
            return;
        }
        Wait('H', "the client's rested line for push %d", g_leg);
        if (Expired(kPushBoundMs)) Invalid('H', "the client's rested line for push %d never came", g_leg);
        return;
    }
    case Step::Done: return;
    }
}

void Reset() {
    g_step = Step::WaitPeer;
    g_armed = false;
    g_slot = 0;
    g_maxStep = 0.f;
    g_leg = 1;
    g_prop.Reset();
    Restart(g_rest);
    g_minGap = 1.0e9f;
    g_contacts = g_moved = 0;
    g_hostBackDone = false;
    g_hostBack.reset();
    g_minPuppetCm = 1.0e9f;
    g_traceTicks = 0;
    g_legMoved = false;
    g_eid = 0;
    g_clientRow = 0;
    g_hostRow = false;
    g_hostWalk.reset();
    g_box.Reset();
    hits::Reset();
}

}  // namespace coop::dev::body_push_drill::detail::host

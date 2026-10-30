// coop/dev/container_intent_drill_throw_into_release.cpp -- the CLIENT's act and its end in the throwinto arm: the throw or the
// let-go of the held drive, the aim and the hold that lead to it, the flight's trace, and the read the client says once the host
// asks. The client's state is `into::g_run`; the steps the arm moves through are in container_intent_drill_throw_into.cpp.

#include "container_intent_drill_throw_into.h"
#include "drive_drill_verbs.h"  // Throw: the player's own throw (co-located private header)

#include "coop/dev/container_route_census.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/drill_chat.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_drop_intent.h"
#include "coop/props/prop_throw.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace coop::dev::container_intent_drill::detail::into {

namespace E  = ue_wrap::engine;
namespace DV = coop::dev::drive_drill_verbs;
namespace DR = coop::director;
namespace DC = coop::dev::drill_chat;

namespace {

void ThrowTheHeld(void* player) {
    MeterBegin(g_run.cm, "client", g_run.packEid);
    // What the path is measured against, read at the release: the trigger box's real world centre and the basket's bounds box.
    TriggerBox real;
    if (!g_run.pack.Get() || !ReadTriggerBox(g_run.pack.Get(), real) || !E::GetActorBounds(g_run.pack.Get(), false, g_run.bOrigin, g_run.bExtent)) {
        ClientFail("the basket's overlap box or bounds do not read");
        return;
    }
    g_run.trigger = real.loc;
    if (!E::TryGetActorLocation(player, g_run.stand)) {   // where the player stands at the release, not where its walk ended
        ClientFail("the player's place does not read at the release");
        return;
    }
    const FVector cam = E::GetCameraLocation();
    const ue_wrap::FRotator camRot = E::GetCameraRotation();
    UE_LOGI("[CI-DRILL] client %s n=%d try %d: release from the camera (%.0f,%.0f,%.0f) pitch %.1f yaw %.1f, %.0f cm from the trigger's centre",
            ArmName(), g_run.r, g_run.t, static_cast<double>(cam.X), static_cast<double>(cam.Y), static_cast<double>(cam.Z),
            static_cast<double>(camRot.Pitch), static_cast<double>(camRot.Yaw), static_cast<double>(Dist3(cam, g_run.trigger)));
    void* const before = coop::prop_drop_intent::dev::LastClientPlaceSpawn();
    if (!DV::Throw(player)) {
        ClientFail("throwHoldingProp did not run");
        return;
    }
    void* const placed = coop::prop_drop_intent::dev::LastClientPlaceSpawn();
    if (!placed || placed == before) {
        ClientFail("the throw spawned no world actor the place hook caught (run with container_route_census=1)");
        return;
    }
    g_run.seq = coop::prop_throw::SeqFor(placed);   // the throw is numbered on this copy, or the host never elects it
    UE_LOGI("[CI-DRILL] client %s n=%d try %d: throw seq %u (minted %u, puppet throwShit ignored n=%u)", ArmName(), g_run.r, g_run.t, g_run.seq,
            coop::prop_throw::dev::Minted(), coop::prop_throw::dev::PuppetIgnored());
    // A throw with no sequence is the arm's n=1 result (the host judges it), not a dead fixture: the try goes on and says seq=0.
    TrackSet(g_run.cm.track, placed);
    ArmingWatchCopy(placed);   // the arming measure stamps this copy's destroy
    ProbeRelease("client", g_run.r, g_run.t, "after the throw", placed, g_run.pack.Get(), g_run.box);
    const FVector v0 = E::GetActorVelocity(placed);
    g_run.v0 = v0;
    UE_LOGI("[CI-DRILL] client %s n=%d try %d: the thrown copy's velocity at the release (%.0f,%.0f,%.0f) cm/s", ArmName(), g_run.r, g_run.t,
            static_cast<double>(v0.X), static_cast<double>(v0.Y), static_cast<double>(v0.Z));
    HitWatchBegin(placed);   // the first blocking hit after the release (container_intent_drill_throw_hit.cpp)
    g_run.hit.Begin(/*releaseKnown=*/true);
    ClientNext(Step::Flight);
}

// The control's let-go: dropGrabObject alone, once the held drive is over the basket.
void DropTheGrab(void* player) {
    void* item = DR::Grabbing(player);
    if (!item || item != g_run.item.Get()) {
        ClientFail("the grab was lost before the drop");
        return;
    }
    g_run.cm.pressMs = cen::UnixMs();   // the release's clock, not the hold's
    ProbeRelease("client", g_run.r, g_run.t, "held, before the let-go", item, g_run.pack.Get(), g_run.box);
    if (!DR::CallOnPlayer(player, L"dropGrabObject")) {   // the grab's let-go alone: no traceThrow, no throwShit
        ClientFail("dropGrabObject did not run");
        return;
    }
    ProbeRelease("client", g_run.r, g_run.t, "after the let-go", item, g_run.pack.Get(), g_run.box);
    g_run.seq = coop::prop_throw::SeqFor(item);
    UE_LOGI("[CI-DRILL] client %s n=%d try %d: dropGrabObject alone, throw seq %u, the grab is %s", ArmName(), g_run.r, g_run.t, g_run.seq,
            DR::Grabbing(player) == item ? "still held" : "released");
    ClientNext(Step::Flight);
}

// The end of a hold with no release: the drive was filed while held (n=3's case; n=2 reads it as filed while held, not a fixture
// failure), or n=3's wait passed with the drive still held and it is let go; the trace of the flight (to its death or its rest) follows.
void HeldInEnd(void* player) {
    const bool alive = g_run.item.Get() != nullptr;
    g_run.filedHeld = !alive;
    g_run.cm.pressMs = cen::UnixMs();
    if (alive && !DR::CallOnPlayer(player, L"dropGrabObject")) {
        ClientFail("dropGrabObject did not run");
        return;
    }
    UE_LOGI("[CI-DRILL] client %s n=%d try %d: the held drive %s; the grab reads %s", ArmName(), g_run.r, g_run.t,
            alive ? "was not filed and is let go" : "died in the box", DR::Grabbing(player) ? "set" : "null");
    ClientNext(Step::Flight);
}

}  // namespace

void StepAim(void* player) {
    char why[160];
    switch (g_run.aimer.Tick(player, g_run.pack.Get(), g_run.stand, why, sizeof(why))) {
    case Aimer::State::Working: return;
    case Aimer::State::Failed:  ClientFail("%s", why); return;
    case Aimer::State::Aimed: ThrowTheHeld(player); return;
    }
}

// The control, closed loop (container_intent_drill_throw_into_hold.cpp): the held drive is brought over the box, measured, and let go.
void StepHold(void* player) {
    char why[200];
    switch (HoldTick(player, g_run.item.Get(), why, sizeof(why))) {
    case HoldState::Working: return;
    case HoldState::Failed:  ClientFail("%s", why); return;
    case HoldState::Ready:   if (g_run.r == 3 || !g_run.item.Get()) HeldInEnd(player); else DropTheGrab(player); return;
    }
}

namespace {

// The settled try, said to the host: what the copy did here. The counts are read later, when the host asks (StepWaitRead).
void Wrap() {
    if (ThrownRun(g_run.r)) {   // the thrown path, measured: the first hit, the closest approach, the box, the launch
        g_run.hit.Finish(g_run.cm.track);
        HitWatchEnd();
        PathPrint("client", g_run.r, g_run.t, g_run.cm.track, g_run.cm.pressMs, g_run.trigger, g_run.bOrigin, g_run.bExtent);
    }
    BoxApproachPrint("client", g_run.r, g_run.t, g_run.cm.track, g_run.cm.pressMs, g_run.box);
    const FVector rest = g_run.cm.track.last;
    if (ThrownRun(g_run.r)) {   // one line a try, so a miss is read without a scout: where it stood, what it aimed at, how it left, where it came to rest
        UE_LOGI("[CI-DRILL] client %s n=%d try %d throw measure: stood at (%.0f,%.0f,%.0f), %.0f cm flat and %.0f cm 3-D from the basket's "
                "centre (%.0f,%.0f,%.0f); aimed at (%.0f,%.0f,%.0f); release velocity (%.0f,%.0f,%.0f) cm/s; rest (%.1f,%.1f,%.1f) is %.0f cm "
                "flat and %.0f cm 3-D from the box centre (%.0f,%.0f,%.0f)", ArmName(), g_run.r, g_run.t, static_cast<double>(g_run.stand.X),
                static_cast<double>(g_run.stand.Y), static_cast<double>(g_run.stand.Z),
                static_cast<double>(std::hypot(g_run.bOrigin.X - g_run.stand.X, g_run.bOrigin.Y - g_run.stand.Y)),
                static_cast<double>(Dist3(g_run.stand, g_run.bOrigin)), static_cast<double>(g_run.bOrigin.X), static_cast<double>(g_run.bOrigin.Y),
                static_cast<double>(g_run.bOrigin.Z), static_cast<double>(g_run.aim.X), static_cast<double>(g_run.aim.Y),
                static_cast<double>(g_run.aim.Z), static_cast<double>(g_run.v0.X), static_cast<double>(g_run.v0.Y), static_cast<double>(g_run.v0.Z),
                static_cast<double>(rest.X), static_cast<double>(rest.Y), static_cast<double>(rest.Z),
                static_cast<double>(std::hypot(rest.X - g_run.trigger.X, rest.Y - g_run.trigger.Y)),
                static_cast<double>(Dist3(rest, g_run.trigger)), static_cast<double>(g_run.trigger.X), static_cast<double>(g_run.trigger.Y),
                static_cast<double>(g_run.trigger.Z));
    }
    UE_LOGI("[CI-DRILL] client %s n=%d %s try %d end: arc=%s gone=%d rest=(%.1f,%.1f,%.1f) mirror from %d", ArmName(), g_run.r, KindOf(g_run.r),
            g_run.t, ArcOf(g_run.cm.track), g_run.cm.track.gone ? 1 : 0, static_cast<double>(rest.X), static_cast<double>(rest.Y),
            static_cast<double>(rest.Z), g_run.cm.c0);
    char say[120];
    std::snprintf(say, sizeof(say), "seq=%u c0=%d gone=%d rest=%d,%d,%d", g_run.seq, g_run.cm.c0, g_run.cm.track.gone ? 1 : 0,
                  static_cast<int>(rest.X), static_cast<int>(rest.Y), static_cast<int>(rest.Z));
    DC::Say(Ln("client", "end", g_run.r, g_run.t, true) + say);
    ClientNext(Step::WaitRead);
}

}  // namespace

// The host has taken its own reads and says so: the mirror's count, the strays and the route census are read now, in the
// same window as the host's.
void StepWaitRead() {
    if (!DC::SeenHas(Ln("host", "read", g_run.r, g_run.t, false))) {
        if (ClientExpired(kNextBoundMs)) ClientFailNow("the host never asked for the read of n=%d try %d within %llu s", g_run.r, g_run.t, U(kNextBoundMs / 1000));
        return;
    }
    const int c1 = CountOf(g_run.packEid);
    const StrayTerms st = StrayTermsOf(g_run.cm, HeldRun(g_run.r));
    const int stray = st.Max();
    const uint64_t adds = Adds(g_run.cm);
    UE_LOGI("[CI-DRILL] client %s n=%d try %d read: mirror %d -> %d, stray=%d (alive=%d excess=%d key=%d), overlaps=%llu, addObject=%llu@%llu",
            ArmName(), g_run.r, g_run.t, g_run.cm.c0, c1, stray, st.alive, st.excess, st.key, U(Overlaps(g_run.cm)), U(adds), U(AddMs(g_run.cm)));
    void* const player = coop::players::Registry::Get().Local();
    char say[230];
    std::snprintf(say, sizeof(say), "c1=%d stray=%d ov=%llu adds=%llu addms=%llu alive=%d cpre=%llu cdes=%llu gr=%d sa=%d se=%d sk=%d fwh=%d", c1, stray,
                  U(Overlaps(g_run.cm)), U(adds), U(AddMs(g_run.cm)), PropAlive(g_run.cm) ? 1 : 0, U(FirstOverlapMs(g_run.cm)), U(ArmingCopyDestroyMs()),
                  player && DR::Grabbing(player) ? 1 : 0, st.alive, st.excess, st.key, g_run.filedHeld ? 1 : 0);
    DC::Say(Ln("client", "late", g_run.r, g_run.t, true) + say);
    ClientNext(Step::WaitNext);
}

void StepFlight(const coop::net::Session& session) {
    TrackTick(g_run.cm.track, session.rttMsForSlot(0));
    if (ThrownRun(g_run.r)) g_run.hit.Tick(g_run.cm.track);
    MeterKey(g_run.cm);
    const uint64_t now = cen::UnixMs();
    if ((Settled(g_run.cm) && now >= g_run.cm.pressMs + kMinTraceMs) || now >= g_run.cm.pressMs + kFlightBoundMs) Wrap();
}

}  // namespace coop::dev::container_intent_drill::detail::into

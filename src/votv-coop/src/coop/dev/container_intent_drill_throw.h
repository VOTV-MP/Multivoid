// coop/dev/container_intent_drill_throw.h -- what the container intent drill's throw arms (throwhand, throwgrab) share
// between their three files: container_intent_drill_throw.cpp (the client's machine), _throw_host.cpp (the host's) and
// _throw_trace.cpp (one actor's trace). A private header of coop/dev/container_intent_drill_throw*.cpp (src tree, not
// include/).
//
// The arms MEASURE; they fail only on a fixture failure. A client throws a drive at a shared backpack from about
// kStandFromTargetCm, from the hotbar hand (throwhand) or from a physics grab (throwgrab); each peer traces the flight,
// the overlap and the final poses, and the host prints the verdict of each throw from both peers' numbers.

#pragma once

#include "container_intent_drill_detail.h"

#include "coop/dev/director/aim_fan.h"

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace coop::dev::container_intent_drill::detail {

// The lines the two peers say to each other on drill chat. Per throw n (1, then 2 after a miss): `ThrowLine` builds them.
inline constexpr const char* kThrowFixtureLine = "ci-throw host fixture ";    // + "pack=<eid> item=<eid>"
inline constexpr const char* kThrowSecondLine  = "ci-throw client second ";   // + "at=<x>,<y>,<z>": the standpoint of throw 2
inline constexpr const char* kThrowItem2Line   = "ci-throw host item2 ";       // + "eid=<eid>" (throwgrab)
inline constexpr const char* kThrowDoneLine    = "ci-throw client done";
inline constexpr const char* kThrowClientFailedLine = "ci-throw client failed";
inline constexpr const char* kThrowHostFailedLine   = "ci-throw host failed";
// "ci-throw <who> <what> n=<n>" + a trailing space when the line carries fields:
//   client arm (held=<eid>), host armed, host thrown (eid=<eid>), client peak (t= v= p=), client end (arc= gone= ov= all= mir= rest=)
std::string ThrowLine(const char* who, const char* what, int n, bool fields);

inline constexpr float    kStandFromTargetCm = 350.f;   // the standpoint of throw 1, before the target
inline constexpr float    kItemSideCm        = 80.f;    // the drive the client takes lies this far to one side of the standpoint
inline constexpr float    kWalkReachCm       = 80.f;    // the director's walk ends this near the standpoint
inline constexpr float    kHitCm             = 60.f;    // a rest this near the target (horizontally) is a hit
inline constexpr float    kMoveMinCm         = 30.f;    // a miss along the line under this does not earn a second throw
inline constexpr float    kMinStandCm        = 120.f;   // the second standpoint is kept in [this, kMaxStandCm] from the target
inline constexpr float    kMaxStandCm        = 800.f;
inline constexpr float    kFlewCm            = 150.f;   // a copy that travelled this far (horizontally) from its first sample flew
inline constexpr uint64_t kSpeedWindowMs     = 50;      // a sample's speed is its displacement over at least this long
inline constexpr uint64_t kRestWindowMs      = 1000;    // at rest: moved under kRestMoveCm over this long
inline constexpr float    kRestMoveCm        = 2.f;
inline constexpr uint64_t kAfterRestMs       = 3000;    // a throw's trace ends this long after rest (or after the actor is gone)
inline constexpr uint64_t kMinTraceMs        = 5000;    // and never sooner than this after the press
inline constexpr uint64_t kFlightBoundMs     = 25000;   // and never later than this after the press

// One actor's trace on one peer: one cached-ref read per tick, the samples kept for the verdict, the lines rate-bounded
// (the first 30, then every 5th, at most 200).
struct Sample {
    uint64_t t;   // the clock of the r4 lines (container_route_census::UnixMs)
    ue_wrap::FVector pos;
    float speed;  // cm/s against the sample at least kSpeedWindowMs older, 0 before there is one
};
struct Track {
    const char* role = "";
    const char* which = "";
    ue_wrap::CachedObjRef ref;
    bool on = false;
    bool gone = false;
    bool atRest = false;
    uint32_t eid = 0;
    int seen = 0;
    int lines = 0;
    std::vector<Sample> samples;
    size_t window = 0;            // the sample the speed is taken against
    ue_wrap::FVector first{};
    ue_wrap::FVector last{};
    float maxHoriz = 0.f;         // the farthest horizontal distance from the first sample
    float peakSpeed = 0.f;        // the largest speed, cm/s
    uint64_t peakT = 0;
    ue_wrap::FVector peakPos{};
    ue_wrap::FVector restRef{};
    uint64_t restRefMs = 0;
    uint64_t restMs = 0;          // the clock at which the actor came to rest (0 while it moves)
    uint64_t goneMs = 0;
};

// Starts a trace of `actor` (null: not yet; Track::on stays false until TrackSet). Clears the samples and reserves room.
void TrackBegin(Track& t, const char* role, const char* which);
void TrackSet(Track& t, void* actor);

// One read: the line (rate-bounded; the one that says the actor is gone always prints), the sample, the summary. `rttMs` is
// printed on the first line and every 20th.
void TrackTick(Track& t, int rttMs);

// "flew" when the copy travelled kFlewCm from its first sample, "fell" when it did not, "none" with no sample.
const char* ArcOf(const Track& t);

// The sample nearest `pos` in space, or null with none.
const Sample* NearestSample(const Track& t, const ue_wrap::FVector& pos, float& gapCm);

float Dist3(const ue_wrap::FVector& a, const ue_wrap::FVector& b);

// The host's state back to the start (container_intent_drill_throw_host.cpp), called by the client's ThrowReset.
void ThrowHostReset();

// ---- the acts of a throw that the thrower's peer runs (container_intent_drill_throw_act.cpp) -------------------------------

// The camera turned at the target's opening point (its location plus its bounds' half-height, read once): the aim fan
// first and, when no pose of it gets the camera's forward within 2 degrees of the direction to the point, a direct look
// at it. Ticked every game tick until Aimed or Failed (`why` filled); the bound is 40 s.
class Aimer {
public:
    enum class State { Working, Aimed, Failed };
    void Begin(int n);
    State Tick(void* player, void* pack, const ue_wrap::FVector& stand, char* why, size_t whyLen);

private:
    coop::director::AimFan fan_;
    ue_wrap::FVector point_{};
    bool read_ = false, direct_ = false;
    uint64_t startMs_ = 0;
    float off_ = 0.f;
    int n_ = 1;
};

// The grabbed `item` thrown by the functions the fire key's grab branch calls, in its order (traceThrow, throwShit, the
// prop's thrown, dropGrabObject). False, with `why` filled, when one did not run.
bool ThrowGrabBranch(void* player, void* item, int n, char* why, size_t whyLen);

// ---- the first blocking hit of a thrown prop (container_intent_drill_throw_hit.cpp) --------------------------------------

inline constexpr uint64_t kFirstHitWindowMs = 1000;   // no hit this long after the release: `first-hit none`
inline constexpr float    kReleaseSpeedCmS  = 200.f;  // a peer that did not throw sees the release as the speed passing this

// Registers the watch of prop_drive_C's component-hit handler (idempotent, game thread) and says whether it is live.
void HitWatchInstall();
bool HitWatchLive();
// Records the hits of `actor` (and of a second actor) from now on; a new Begin forgets the last throw's.
void HitWatchBegin(void* actor);
void HitWatchAdd(void* actor);
void HitWatchEnd();

// Prints the first-hit line of one throw on this peer, once. `releaseKnown`: this peer made the release (the throw's press
// is the release); otherwise the release is read from the trace.
class HitJudge {
public:
    void Begin(bool releaseKnown);
    void Tick(const Track& t);     // per tick
    void Finish(const Track& t);   // the trace is over: a hit not yet judged is `none`
private:
    bool said_ = false;
    size_t scanned_ = 0;
    uint64_t releaseMs_ = 0;
};

// The host-as-thrower arm (container_intent_drill_throw_hostgrab.cpp); the two halves, and their state back to the start.
void HostGrabHostTick(const coop::net::Session& session);
void HostGrabClientTick(const coop::net::Session& session);
void HostGrabReset();

}  // namespace coop::dev::container_intent_drill::detail

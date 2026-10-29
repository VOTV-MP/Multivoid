// coop/dev/container_intent_drill_throw.h -- what the container intent drill's throw arms (throwhand, throwgrab) share
// between their three files: container_intent_drill_throw.cpp (the client's machine), _throw_host.cpp (the host's) and
// _throw_trace.cpp (one actor's trace). A private header of coop/dev/container_intent_drill_throw*.cpp (src tree, not
// include/).
//
// The arms MEASURE; they fail on a fixture failure and on the flight the host judges (the thrower parked, the host's copy too far
// from the thrower's at the instants it says, throwclamp's copy past the throw bound). A client throws a drive at a shared backpack from about
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
//   client arm (held=<eid>), host armed, host thrown (eid=<eid>), client end (arc= gone= ov= all= mir= rest= park= tmax= launch= peakt= peakv= peakp=)
//   and the "inst" lines of SayInstants (part=<i>/<m> press=<ms> then the tuples)
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
    int notSimMoving = 0;         // samples before rest at which the actor's root was not simulating (a parked copy)
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

// ---- the instants of a flight, said by one peer and set against another's trace (container_intent_drill_throw_gap.cpp) ----

inline constexpr int      kAtCount         = 8;       // a peer says where its copy was at this many instants after the press
inline constexpr uint64_t kAtEveryMs       = 250;
inline constexpr uint64_t kAtSlackMs       = 40;      // a sample this near an instant stands for it
inline constexpr float    kFlightGapPassCm = 200.f;   // PROVISIONAL: the largest in-flight gap a green run may print; the proof sets it
inline constexpr float    kRestAgreeCm     = 50.f;    // PROVISIONAL: peers at rest agree within this

// The `n` comma-separated integers after `key` (with its '=') in `text`; false when the key is absent or the list short.
bool IntsAfter(const std::string& text, const char* key, int* out, int n);
// Says the track's sample nearest each instant (pressMs + k * kAtEveryMs) as the inst-lines of ThrowLine: `part=<i>/<m>
// press=<ms> <k>,<ms after the press>,<x>,<y>,<z>;...`, in as few lines as the chat's cap allows (one part with no tuple says none).
void SayInstants(const Track& t, uint64_t pressMs, const char* who, int n);
// What a reader holds of one peer's instants: each part it has seen, kept however late the feed retires the row.
struct Instants {
    static constexpr int kMaxParts = 8;
    struct One { bool have = false; long long dtMs = 0; int p[3] = {}; };
    int m = 0;                  // the parts said (0 until one is read)
    unsigned seen = 0;          // bit i-1: part i is held
    uint64_t pressMs = 0;
    One at[kAtCount + 1];       // by k
};
inline constexpr uint64_t kInstantsBoundMs = 30000;   // the end line seen to the last part: past it the arm fails
// Reads the live inst-lines of `who`'s throw n into `in` (every part seen is kept); called on the drill's poll until complete.
void InstantsCollect(Instants& in, const char* who, int n);
int InstantsHeld(const Instants& in);
bool InstantsComplete(const Instants& in);   // all m parts held
// Sets the track against the instants a peer said: how many were said, how many the track has a sample for, the largest gap.
struct Gap { int matched = 0, said = 0, worstK = 0; float worst = 0.f; };
Gap JudgeInstants(const Track& t, const Instants& in);
// The fastest sample (cm/s) of the track within kEarlyMs after the press: the speed the throw gave the copy.
inline constexpr uint64_t kEarlyMs = 500;
int EarlySpeed(const Track& t, uint64_t pressMs);

// The throw a hand path takes: the hotbar hand's (throwhand, throwclamp), and throwplace's second part. Every other throw
// arm's is a physics grab's.
inline bool HandPath(Arm a, int n) { return a == Arm::ThrowHand || a == Arm::ThrowClamp || (a == Arm::ThrowPlace && n == 2); }

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

// ---- throwplace, throwjoin, throwdepart, throwtouch (container_intent_drill_throw_arms.cpp) -----------------------------------

// What a host-side judge says of its arm on a tick: still waiting, passed, failed (`why` filled), or not measured (said).
enum class HostJudge { Working, Passed, Failed, NotMeasured };

// throwplace, the client: part 1 lets go of the grab (n=1), part 2 places a held drive (n=2). Ticked until Done or Failed.
enum class PlaceState { Working, Done, Failed };
PlaceState PlaceTick(void* player, int n, char* why, size_t whyLen);
// throwdepart, the client: true once it has parked (a row was sent); a relaunched client is idle and reports its seed.
bool DepartParkTick(uint32_t itemEid);
bool DepartRejoinedIdle();
void DepartRejoinedTick();
// throwjoin, the second client: its whole machine.
bool JoinerRole();
void JoinerTick(const coop::net::Session& session);

// The host: the counters at the arm line (before the client's act), the judges per arm, the join's gate and its eid line.
void ArmsHostArmed();
HostJudge PlaceHostJudge(int n, uint64_t ageMs, char* why, size_t whyLen);
void PlaceHostPass();
HostJudge TouchHostTick(uint32_t eid, char* why, size_t whyLen);
HostJudge DepartHostTick(const Track& host, uint32_t eid, char* why, size_t whyLen);
// The joiner's world-ready answer is held from the host's first tick (JoinHoldBegin); the go is the hold having caught its ask.
void JoinHoldBegin(const coop::net::Session& s);
HostJudge JoinGate(const coop::net::Session& s, uint64_t waitedMs, char* why, size_t whyLen);
// At the first admitted row of the throw: the hold is released, the joiner is seeded mid-flight.
void JoinHostRelease();
void JoinHostSay(int n, uint32_t eid);
// Latches whether a client other than slot `thrower` was world-ready before the host copy rested (`hostAtRest`), per throw n.
void JoinHostWatchReady(const coop::net::Session& s, int thrower, int n, bool hostAtRest);
// The throw's one joiner line: bound, world-ready-before-rest, mirror gap.
void JoinHostReport(int n, float gapCm);
// Collects the joiner's instant parts of throw n (a new n starts a new set); on the host's poll while the throw is judged.
void JoinHostCollect(int n);
HostJudge JoinHostJudge(const Track& host, int n, float& gapOut, char* why, size_t whyLen);
void ArmsReset();

// The host-as-thrower arm (container_intent_drill_throw_hostgrab.cpp); the two halves, and their state back to the start.
void HostGrabHostTick(const coop::net::Session& session);
void HostGrabClientTick(const coop::net::Session& session);
void HostGrabReset();

}  // namespace coop::dev::container_intent_drill::detail

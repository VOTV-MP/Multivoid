// coop/dev/basedmove_drill_detail.h -- what the based-movement drill's files share (basedmove_drill.cpp, _sides, _rider, _watch,
// _host, _client). Dev only; see coop/dev/basedmove_drill.h.

#pragma once

#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/types.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace coop::net { class Session; }
namespace coop::director { struct BackgroundWalk; }

namespace coop::dev::basedmove_drill::detail {

inline constexpr const char* kPrefix = "bmv ";
inline constexpr uint64_t kBoundMs = 180000;       // a step that waits this long for its event says INVALID and stops
inline constexpr uint64_t kWalkBoundMs = 480000;   // a walk onto the platform, the director's own deadline included
inline constexpr uint64_t kRideBoundMs = 150000;   // the lift's travel is about twelve seconds; a ride that lasts this long is INVALID

// A body stands on the platform when its feet (the capsule's centre less its half height) are within kFeetTolCm of the
// platform's walkable surface and it is no farther than kEdgeSlackCm past the platform's flat extent. Said once, here, for
// every reading: the boarding, the host's reading of a puppet, the client's of the host's.
inline constexpr float kFeetTolCm = 20.f;
inline constexpr float kEdgeSlackCm = 10.f;

// The arm this peer runs (the basedmove_drill row). Who rides, who presses and who watches:
//   ride        the client rides; the host presses and watches the client's puppet; PASS at the lift's stop, FAIL at a reversal.
//   hostride    the host rides and presses; the client watches the host's puppet against its own platform copy: PASS at the stop,
//               FAIL at the first frame its feet are more than kFeetTolCm off the slab.
//   bad_base_*  the client rides, then forges its claim; the host judges what it does with it.
//   baseend     the client rides; mid-travel the host destroys its copy of the platform actor and judges the puppet it carried.
enum class Arm : uint8_t { Off, Ride, HostRide, BadXy, BadVol, BaseEnd };
Arm ArmOf();
const char* ArmName();
bool Hostile();   // a bad_base arm

// ---- the log, the chat, the verdict and the bound ----
void Fact(const char* fmt, ...);
void Say(const std::string& line);
bool Rest(const std::string& prefix, std::string* rest);   // the text after `prefix` of the last chat line that starts with it
bool Seen(const std::string& line);
int CountOf(const std::string& prefix);                    // how many chat rows of this drill start with `prefix`
void Invalid(const char* why);
void Pass(const char* fmt, ...);                           // `rig: VERDICT basedmove-<arm> PASS`, said once (the host)
void Fail(const char* fmt, ...);                           // FAIL, said where it is decided
bool Ended();
void Finish();                                             // this peer has nothing more to do
void Go();                                                 // a step began: its bound starts again
bool Late(const char* why, uint64_t boundMs = kBoundMs);   // true when the step's bound ran out (it has said INVALID)

// ---- the platform ----
struct PlatformBox {
    ue_wrap::FVector centre{};
    ue_wrap::FVector extent{};   // half sizes of the colliding bounds, which take in what stands on the slab (rails, frame)
    float top = 0.f;             // the bounds' top: NOT a surface a body stands on
    float surface = 0.f;         // the platform's walkable surface at rest, from a down-trace over its centre
    float restZ = 0.f;           // the platform component's world Z when the surface was read; the surface follows it
    float floorZ = 0.f;          // the floor under the platform (another actor's top), where a body stands to reach it
};
// The platform's colliding bounds and its walkable surface: the first upward-facing hit of a down-trace over the bounds'
// centre that strikes the platform's own actor (`ctx` is the trace's world context, any live actor but the platform). Says its
// samples in the log. False when the bounds do not read or the trace strikes no upward-facing surface of the platform.
bool ReadPlatform(void* ctx, void* platformActor, float platformZ, PlatformBox& out);
// Whether a character's feet stand on the platform's surface (the surface moved with the platform: `platformZ` is its
// component's Z now), and the two numbers that say it: the flat distance past the platform's extent (negative inside) and
// the feet's height over the surface.
bool StandsOn(void* character, const PlatformBox& box, float platformZ, float* flatPast = nullptr, float* feetOver = nullptr);
// One side of the platform as a way on: `stand` is the floor point (capsule-centre height) in front of it. A side is OPEN when
// the floor in front of it is the floor the platform rests on (an outside ground or a ledge is not) and nine lines from the
// stand point to the platform's centre -- three heights over the surface, three across the body's width -- strike nothing: no
// rail, wall or door between the floor and the slab. `why` says what decided it, for the log.
struct Side {
    const char* name = "";
    ue_wrap::FVector stand{};
    bool open = false;
    std::string why;
};
// The four sides (+X, -X, +Y, -Y), probed by traces from `ctx`'s world; `halfHeight` is the capsule's.
std::vector<Side> ProbeSides(void* ctx, const PlatformBox& box, float halfHeight);
// "<component>/<owner>" of a movement base, or "none".
std::string BaseName(void* component, void* owner);
std::string NameOf(void* obj);

// ---- boarding: the local player walked onto the platform of `lift` (this peer's copy) ----
class Boarder {
public:
    enum class State { Working, OnPlatform, NoRoute, Failed };
    // Starts (or restarts, at another stop of the lift) a boarding. The lift must be at rest; until it is, Tick waits.
    void Begin(void* lift);
    // One tick. NoRoute: no open side of the platform has a route to its stand point (the caller may bring the lift to its
    // other stop and Begin again). Failed: INVALID was said. OnPlatform: the feet stand on the surface within the tolerance.
    State Tick(void* me);
    const PlatformBox& Box() const { return box_; }
    void* Lift() const { return lift_.Get(); }
    const std::string& SideName() const { return sides_.empty() ? none_ : sides_[stand_]; }

private:
    enum class Phase { Wait, Walk, Step, Done };
    void Prepare(void* me);
    void StartLeg(void* me);
    void StartStep(void* me);
    bool Stalled(void* me, const ue_wrap::FVector& goal, float minGainCm, uint64_t spanMs, const char* what);

    Phase phase_ = Phase::Done;
    ue_wrap::CachedObjRef lift_;
    PlatformBox box_;
    std::vector<ue_wrap::FVector> ring_;   // the stand points before the platform's open sides a route reaches, shortest first
    std::vector<std::string> sides_;
    size_t stand_ = 0;
    std::shared_ptr<coop::director::BackgroundWalk> walk_;
    float bestDist_ = 0.f;
    uint64_t bestAt_ = 0;
    std::string none_ = "none";
};

// ---- watching a puppet ride a lift copy ----
struct Watched {
    bool ok = false;           // everything read this frame
    bool moving = false, flipped = false;
    bool based = false;        // the puppet's latest pose names a mover
    bool attached = false;     // the puppet's root is attached to the platform
    float gap = 0.f;           // attach Z - platform Z: what the lift's own block test reads (it reverses at 10)
    float d = -1.f;            // |puppet - (platform now + the rider's offset from its latest pose)|, -1 without a base
    float feet = 0.f;          // the puppet's feet over the platform's surface
    ue_wrap::FVector at{};     // the puppet's place this frame
};
// The tallies and per-frame log of one watch. The press sets `moving`; a reversal is `dir` changing once it moved.
class Watcher {
public:
    void Begin(int puppetSlot) { *this = Watcher(); slot_ = puppetSlot; start_ = std::chrono::steady_clock::now(); }
    Watched Frame(coop::net::Session& s, void* lift, const PlatformBox& box);
    bool SawMoving() const { return sawMoving_; }
    int frames = 0, flips = 0, basedFrames = 0, attachedFrames = 0;
    float maxGap = 0.f, maxD = 0.f, maxFeet = 0.f;

private:
    int slot_ = 0;
    bool sawMoving_ = false, prevDir_ = false;
    std::chrono::steady_clock::time_point start_{};
};

// The two peers' connection gates.
bool HostMayStart(coop::net::Session& s);
bool ClientMayStart();

// ---- each peer's steps, and the tick ----
void HostTick(coop::net::Session& s);
void ClientTick(coop::net::Session& s);

}  // namespace coop::dev::basedmove_drill::detail

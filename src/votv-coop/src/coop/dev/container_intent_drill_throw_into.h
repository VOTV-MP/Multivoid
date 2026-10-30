// coop/dev/container_intent_drill_throw_into.h -- what the throwinto arm's two halves share: the lines the peers say to each
// other, the bounds both read, and one peer's reading of one try (the basket's count, the live drives, the overlaps and the
// addObject verbs of the overlap route). A private header of coop/dev/container_intent_drill_throw_into*.cpp (src tree, not
// include/). See container_intent_drill_throw_into.cpp for the arm.

#pragma once

#include "container_intent_drill_throw.h"

#include "coop/dev/container_route_census.h"
#include "coop/element/registry.h"
#include "coop/player/hand_item.h"

#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/cached_obj_ref.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine_nav.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace coop::net {
class Session;
}

namespace coop::dev::container_intent_drill::detail::into {

namespace cen = coop::dev::container_route_census;
using ue_wrap::FVector;

inline constexpr int      kR4Route        = 3;       // container_route_census::RouteVerbs: R1..R6 are 0..5
inline constexpr int      kTries          = 3;       // a sub-run's tries: the first and at most two after a miss
inline constexpr float    kIntoStandCm    = 70.f;    // the thrown hand drive's standpoint, from the basket
inline constexpr const wchar_t* kBasketPath = L"/Game/objects/prop_container_pbasket.prop_container_pbasket_C";   // the target: an open basket, trigger 40x20x16 cm at z30
inline constexpr uint64_t kPeerBoundMs    = 120000;  // a world-ready client; the client's wait for the fixture and its mirrors
inline constexpr uint64_t kArmBoundMs     = 240000;  // a try's start to the client's arm line (its walk, its hand or grab, the chat)
inline constexpr uint64_t kPublishMs      = 3000;    // the contents' publication: this long after the prop settled

// The navmesh round a target: a fan of kFan directions, each point projected within kNavSnapCm.
inline constexpr int   kFan          = 8;
inline constexpr float kNavSnapCm    = 25.f;
inline constexpr float kRingCm       = 100.f;   // the host's ring check: at least kRingMin of the kFan points at this distance have a route from the client
inline constexpr int   kRingMin      = 4;
inline constexpr float kRingStepCm   = 100.f;   // a spot without a ring moves along the client's facing by this, kRingSteps times at most
inline constexpr int   kRingSteps    = 8;
inline constexpr float kIntoWidenCm  = 120.f;   // the thrown standpoint, widened to this when none fits at kIntoStandCm
inline constexpr float kIntoWiden2Cm = 180.f;   // and then to this

inline constexpr const char* kFixtureLine      = "ci-tinto host fixture ";   // + "pack=<eid> item=<eid>"
inline constexpr const char* kDoneLine         = "ci-tinto host done";
inline constexpr const char* kHostFailedLine   = "ci-tinto host failed";
inline constexpr const char* kClientFailedLine = "ci-tinto client failed";

inline unsigned long long U(uint64_t v) { return static_cast<unsigned long long>(v); }
inline bool EidBound(uint32_t eid) { return eid != static_cast<uint32_t>(coop::element::kInvalidId); }
// The sub-runs: 1 a thrown hand drive, 2 a grabbed drive let go over a basket, 3 a grabbed drive lowered into the box and not let go,
// 4 a thrown hand drive into a basket the fixture did not arm (the client's take armed it), 5 the same with two clients: the first takes,
// the second throws. Only a run with a second client world-ready is 5.
inline constexpr int kLastRun = 5;
inline bool ThrownRun(int r) { return r == 1 || r == 4 || r == 5; }
inline bool HeldRun(int r) { return r == 2 || r == 3; }
inline bool TakenRun(int r) { return r == 4 || r == 5; }
inline const char* KindOf(int r) { return r == 1 ? "thrown" : r == 2 ? "control" : r == 3 ? "held" : r == 4 ? "taken" : "observed"; }

// "ci-tinto <who> <what> r=<sub-run> t=<try>" and a trailing space when the line carries fields.
inline std::string Ln(const char* who, const char* what, int r, int t, bool fields) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "ci-tinto %s %s r=%d t=%d%s", who, what, r, t, fields ? " " : "");
    return buf;
}

// The value of the token `key=` (a whole token) in `text`.
inline bool Num(const std::string& text, const char* key, long long& out) {
    const std::string k = std::string(key) + "=";
    for (size_t at = text.find(k); at != std::string::npos; at = text.find(k, at + 1)) {
        if (at != 0 && text[at - 1] != ' ') continue;
        const char* begin = text.c_str() + at + k.size();
        char* end = nullptr;
        const long long v = std::strtoll(begin, &end, 10);
        if (end == begin) return false;
        out = v;
        return true;
    }
    return false;
}

inline bool Unit(const FVector& from, const FVector& to, float& ux, float& uy, float& len) {
    ux = to.X - from.X;
    uy = to.Y - from.Y;
    len = std::hypot(ux, uy);
    if (len < 1.f) return false;
    ux /= len;
    uy /= len;
    return true;
}

inline int LiveDrives() {
    static std::vector<uint32_t> ids;
    LiveDriveEids(ids);
    return static_cast<int>(ids.size());
}

// ---- what one peer reads of one try -------------------------------------------------------------------------------------

struct Meter {
    Track    track;
    int      c0 = -1;               // the basket's record count before the act
    int      drives0 = 0;           // the live drives then
    uint64_t ovAll0 = 0, noSim0 = 0, adds0 = 0, pressMs = 0;
    std::wstring itemKey;           // the traced item's Key (its save UUID), read once the actor carries one
};

inline void MeterBegin(Meter& m, const char* role, uint32_t packEid) {
    TrackBegin(m.track, role, "element");
    m.c0 = CountOf(packEid);
    m.drives0 = LiveDrives();
    m.ovAll0 = cen::R4ItemOverlaps();
    m.noSim0 = cen::R4ItemOverlapsNoSim();
    cen::MarkItemOverlaps();
    m.adds0 = cen::RouteVerbs(kR4Route, /*take=*/false);
    cen::MarkVerbs(kR4Route, /*take=*/false);
    m.pressMs = cen::UnixMs();
    m.itemKey.clear();
}

// The census counts both as they happen: the overlaps of any shared container by a shared actor, and the addObject verbs of the
// overlap route; the clock of the first of those verbs is stamped by the census where it counts it. Nothing here polls them.
inline uint64_t Overlaps(const Meter& m) { return cen::R4ItemOverlaps() - m.ovAll0; }
inline uint64_t OverlapsNoSim(const Meter& m) { return cen::R4ItemOverlapsNoSim() - m.noSim0; }
inline uint64_t FirstOverlapMs(const Meter&) { return cen::FirstItemOverlapMs(); }
inline uint64_t Adds(const Meter& m) { return cen::RouteVerbs(kR4Route, false) - m.adds0; }
inline uint64_t AddMs(const Meter&) { return cen::FirstVerbMs(kR4Route, false); }

// Per tick: the traced item's Key, which a straggler gets a tick or more after its birth and the actor takes with it when it goes.
inline void MeterKey(Meter& m) {
    if (m.itemKey.empty() || m.itemKey == L"None") {
        if (void* a = m.track.ref.Get()) m.itemKey = ue_wrap::prop::GetInteractableKeyString(a);
    }
}

// The copy came to rest, or is gone, and the publication of the contents has had kPublishMs.
inline bool Settled(const Meter& m) {
    const uint64_t now = cen::UnixMs();
    return (m.track.atRest && now >= m.track.restMs + kPublishMs) || (m.track.gone && now >= m.track.goneMs + kPublishMs);
}

// The traced actor is still a live world actor (read fresh: a trace that stopped ticking keeps its last `gone`).
inline bool PropAlive(const Meter& m) { return m.track.on && m.track.ref.Get() != nullptr; }

// The LIVE world drives (the GUObjectArray, registered or not; an actor destroyed and not yet collected is no copy) that carry the
// item's Key, its save UUID, which a duplicate of it shares, the local hand's view actor left out. 0 while the item has no key.
inline int WorldDrivesWithKey(const std::wstring& key) {
    if (key.empty() || key == L"None") return 0;
    void* const hand = coop::hand_item::LocalHandActor();
    int n = 0;
    for (void* a : ue_wrap::reflection::FindObjectsByClass(L"prop_drive_C"))
        if (a != hand && ue_wrap::reflection::IsLive(a) && ue_wrap::prop::GetInteractableKeyString(a) == key) ++n;
    return n;
}

// A live world copy of the drive left on this peer, three ways: the traced actor itself; a drive beyond those the act accounts
// for (the grabbed one is consumed by a landing, the thrown one is born by the throw); a drive anywhere that carries the item's key.
// The three terms are kept apart so every verdict line can print them.
struct StrayTerms {
    int alive = 0, excess = 0, key = 0;
    int Max() const { return std::max({alive, excess, key}); }
};
inline StrayTerms StrayTermsOf(const Meter& m, bool control) {
    StrayTerms t;
    t.alive = PropAlive(m) ? 1 : 0;
    t.excess = LiveDrives() - (m.drives0 - (control ? 1 : 0));
    t.key = WorldDrivesWithKey(m.itemKey);
    return t;
}
inline int StrayOf(const Meter& m, bool control) { return StrayTermsOf(m, control).Max(); }

// Fan point `i` of kFan round `c`, `cm` out, starting at the angle `baseRad`.
inline FVector FanPoint(const FVector& c, float baseRad, int i, float cm) {
    const float a = baseRad + static_cast<float>(i) * (6.28318531f / static_cast<float>(kFan));
    return {c.X + std::cos(a) * cm, c.Y + std::sin(a) * cm, c.Z};
}

// The navmesh point under `want`, within kNavSnapCm of it horizontally (E::ProjectToNav, as coffee_drill_cast does).
inline bool NavSnap(void* ctx, const FVector& want, FVector* out) {
    FVector p{};
    if (!ue_wrap::engine::ProjectToNav(ctx, want, FVector{kNavSnapCm, kNavSnapCm, 120.f}, &p)) return false;
    if (std::hypot(p.X - want.X, p.Y - want.Y) > kNavSnapCm) return false;
    if (out) *out = p;
    return true;
}

// What a thrown copy's path did against the target (container_intent_drill_throw_into_path.cpp): one line, from the samples of
// `tr`, `pressMs` the release's clock, `trigger` the trigger's centre, `origin` and `extent` the target's bounds box.
void PathPrint(const char* role, int n, int t, const Track& tr, uint64_t pressMs, const FVector& trigger, const FVector& origin,
               const FVector& extent);

// The target's trigger, armed the game's own way (container_intent_drill_throw_into_arm.cpp): the `overlap` box's
// GetCollisionEnabled (0 none, 1 query only, 2 physics only, 3 both; -1 unreadable), and a call of the container's dingus().
// kDingusWaitMs: how long the measure tail waits after a call before it reads the box (the container's own overlapDelay of 0.1 s and a tick).
inline constexpr uint64_t kDingusWaitMs = 200;
int TriggerState(void* container);
bool CallDingus(void* container);
// A reflected call of the parameterless function `fnName` on `obj`; false when it does not resolve or run.
bool CallParamless(void* obj, const wchar_t* fnName);

// A primitive component's GetCollisionEnabled (0 none, 1 query only, 2 physics only, 3 both; -1 unreadable).
int CollisionOf(void* comp);
// The target's `overlap` box component, or null.
void* TriggerComponent(void* container);

// The measuring of the drive and of the target's trigger box (container_intent_drill_throw_into_probe.cpp): the box in the world.
struct TriggerBox {
    FVector loc{}, ext{};          // its world location and scaled half-extent
    ue_wrap::FRotator rot{};
    float zReach = 0.f;            // the half-extent's reach along world Z with the box's rotation taken in (its top face is loc.Z + this)
    bool ok = false;
};
bool ReadTriggerBox(void* container, TriggerBox& out);
// Whether a world point lies inside the box (its rotation taken in).
bool PointInBox(const TriggerBox& box, const FVector& p);
// The steps of a track near the box: the longest step between two consecutive samples whose later sample lay within `withinCm`
// of the box, the time between those two samples, the speed that step is (step / dt), the mean time between consecutive near
// samples (the tick's own dt: one sample is taken per tick) and the per-frame step at that speed and that frame time, which is what
// a step per FRAME would be (speed x frame dt); a step per ROW would be nearer speed x the rows' interval.
struct StepStats {
    float step = 0.f, speed = 0.f, frameMs = 0.f, perFrame = 0.f;
    uint64_t dtMs = 0;
    int nearCount = 0;
};
StepStats StepsNearBox(const Track& tr, const TriggerBox& box, float withinCm);
// At a release: the drive's root and static-mesh component and the target's box, their overlap events, collision state, object type
// and responses by reflection, and the box's world place into `box`.
void ProbeRelease(const char* role, int n, int t, const char* when, void* drive, void* container, TriggerBox& box);
// After the copy settled: the closest approach of the drive's path to the box.
void BoxApproachPrint(const char* role, int n, int t, const Track& tr, uint64_t pressMs, const TriggerBox& box);

// The arming measure of a container's box (container_intent_drill_throw_into_arming.cpp; the dev row
// container_arming_measure, default off): a native post hook on SetCollisionEnabled, a PRE watch on dingus, the rate of the hook,
// and a tail run after the sub-runs. Nothing is cancelled.
bool ArmingOn();
void ArmingInstall(bool host);   // the hook and (client) the PRE watch; idempotent
void ArmingTrack(void* container);   // a shared container's `overlap` component joins the pointer set
void ArmingRoute(const char* label, uint64_t windowMs = 0); // matched calls in the next second (or windowMs) belong to this route ("ourcall", "ui", "m3")
void ArmingTick();               // the rate line every 5 s, once per tick; a clock compare otherwise
enum class TailState : uint8_t { Working, Done, Failed };
void ArmingTailBegin(bool host, void* basket, uint32_t basketEid);
TailState ArmingTailTick(bool host, bool due, char* why, size_t whyLen);
bool ArmingTailAsked(uint32_t* eid);   // the host has started the tail (client), and the eid of the basket it fills
void ArmingReset();
// m4, per try from the host's verdict: does the host's copy (parked while a client flies it) produce the container's overlap, and how far it
// moves per sample near the box; the order of the host's PRE entry, the client's and the client's destroy of the copy.
struct M4Try {
    int r = 0, t = 0;
    uint64_t hostOverlaps = 0, hostNoSim = 0;   // the host's PRE entries by an item in the try, and those with the component not simulating
    float maxStep = 0.f, boxMin = 0.f;          // the largest step near the box; the box's smallest full extent
    uint64_t stepDtMs = 0;                      // the time between the two samples of that step
    float speed = 0.f, frameMs = 0.f, perFrame = 0.f;   // that step as a speed; the mean sample interval; speed x frame time
    int nearSamples = 0;
    uint64_t hostPreMs = 0, clientPreMs = 0, clientDestroyMs = 0;   // 0: none
};
void ArmingM4Note(const M4Try& m);
void ArmingWatchCopy(void* actor);   // the thrown copy's destroy is stamped where the engine ends its play
uint64_t ArmingCopyDestroyMs();

// The held runs' hold loop (container_intent_drill_throw_into_hold.cpp): the held drive is brought to a point over the target's box
// (n=2: over its top face, measured, and let go by the client once Ready) or to the box's centre (`inside`, n=3: the drive is
// never let go; Ready is the prop's death, or a wait that passed while it stayed held).
enum class HoldState : uint8_t { Working, Ready, Failed };
// The point over the box's real centre, the held item's own bounds radius plus kOverRimCm over its top face (so the held drive cannot touch
// the trigger before the release); inside: the centre.
bool HoldReadPoint(void* container, bool inside, void* item);
float HoldStandCm(float fallbackCm);       // the horizontal distance from that point the standpoint aims at
bool HoldBegin(void* container, int r, int t, bool inside, void* item);
HoldState HoldTick(void* player, void* item, char* why, size_t whyLen);
void HoldReset();

// The preamble of a taken run (container_intent_drill_throw_into_take.cpp): the taker names itself (need), waits for a basket holding
// one record and takes it through the game's own screen, which makes the host arm it; a watcher (n=5's second client) only waits for
// the basket and the taker's word and reads its own mirror's box armed. PreTick is ticked on the drill's poll until Done
// (out: the basket's mirror, its eid and place) or Failed (why filled).
enum class PreState : uint8_t { Working, Done, Failed };
struct PreOut {
    void* pack = nullptr;
    uint32_t eid = 0;
    FVector at{};
};
bool FillBasket(void* basket);   // the host adds one drive record to the basket, no route counted
void PreBegin(int r, const FVector& at, bool taker);
PreState PreTick(char* why, size_t whyLen, PreOut& out);
void PreReset();

// n=5, two clients (container_intent_drill_throw_into_observer.cpp). The second client is the one with a connected peer of a lower slot; it
// idles through n=1..4 (IdleTick: Start when the host has begun n=5, Done at the host's done line). The first client, after its take, only
// watches: it reads its mirror around each of the second's tries and answers the host's read (ObsTick, Done at the host's done line).
bool SecondClient();
enum class IdleState : uint8_t { Working, Start, Done };
IdleState IdleTick();
enum class ObsState : uint8_t { Working, Done, Failed };
void ObsBegin(uint32_t packEid);
ObsState ObsTick(char* why, size_t whyLen);
void ObsReset();

// The red container_throwinto_unarmed (BOTH peers): the host does not arm the fixtures' baskets and the client does not wait for the box.
bool UnarmedOn();

// ---- the client's run (container_intent_drill_throw_into.cpp holds the steps, ..._release.cpp the act and its end) -----------

enum class Step : uint8_t { WaitLine, WaitMirrors, Rest, Arming, ItemWait, Grab, WalkWait, TakeHand, HandRead, ArmWait, Aim, Hold, Flight,
                            WaitRead, WaitNext, Pre, Idle, Obs, Tail, Held, Done };
inline constexpr uint64_t kNextBoundMs = 240000;   // the end line to the host's verdict line

// What the client's steps and its release group share.
struct ClientRun {
    int      r = 1, t = 1;                 // the sub-run and the try
    uint32_t packEid = 0;
    ue_wrap::CachedObjRef pack, item;
    FVector  stand{};
    bool     filedHeld = false;            // the held drive was filed by the host before any release
    Aimer    aimer;
    Meter    cm;
    HitJudge hit;                          // the first hit of the thrown copy after its release
    FVector  trigger{}, bOrigin{}, bExtent{};   // the thrown path is measured against these
    TriggerBox box;                        // the target's overlap box in the world, read at the release
    FVector  aim{}, v0{};                  // a thrown try: the point the camera was aimed at, and the copy's velocity at the release
    unsigned seq = 0;
};
extern ClientRun g_run;

void ClientNext(Step s);
bool ClientExpired(uint64_t bound);
void ClientFail(const char* fmt, ...);
void ClientFailNow(const char* fmt, ...);   // no hold: the host's answer is what failed, or the arm cannot go on

// The release group (container_intent_drill_throw_into_release.cpp): the aim, the hold, the flight and the read of the host's ask.
void StepAim(void* player);
void StepHold(void* player);
void StepFlight(const coop::net::Session& session);
void StepWaitRead();

}  // namespace coop::dev::container_intent_drill::detail::into

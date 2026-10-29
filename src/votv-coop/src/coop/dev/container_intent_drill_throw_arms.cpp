// coop/dev/container_intent_drill_throw_arms.cpp -- what four of the container intent drill's throw arms add to the
// flight the client and the host trace (container_intent_drill_throw.cpp, _throw_host.cpp); see
// container_intent_drill_throw.h.
//   throwplace  -- the bit stays CLEAR on a non-throw: the client lets go of a grab with dropGrabObject alone, then places a
//                  held drive with the game's precision place; the host reads throwSeq 0 on the release and the drop intent
//                  and elects no syncer.
//   throwjoin   -- (--observer) a second client joins while the client's grab throw flies; once world-ready it follows the
//                  relayed rows, and the host sets its copy against the joiner's instants.
//   throwdepart -- the client leaves at the first admitted row; the host takes the prop back at the last row's velocity, the
//                  copy flies on and rests, and the rejoined client's seed agrees with the host's rest pose.
//   throwtouch  -- the host picks the flying prop up 150 ms after the first admitted row; the thrower's row ends with it.

#include "container_intent_drill_put.h"        // Put2IsJoiner: putjoin's role rule
#include "container_intent_drill_throw.h"
#include "container_route_drill_detail.h"

#include "coop/dev/container_route_census.h"
#include "coop/dev/director/aimed_grab.h"
#include "coop/dev/drill_chat.h"
#include "coop/element/registry.h"
#include "coop/net/session.h"
#include "coop/net/throw_bounds.h"
#include "coop/player/players_registry.h"
#include "coop/props/prop_syncer.h"
#include "coop/props/prop_syncer_host.h"
#include "coop/props/prop_throw.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_mainplayer.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace coop::dev::container_intent_drill::detail {
namespace {

namespace E   = ue_wrap::engine;
namespace R   = ue_wrap::reflection;
namespace DR  = coop::director;
namespace DC  = coop::dev::drill_chat;
namespace RD  = coop::dev::container_route_drill::detail;
namespace cen = coop::dev::container_route_census;
using ue_wrap::FVector;

constexpr uint64_t kPlaceModeMs     = 500;      // the precision place's mode is entered this long before the drop
constexpr uint64_t kPlaceLandMs     = 10000;    // the placed prop's release or intent reaches the host within this
constexpr uint64_t kTouchAfterRowMs = 150;      // the host's grab follows the first admitted row by this long
constexpr uint64_t kTouchVerifyMs   = 3000;     // the grab is on the player, and the row ended, within this
constexpr float    kDepartSpeedPct  = 0.20f;    // the host copy's speed after the take-back is within this of the last row's
constexpr uint64_t kDepartSampleMs  = 50;       // the host copy's speed is read from the first sample this long after the take-back
constexpr uint64_t kDepartTakeSlackMs = 500;    // the take-back follows the last admitted row by kThrowSilenceMs plus this
constexpr uint64_t kRejoinBoundMs   = 240000;  // the take-back to the rejoined client's seed line
constexpr uint64_t kJoinWaitMs      = 150000;   // the host's wait for a second client to be connected
constexpr uint64_t kMarkEveryMs     = 1000;     // the host says the thrown eid to a joiner this often

unsigned long long U(uint64_t v) { return static_cast<unsigned long long>(v); }

// ---- throwplace -----------------------------------------------------------------------------------------------------------

enum class P : uint8_t { Start, WaitMode };
P        g_p = P::Start;
uint64_t g_pMs = 0;

// The host's baseline at the arm line, and what it concluded of each part.
struct Base { uint32_t rel = 0, relThrown = 0, intents = 0, intentsThrown = 0, elected = 0; };
Base     g_base;
const char* g_placeGrab = "?";    // "0" once the grab's release read throwSeq 0 with no election
const char* g_placeHand = "n/a";  // "0", or "n/a" when the place verb did not land

// ---- throwtouch -----------------------------------------------------------------------------------------------------------

uint64_t g_touchRowMs = 0, g_touchGrabMs = 0;
bool     g_touchGrabbed = false;

// ---- throwdepart ----------------------------------------------------------------------------------------------------------

bool     g_departRejoinRead = false, g_departRejoined = false;
uint32_t g_departTakeBacks0 = 0, g_departSilence0 = 0;
uint64_t g_departTakeMs = 0, g_departSayMs = 0, g_departAfterRowMs = 0;
float    g_departRowSpeed = 0.f;
bool     g_departSpeedJudged = false;

// ---- throwjoin ------------------------------------------------------------------------------------------------------------

Track    g_jTrack;
uint32_t g_jEid = 0;
int      g_jN = 0, g_jLastN = 0;   // the throw traced now, and the last one measured
uint64_t g_jFirstMs = 0, g_jEndAtMs = 0, g_markMs = 0;
bool     g_jDone = false;

}  // namespace

// ---- throwplace: the client ----------------------------------------------------------------------------------------------

// The first part lets go of the grab with dropGrabObject alone: no traceThrow, no throwShit (mainPlayer.cpp:11150). The second
// enters the placement mode as the game's own place does, simulateDrop(dontWakeup=false, place=true, dontCollect=true)
// (mainPlayer.cpp:1458, :9942), and drops with the custom event precisionPlacemet_drop (:9972, reached from :1617).
PlaceState PlaceTick(void* player, int n, char* why, size_t whyLen) {
    if (n == 1) {
        if (!DR::CallOnPlayer(player, L"dropGrabObject")) {
            std::snprintf(why, whyLen, "dropGrabObject did not run");
            return PlaceState::Failed;
        }
        UE_LOGI("[CI-DRILL] %s %s: dropGrabObject alone released the grab (no traceThrow, no throwShit)", Role(), ArmName());
        return PlaceState::Done;
    }
    if (g_p == P::Start) {
        void* fn = R::FindDispatchFunctionCached(R::ClassOf(player), L"simulateDrop");
        ue_wrap::ParamFrame f(fn);
        if (!fn || !f.valid() || !f.Set<bool>(L"dontWakeup", false) || !f.Set<bool>(L"place", true) ||
            !f.Set<bool>(L"dontCollect", true) || !ue_wrap::Call(player, f)) {
            std::snprintf(why, whyLen, "simulateDrop(place=true) did not run");
            return PlaceState::Failed;
        }
        g_p = P::WaitMode;
        g_pMs = ::GetTickCount64();
        return PlaceState::Working;
    }
    if (::GetTickCount64() - g_pMs < kPlaceModeMs) return PlaceState::Working;
    if (!DR::CallOnPlayer(player, L"precisionPlacemet_drop")) {
        std::snprintf(why, whyLen, "precisionPlacemet_drop did not run");
        return PlaceState::Failed;
    }
    UE_LOGI("[CI-DRILL] %s %s: precisionPlacemet_drop after the placement mode", Role(), ArmName());
    return PlaceState::Done;
}

// ---- throwplace: the host ------------------------------------------------------------------------------------------------

void ArmsHostArmed() {
    g_base.rel = coop::prop_throw::dev::ReleasesSeen();
    g_base.relThrown = coop::prop_throw::dev::ReleasesThrown();
    g_base.intents = coop::prop_throw::dev::IntentsSeen();
    g_base.intentsThrown = coop::prop_throw::dev::IntentsThrown();
    g_base.elected = coop::prop_syncer_host::dev::ThrowViewNow().elected;
    g_departTakeBacks0 = coop::prop_syncer_host::dev::ThrowViewNow().slotLeft;
    g_departSilence0 = coop::prop_syncer_host::dev::ThrowViewNow().silence;
    g_touchRowMs = g_touchGrabMs = g_departTakeMs = g_departSayMs = g_departAfterRowMs = 0;
    g_touchGrabbed = false;
    g_departSpeedJudged = false;
    g_jDone = false;
}

HostJudge PlaceHostJudge(int n, uint64_t ageMs, char* why, size_t whyLen) {
    const bool grab = n == 1;
    const bool landed = grab ? coop::prop_throw::dev::ReleasesSeen() > g_base.rel : coop::prop_throw::dev::IntentsSeen() > g_base.intents;
    if (!landed) {
        if (ageMs < kPlaceLandMs) return HostJudge::Working;
        if (grab) {
            std::snprintf(why, whyLen, "the release of the let-go grab never reached the host within %llu s", U(kPlaceLandMs / 1000));
            return HostJudge::Failed;
        }
        UE_LOGI("[CI-DRILL] host throwplace hand: NOT MEASURED (the place verb did not land)");
        g_placeHand = "n/a";
        return HostJudge::NotMeasured;
    }
    const uint32_t thrown = grab ? coop::prop_throw::dev::ReleasesThrown() - g_base.relThrown : coop::prop_throw::dev::IntentsThrown() - g_base.intentsThrown;
    const uint32_t elected = coop::prop_syncer_host::dev::ThrowViewNow().elected - g_base.elected;
    UE_LOGI("[CI-DRILL] host throwplace %s: the host received it with throwSeq != 0 on %u of it, elected %u", grab ? "grab" : "hand", thrown, elected);
    if (thrown != 0) {
        std::snprintf(why, whyLen, "the %s of a place carried a throw sequence (the bit is set on a non-throw)", grab ? "release" : "drop intent");
        return HostJudge::Failed;
    }
    if (elected != 0) {
        std::snprintf(why, whyLen, "a place elected a syncer");
        return HostJudge::Failed;
    }
    (grab ? g_placeGrab : g_placeHand) = "0";
    return HostJudge::Passed;
}

void PlaceHostPass() {
    UE_LOGI("[CI-DRILL] host throwplace PASS grab seq=%s hand seq=%s elected=0 (puppet throwShit ignored n=%u)", g_placeGrab, g_placeHand,
            coop::prop_throw::dev::PuppetIgnored());
}

// ---- throwtouch: the host ------------------------------------------------------------------------------------------------

// The host's pawn takes the flying prop with the game's own pickup of a named actor, `Hold Object` with `manual` set
// (mainPlayer.cpp:16196; ue_wrap::engine::CallMainPlayerHoldObject): no aim, not a teleport, not a dev write. The pickup
// equips the drive and the world prop leaves, so the thrower's row ends with the prop (Died) and its copy goes with the
// host's destroy. A physics grab of the flying prop needs an aim at a moving target and is not measured here.
HostJudge TouchHostTick(uint32_t eid, char* why, size_t whyLen) {
    const auto& tv = coop::prop_syncer_host::dev::ThrowViewNow();
    const uint64_t now = ::GetTickCount64();
    if (!g_touchRowMs) {
        if (tv.lastEid == eid && tv.lastRows >= 1 && tv.elected > g_base.elected) g_touchRowMs = now;
        return HostJudge::Working;
    }
    void* prop = coop::element::LivePropActor(eid);
    void* player = coop::players::Registry::Get().Local();
    if (!g_touchGrabMs) {
        if (now - g_touchRowMs < kTouchAfterRowMs) return HostJudge::Working;
        if (!prop || !player) {
            std::snprintf(why, whyLen, "the flying prop or the host's player is gone before the pickup");
            return HostJudge::Failed;
        }
        bool collected = false;
        const bool called = ue_wrap::engine::CallMainPlayerHoldObject(player, prop, collected);
        g_touchGrabbed = called && collected;
        g_touchGrabMs = now;
        UE_LOGI("[CI-DRILL] host throwtouch: the pickup ran %llu ms after the first admitted row (called %d, collected %d)",
                U(now - g_touchRowMs), called ? 1 : 0, collected ? 1 : 0);
        if (!g_touchGrabbed) {
            UE_LOGI("[CI-DRILL] host throwtouch: NOT MEASURED (the game's pickup refused the flying prop)");
            return HostJudge::NotMeasured;
        }
        return HostJudge::Working;
    }
    if (!prop) {
        UE_LOGI("[CI-DRILL] host throwtouch: the host picked the prop up mid-flight and it left the world %llu ms later",
                U(now - g_touchGrabMs));
        return HostJudge::Passed;
    }
    if (now - g_touchGrabMs < kTouchVerifyMs) return HostJudge::Working;
    std::snprintf(why, whyLen, "the host's pickup was collected and the flying prop was still in the world after %llu s",
                  U(kTouchVerifyMs / 1000));
    return HostJudge::Failed;
}

// ---- throwdepart: the client ---------------------------------------------------------------------------------------------

bool DepartParkTick(uint32_t itemEid) {
    const auto v = coop::prop_syncer::dev::EntryOf(itemEid);
    if (v.state != coop::prop_syncer::dev::EntryState::Syncing || v.rowsSent < 1) return false;
    coop::prop_syncer::dev::MuteThrownRows();   // the thrower goes silent mid-flight before the rig drops it, as a crash would
    DepartMarkParked();
    UE_LOGI("[CI-DRILL] client throwdepart parked");   // the rig drops this client on this line and relaunches it
    return true;
}

bool DepartRejoinedIdle() {
    if (ArmOf() != Arm::ThrowDepart) return false;
    if (!g_departRejoinRead) {
        g_departRejoinRead = true;
        g_departRejoined = DepartRejoined();
        if (g_departRejoined) UE_LOGI("[CI-DRILL] client throwdepart: rejoined; it reports the seed of the prop's mirror");
    }
    return g_departRejoined;
}

namespace {
constexpr const char* kRejoinEidLine = "ci-throw host rejoin ";   // + "eid=<eid>"
constexpr const char* kRejoinedLine  = "ci-throw client rejoined ";   // + "rest=x,y,z"
uint32_t g_rjEid = 0;
uint64_t g_rjMs = 0;
ue_wrap::FVector g_rjLast{};
int      g_rjStill = 0;
}  // namespace

// The rejoined client: the host names the thrown prop; the mirror comes with the join's seed; once it stays put the client says
// where it is.
void DepartRejoinedTick() {
    if (!g_rjMs) g_rjMs = ::GetTickCount64();
    static uint64_t nextMs = 0;
    const uint64_t now = ::GetTickCount64();
    if (now < nextMs) return;
    nextMs = now + RD::kCheckEveryMs;
    if (now - g_rjMs > kRejoinBoundMs) {
        UE_LOGI("[CI-DRILL] client throwdepart NOT MEASURED (the seed of the prop never read within %llu s)", U(kRejoinBoundMs / 1000));
        Finish();
        return;
    }
    if (!g_rjEid) {
        std::string rest;
        int eid = 0;
        if (DC::SeenRestOf(kRejoinEidLine, &rest) && RD::IntAfter(rest, "eid", eid) && eid > 0) g_rjEid = static_cast<uint32_t>(eid);
        return;
    }
    void* actor = coop::element::LivePropActor(g_rjEid);
    FVector at{};
    if (!actor || !E::TryGetActorLocation(actor, at)) return;
    g_rjStill = std::fabs(at.X - g_rjLast.X) + std::fabs(at.Y - g_rjLast.Y) + std::fabs(at.Z - g_rjLast.Z) < 1.f ? g_rjStill + 1 : 0;
    g_rjLast = at;
    if (g_rjStill < 4) return;
    char say[96];
    std::snprintf(say, sizeof(say), "%srest=%d,%d,%d", kRejoinedLine, static_cast<int>(at.X), static_cast<int>(at.Y), static_cast<int>(at.Z));
    DC::Say(say);
    UE_LOGI("[CI-DRILL] client throwdepart MEASURED (the seed shows the prop at (%.0f,%.0f,%.0f))", static_cast<double>(at.X),
            static_cast<double>(at.Y), static_cast<double>(at.Z));
    Finish();
}

// ---- throwdepart: the host -----------------------------------------------------------------------------------------------

// Per tick while the throw flies. The take-back (the thrower fell silent, or left) is read off the elected rows' view and must
// follow the last admitted row by kThrowSilenceMs plus kDepartTakeSlackMs; the host copy's speed at the first sample
// kDepartSampleMs after it is set against the last row's. Passed once the copy rests and the rejoined client's seed agrees
// with its rest pose (a seed never read is NOT MEASURED, never a FAIL).
HostJudge DepartHostTick(const Track& host, uint32_t eid, char* why, size_t whyLen) {
    const auto& tv = coop::prop_syncer_host::dev::ThrowViewNow();
    const uint64_t unixMs = cen::UnixMs();
    if (!g_departTakeMs) {
        if (tv.slotLeft > g_departTakeBacks0 || tv.silence > g_departSilence0) {
            g_departTakeMs = unixMs;
            g_departRowSpeed = tv.lastTakeBackSpeed;
            g_departAfterRowMs = tv.lastTakeBackMs >= tv.lastRowMs ? tv.lastTakeBackMs - tv.lastRowMs : 0;
            UE_LOGI("[CI-DRILL] host throwdepart: the thrower %s; the host took the prop back %llu ms after the last admitted row, at the last row's %.0f cm/s",
                    tv.silence > g_departSilence0 ? "fell silent" : "left", U(g_departAfterRowMs), static_cast<double>(g_departRowSpeed));
            if (g_departAfterRowMs > coop::net::throw_bounds::kThrowSilenceMs + kDepartTakeSlackMs) {
                std::snprintf(why, whyLen, "the take-back came %llu ms after the last admitted row, past %llu ms",
                              U(g_departAfterRowMs), U(coop::net::throw_bounds::kThrowSilenceMs + kDepartTakeSlackMs));
                return HostJudge::Failed;
            }
            if (g_departRowSpeed <= 1.f) {
                std::snprintf(why, whyLen, "the take-back carried no velocity (%.1f cm/s)", static_cast<double>(g_departRowSpeed));
                return HostJudge::Failed;
            }
        }
        return HostJudge::Working;
    }
    if (!g_departSpeedJudged) {
        for (const Sample& s : host.samples) {
            if (s.t < g_departTakeMs + kDepartSampleMs || s.speed <= 0.f) continue;
            g_departSpeedJudged = true;
            const float lo = g_departRowSpeed * (1.f - kDepartSpeedPct), hi = g_departRowSpeed * (1.f + kDepartSpeedPct);
            UE_LOGI("[CI-DRILL] host throwdepart: the host copy flew at %.0f cm/s %llu ms after the take-back (the last row's %.0f, within %.0f percent); took-back=%llu speed=%.0f/%.0f",
                    static_cast<double>(s.speed), U(s.t - g_departTakeMs), static_cast<double>(g_departRowSpeed), static_cast<double>(kDepartSpeedPct * 100.f),
                    U(g_departAfterRowMs), static_cast<double>(s.speed), static_cast<double>(g_departRowSpeed));
            if (s.speed < lo || s.speed > hi) {
                std::snprintf(why, whyLen, "the host copy's speed %.0f cm/s after the take-back is not within %.0f percent of the last row's %.0f cm/s",
                              static_cast<double>(s.speed), static_cast<double>(kDepartSpeedPct * 100.f), static_cast<double>(g_departRowSpeed));
                return HostJudge::Failed;
            }
            break;
        }
        return HostJudge::Working;
    }
    if (!host.atRest) return HostJudge::Working;
    const uint64_t now = ::GetTickCount64();
    if (now >= g_departSayMs) {   // the rejoined client may be anywhere in its join: the line is said again every second
        g_departSayMs = now + kMarkEveryMs;
        char say[64];
        std::snprintf(say, sizeof(say), "%seid=%u", kRejoinEidLine, eid);
        DC::Say(say);
    }
    std::string rest;
    int at[3] = {};
    if (!DC::SeenRestOf(kRejoinedLine, &rest)) {
        static uint64_t restMs = 0;
        if (!restMs) restMs = now;
        if (now - restMs > kRejoinBoundMs) {
            UE_LOGI("[CI-DRILL] host throwdepart: the rejoined client's seed was NOT MEASURED (no line within %llu s)", U(kRejoinBoundMs / 1000));
            return HostJudge::NotMeasured;
        }
        return HostJudge::Working;
    }
    if (!IntsAfter(rest, "rest=", at, 3)) {
        std::snprintf(why, whyLen, "the rejoined client's rest line does not read (%s)", rest.c_str());
        return HostJudge::Failed;
    }
    const float gap = Dist3(host.last, FVector{static_cast<float>(at[0]), static_cast<float>(at[1]), static_cast<float>(at[2])});
    UE_LOGI("[CI-DRILL] host throwdepart: the rejoined client's seed is %.0f cm from the host's rest pose (bound %.0f)", static_cast<double>(gap),
            static_cast<double>(kRestAgreeCm));
    if (gap > kRestAgreeCm) {
        std::snprintf(why, whyLen, "the rejoined client's seed is %.0f cm from the host's rest pose, past %.0f cm", static_cast<double>(gap),
                      static_cast<double>(kRestAgreeCm));
        return HostJudge::Failed;
    }
    return HostJudge::Passed;
}

// ---- throwjoin: the host -------------------------------------------------------------------------------------------------

namespace {
constexpr const char* kJoinMarkLine = "ci-throw host joinmark ";   // + "n=<n> eid=<eid>"
Instants    g_jhInst;           // the host's set of the joiner's instants, and the joiner's end line when first seen
int         g_jhN = 0;
std::string g_jhEnd;
uint64_t    g_jhEndMs = 0;
int         g_jhReadyN = 0;          // the throw whose "was a second client in the world before the host copy rested" is latched
bool        g_jhReady = false;
}

// The go: a second client is connected (world-ready or not: either way it follows the rows). False while none is, until
// the wait runs out; then the arm runs as a plain throw and says so.
bool JoinGate(const coop::net::Session& s, int clientSlot, uint64_t waitedMs) {
    int joining = -1;
    for (int i = 1; i < coop::net::kMaxPeers; ++i) {
        if (i == clientSlot || !s.IsSlotReady(i)) continue;
        joining = i;
        if (s.IsSlotWorldReady(i)) UE_LOGI("[CI-DRILL] host throwjoin: the second client (slot %d) is already world-ready; it follows the whole flight, not a mid-flight join", i);
        else UE_LOGI("[CI-DRILL] host throwjoin: the second client (slot %d) is connected and not world-ready; the throw may fly through its join", i);
    }
    if (joining >= 0) return true;
    if (waitedMs > kJoinWaitMs) {
        UE_LOGI("[CI-DRILL] host throwjoin: no second client within %llu s -- the throw runs alone and the mirror gap is NOT MEASURED", U(kJoinWaitMs / 1000));
        return true;
    }
    return false;
}

// Says the thrown eid to a joiner every second while the throw is judged: it may connect at any moment.
void JoinHostSay(int n, uint32_t eid) {
    const uint64_t now = ::GetTickCount64();
    if (now < g_markMs) return;
    g_markMs = now + kMarkEveryMs;
    char say[64];
    std::snprintf(say, sizeof(say), "%sn=%d eid=%u", kJoinMarkLine, n, eid);
    DC::Say(say);
}

// The joiner's instants of throw n, part by part; the poll runs while the throw is judged, so no part is lost to the feed.
void JoinHostCollect(int n) {
    if (n != g_jhN) {   // a new throw: the last throw's set and end line are not this one's
        g_jhN = n;
        g_jhInst = Instants{};
        g_jhEnd.clear();
        g_jhEndMs = 0;
    }
    InstantsCollect(g_jhInst, "joiner", n);
}

// Latches, for throw n, whether a client other than the thrower was world-ready at some tick before the host copy rested.
// Called every tick the throw flies or is judged; once the copy rests the answer stands.
void JoinHostWatchReady(const coop::net::Session& s, int thrower, int n, bool hostAtRest) {
    if (n != g_jhReadyN) {
        g_jhReadyN = n;
        g_jhReady = false;
    }
    if (hostAtRest || g_jhReady) return;
    for (int i = 1; i < coop::net::kMaxPeers; ++i)
        if (i != thrower && s.IsSlotWorldReady(i)) g_jhReady = true;
}

// One line per throw: whether the joiner bound the prop (its end line arrived), whether it was in the world before the host
// copy rested, and the mirror gap. A throw that rested before the join says so; the next one measures the joiner as a mirror.
void JoinHostReport(int n, float gapCm) {
    char gap[24];
    if (gapCm >= 0.f) std::snprintf(gap, sizeof(gap), "%.0f", static_cast<double>(gapCm));
    else              std::snprintf(gap, sizeof(gap), "n/a");
    const bool ready = n == g_jhReadyN && g_jhReady;
    UE_LOGI("[CI-DRILL] host throwjoin n=%d joiner: bound=%d world-ready-before-rest=%d mirror-gap=%s%s", n, g_jhEnd.empty() ? 0 : 1,
            ready ? 1 : 0, gap, ready ? "" : " (rested before the join)");
}

// The joiner's end line of throw n, set against the host's trace: the mirror gap at its instants and the rest agreement. A
// joiner that sampled the prop only after the host's copy rested was not in flight: said so, and only the seeded rest pose is
// checked. `gapOut` is -1 with no instant matched.
HostJudge JoinHostJudge(const Track& host, int n, float& gapOut, char* why, size_t whyLen) {
    gapOut = -1.f;
    JoinHostCollect(n);
    if (g_jhEnd.empty()) {
        if (!DC::SeenRestOf(ThrowLine("joiner", "end", n, true), &g_jhEnd)) return HostJudge::Working;
        g_jhEndMs = ::GetTickCount64();
    }
    // The joiner's instants come before its end line, in a few parts; the judgement waits for every one of them.
    if (!InstantsComplete(g_jhInst)) {
        if (::GetTickCount64() - g_jhEndMs <= kInstantsBoundMs) return HostJudge::Working;
        std::snprintf(why, whyLen, "the joiner's instants never all arrived (%d/%d)", InstantsHeld(g_jhInst), g_jhInst.m);
        return HostJudge::Failed;
    }
    const std::string& rest = g_jhEnd;
    int at[3] = {};
    unsigned long long first = 0;
    if (const size_t f = rest.find("first="); f != std::string::npos) first = std::strtoull(rest.c_str() + f + 6, nullptr, 10);
    if (!IntsAfter(rest, "rest=", at, 3)) {
        std::snprintf(why, whyLen, "the joiner's end line does not read (%s)", rest.c_str());
        return HostJudge::Failed;
    }
    const Gap g = JudgeInstants(host, g_jhInst);
    const bool inFlight = host.atRest ? first < host.restMs : true;
    if (!inFlight || g.matched == 0)
        UE_LOGI("[CI-DRILL] throwjoin: rested before the join (%lld ms): the joiner first saw the prop %llu, the host copy rested at %llu; only the seeded rest pose is checked",
                static_cast<long long>(first) - static_cast<long long>(host.restMs), first, U(host.restMs));
    else gapOut = g.worst;
    const float restGap = Dist3(host.last, FVector{static_cast<float>(at[0]), static_cast<float>(at[1]), static_cast<float>(at[2])});
    UE_LOGI("[CI-DRILL] host throwjoin n=%d: mirror-gap=%.0f cm over %d of %d instants (bound %.0f, PROVISIONAL), the joiner's rest is %.0f cm from the host's (bound %.0f, PROVISIONAL)",
            n, static_cast<double>(g.worst), g.matched, g.said, static_cast<double>(kFlightGapPassCm), static_cast<double>(restGap), static_cast<double>(kRestAgreeCm));
    if (gapOut > kFlightGapPassCm) {
        std::snprintf(why, whyLen, "the joiner's copy was %.0f cm from the host's at k=%d, past %.0f cm", static_cast<double>(g.worst), g.worstK,
                      static_cast<double>(kFlightGapPassCm));
        return HostJudge::Failed;
    }
    if (restGap > kRestAgreeCm) {
        std::snprintf(why, whyLen, "the joiner's rest pose is %.0f cm from the host's, past %.0f cm", static_cast<double>(restGap),
                      static_cast<double>(kRestAgreeCm));
        return HostJudge::Failed;
    }
    return HostJudge::Passed;
}

// ---- throwjoin: the joiner -----------------------------------------------------------------------------------------------

bool JoinerRole() { return ArmOf() == Arm::ThrowJoin && Put2IsJoiner(); }

// The second client: for EVERY throw the host names it waits for the prop, traces its mirror from the moment it reads, and says
// where it was at fixed instants after its first sample and where it came to rest. It is done once the thrower's done line
// is seen and no newer throw is named.
void JoinerTick(const coop::net::Session& session) {
    if (g_jDone) return;
    const uint64_t now = cen::UnixMs();
    if (!g_jEid) {
        static uint64_t nextMs = 0;
        const uint64_t tick = ::GetTickCount64();
        if (tick < nextMs) return;
        nextMs = tick + RD::kCheckEveryMs;
        std::string rest;
        int n = 0, eid = 0;
        if (!DC::SeenRestOf(kJoinMarkLine, &rest) || !RD::IntAfter(rest, "n", n) || !RD::IntAfter(rest, "eid", eid) || eid <= 0) return;
        if (n <= g_jLastN) {   // every named throw is measured: the thrower's done line ends the arm
            if (g_jLastN > 0 && DC::SeenHas(kThrowDoneLine)) {
                UE_LOGI("[CI-DRILL] joiner throwjoin DONE (throws measured up to %d)", g_jLastN);
                g_jDone = true;
                Finish();
            }
            return;
        }
        void* actor = coop::element::LivePropActor(static_cast<uint32_t>(eid));
        if (!actor) return;   // the join's seed has not bound the prop yet
        g_jEid = static_cast<uint32_t>(eid);
        g_jN = n;
        g_jFirstMs = now;
        TrackBegin(g_jTrack, "joiner", "mirror");
        TrackSet(g_jTrack, actor);
        g_jEndAtMs = 0;
        UE_LOGI("[CI-DRILL] joiner throwjoin: the prop of throw %d (eid=%u) is bound here; tracing it", n, g_jEid);
        return;
    }
    TrackTick(g_jTrack, session.rttMsForSlot(0));
    if (!g_jEndAtMs) {
        if (g_jTrack.atRest) g_jEndAtMs = g_jTrack.restMs + kAfterRestMs;
        else if (g_jTrack.gone) g_jEndAtMs = g_jTrack.goneMs + kAfterRestMs;
    }
    if (!((g_jEndAtMs && now >= g_jEndAtMs) || now >= g_jFirstMs + kFlightBoundMs)) return;
    SayInstants(g_jTrack, g_jFirstMs, "joiner", g_jN);
    char say[160];
    std::snprintf(say, sizeof(say), "%sfirst=%llu rest=%d,%d,%d", ThrowLine("joiner", "end", g_jN, true).c_str(), U(g_jFirstMs),
                  static_cast<int>(g_jTrack.last.X), static_cast<int>(g_jTrack.last.Y), static_cast<int>(g_jTrack.last.Z));
    DC::Say(say);
    UE_LOGI("[CI-DRILL] joiner throwjoin MEASURED (throw %d, first sample t=%llu, rest (%.0f,%.0f,%.0f))", g_jN, U(g_jFirstMs),
            static_cast<double>(g_jTrack.last.X), static_cast<double>(g_jTrack.last.Y), static_cast<double>(g_jTrack.last.Z));
    g_jLastN = g_jN;   // the next throw the host names is traced afresh
    g_jEid = 0;
    g_jEndAtMs = 0;
    g_jTrack = Track{};
}

void ArmsReset() {
    g_p = P::Start;
    g_pMs = 0;
    g_base = Base{};
    g_placeGrab = "?";
    g_placeHand = "n/a";
    g_touchRowMs = g_touchGrabMs = 0;
    g_touchGrabbed = false;
    g_departRejoinRead = g_departRejoined = false;
    g_departTakeBacks0 = g_departSilence0 = 0;
    g_departTakeMs = g_departSayMs = g_departAfterRowMs = 0;
    g_departRowSpeed = 0.f;
    g_departSpeedJudged = false;
    g_jTrack = Track{};
    g_jEid = 0;
    g_jN = g_jLastN = 0;
    g_jFirstMs = g_jEndAtMs = g_markMs = 0;
    g_jDone = false;
    g_jhReadyN = 0;
    g_jhReady = false;
    g_rjEid = 0;
    g_rjMs = 0;
    g_rjLast = FVector{};
    g_rjStill = 0;
    g_jhInst = Instants{};
    g_jhN = 0;
    g_jhEnd.clear();
    g_jhEndMs = 0;
}

}  // namespace coop::dev::container_intent_drill::detail

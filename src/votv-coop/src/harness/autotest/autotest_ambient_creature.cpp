// harness/autotest/autotest_ambient_creature.cpp -- TEST-ONLY: one set of deer and walking trees across two peers.
//
// The client's four ambient spawner rows refuse (read from the gate's counters), then the host rolls a walking tree and a deer
// through the spawners' own bodies. The client must have one mirror of each, the tree's size and landing as the host's, a deer
// that dissolves for its own viewer when the host brings it near, and its mirror of a second deer ends when the host's does. The
// host judges both peers' counts and prints `ambient-creature: VERDICT`. The client reaches the host with the director.
// Gated by VOTVCOOP_RUN_AMBIENT_CREATURE; the folder is VOTVCOOP_CAN_DIR. The deer A/B reads the SpringArm's and the actor's
// rotation on both peers each frame while the host's deer walks, a client mirror's actor tick off for one window and on (as
// built) for the next. The host first moves its deer 7000 uu or more from both bodies, to the ground least like its spawn, so the slope under the host
// deer differs from the one the mirror spawned on. The mirror's lean in the tick-on window is a verdict term: it must be the host's
// within kTiltTolerance, and a guard term holds that the window could tell the two apart. Red arms: VOTVCOOP_AMBIENT_CREATURE_RED.

#include "harness/autotest.h"
#include "harness/autotest/meet.h"
#include "harness/autotest/peer_signal.h"

#include "coop/config/config.h"
#include "coop/config/config_registry.h"
#include "coop/creatures/ambient_creature_sync.h"
#include "coop/creatures/npc_sync.h"
#include "coop/element/registry.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/net_pump.h"
#include "coop/world/spawn_authority.h"
#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/game_thread.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/world_identity.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace harness::autotest {
namespace {

namespace AC = ue_wrap::ambient_creature;
namespace E = ue_wrap::engine;
namespace R = ue_wrap::reflection;
namespace OI = ue_wrap::object_index;
namespace PS = harness::autotest::peer_signal;
using PS::Await;
using PS::AwaitText;
using PS::Mark;

int g_fails = 0;

template <class Fn>
void OnGameThread(Fn&& fn) {
    ue_wrap::game_thread::RunAndWait([&fn](std::atomic<int>& d) { fn(); d.store(1); });
}

// A readiness wait on a state the game thread reads. A judged wait (`abort` false) ends in the caller's check.
template <class Pred>
bool WaitGT(const char* what, int seconds, Pred pred, bool abort = true) {
    return PS::WaitFor(what, seconds, [&] {
        bool ok = false;
        OnGameThread([&] { ok = pred(); });
        return ok;
    }, abort);
}

void Check(bool ok, const char* what) {
    UE_LOGI("ambient-creature: %hs %hs", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

// A peer's fails count as it left it in its done signal; text that is no count is a fail.
int ParseFails(const std::string& text) {
    char* end = nullptr;
    const long n = std::strtol(text.c_str(), &end, 10);
    return end != text.c_str() && n >= 0 ? static_cast<int>(n) : 1;
}

bool RedArm(const char* arm) {
    static const std::string s_arm = coop::config::ResolveEnum(coop::config_registry::rows::ambient_creature_red);
    return s_arm == arm;
}

void* FirstOf(const wchar_t* cls) {
    void* found = nullptr;
    if (void* c = OI::ClassByName(cls))
        OI::ForEachInstance(c, [](void* ctx, void* obj, int32_t) {
            if (R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) *static_cast<void**>(ctx) = obj;
        }, &found);
    return found;
}

// One run of a spawner's own function.
bool RunSpawner(const wchar_t* cls, const wchar_t* function, bool withDelta) {
    void* ticker = FirstOf(cls);
    void* fn = ticker ? R::FindDispatchFunctionCached(R::ClassOf(ticker), function) : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    if (withDelta) f.Set<float>(L"DeltaSeconds", 0.016f);
    return ue_wrap::Call(ticker, f);
}

// The deer's roll wants night hours: the clock's hour, written where the body reads it.
void MakeNight() {
    void* cycle = FirstOf(L"dayNightCycle_C");
    const int32_t off = cycle ? R::FindPropertyOffset(R::ClassOf(cycle), L"timeZ") : -1;
    if (off >= 0) *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(cycle) + off) = 22;
}

std::vector<void*> Live(bool deer) {
    std::vector<void*> v;
    if (deer) AC::ListDeer(v);
    else AC::ListTrees(v);
    return v;
}

// The creature a spawner's roll just added to `before`, or null.
void* Added(bool deer, const std::vector<void*>& before) {
    for (void* a : Live(deer))
        if (std::find(before.begin(), before.end(), a) == before.end()) return a;
    return nullptr;
}

// The host's roll of a deer is one in fifty by night, so the body runs until one stands; each run is a frame.
void* RollDeer() {
    std::vector<void*> had;
    OnGameThread([&] { had = Live(true); });
    void* deer = nullptr;
    for (int i = 0; i < 3000 && !deer; ++i) {
        OnGameThread([&] {
            MakeNight();
            RunSpawner(L"ticker_deerSpawner_C", L"ReceiveTick", true);
            deer = Added(true, had);
        });
    }
    return deer;
}

// Whether the npc lane has enrolled the actor: an actor it has not tracked answers kInvalidId, never 0.
bool Enrolled(uint32_t eid) { return eid != 0 && eid != coop::element::kInvalidId; }

void* Mirror(uint32_t eid) { return coop::element::LiveActorOfType(eid, coop::element::ElementType::Npc); }

// ---- the deer A/B: a mirror's actor tick on against off ----

struct Sample {
    double t = 0;          // seconds on the steady clock, one origin for both processes of this machine
    float  arm[3] = {};    // the SpringArm's world rotation: pitch, yaw, roll
    float  act[3] = {};    // the actor's
    float  at[3] = {};     // the actor's place (the host's own samples only)
};

double NowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool SampleDeer(void* deer, Sample& s) {
    if (!deer || !R::IsLive(deer)) return false;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(deer), L"SpringArm");
    void* arm = off >= 0 ? *reinterpret_cast<void**>(static_cast<uint8_t*>(deer) + off) : nullptr;
    ue_wrap::FRotator ar{}, cr{};
    ue_wrap::FVector at{};
    if (!arm || !E::TryGetComponentWorldRotation(arm, ar) || !E::TryGetActorRotation(deer, cr) || !E::TryGetActorLocation(deer, at))
        return false;
    s.t = NowSec();
    s.arm[0] = ar.Pitch, s.arm[1] = ar.Yaw, s.arm[2] = ar.Roll;
    s.act[0] = cr.Pitch, s.act[1] = cr.Yaw, s.act[2] = cr.Roll;
    s.at[0] = at.X, s.at[1] = at.Y, s.at[2] = at.Z;
    return true;
}

// Every frame for `seconds`: one read per game-thread turn.
template <class ActorOf>
std::vector<Sample> Window(ActorOf actorOf, double seconds) {
    std::vector<Sample> out;
    const double end = NowSec() + seconds;
    while (NowSec() < end) {
        OnGameThread([&] {
            Sample s;
            if (SampleDeer(actorOf(), s)) out.push_back(s);
        });
    }
    return out;
}

std::string Pack(const std::vector<Sample>& v) {
    std::string out;
    char b[160];
    for (const Sample& s : v) {
        std::snprintf(b, sizeof(b), "%.6f %.3f %.3f %.3f %.3f %.3f %.3f ", s.t, s.arm[0], s.arm[1], s.arm[2], s.act[0], s.act[1], s.act[2]);
        out += b;
    }
    return out;
}

std::vector<Sample> Unpack(const std::string& text) {
    std::vector<Sample> out;
    std::istringstream in(text);
    Sample s;
    while (in >> s.t >> s.arm[0] >> s.arm[1] >> s.arm[2] >> s.act[0] >> s.act[1] >> s.act[2]) out.push_back(s);
    return out;
}

float Wrap(float d) {
    while (d > 180.f) d -= 360.f;
    while (d < -180.f) d += 360.f;
    return d;
}

// UE's FRotator -> FQuat (Yaw about Z, Pitch about Y, Roll about X; degrees), as {x, y, z, w}.
struct Quat {
    double x, y, z, w;
};

Quat FromRot(const float* pyr) {
    const double k = 3.14159265358979323846 / 360.0;
    const double sp = std::sin(pyr[0] * k), cp = std::cos(pyr[0] * k), sy = std::sin(pyr[1] * k), cy = std::cos(pyr[1] * k),
                 sr = std::sin(pyr[2] * k), cr = std::cos(pyr[2] * k);
    return {cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy, cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy};
}

constexpr double kRad = 180.0 / 3.14159265358979323846;

struct Rel {
    double sumRel = 0, maxRel = 0, sumTilt = 0, maxTilt = 0;
    size_t n = 0;
};

// The arm's orientation in its own actor's frame: the geodesic angle, and how far its up axis leans off the actor's up axis.
Rel Relative(const std::vector<Sample>& v) {
    Rel r;
    for (const Sample& s : v) {
        const Quat a = FromRot(s.arm), c = FromRot(s.act);
        const double w = c.w * a.w + c.x * a.x + c.y * a.y + c.z * a.z;  // conj(act) * arm: scalar part, then x and y
        const double x = c.w * a.x - a.w * c.x - (c.y * a.z - c.z * a.y), y = c.w * a.y - a.w * c.y - (c.z * a.x - c.x * a.z);
        const double rel = 2.0 * std::acos((std::min)(1.0, std::fabs(w))) * kRad;
        const double tilt = std::acos((std::max)(-1.0, (std::min)(1.0, 1.0 - 2.0 * (x * x + y * y)))) * kRad;
        r.sumRel += rel, r.maxRel = (std::max)(r.maxRel, rel);
        r.sumTilt += tilt, r.maxTilt = (std::max)(r.maxTilt, tilt);
        ++r.n;
    }
    return r;
}

void PrintRel(const char* who, const char* tick, const std::vector<Sample>& v) {
    const Rel r = Relative(v);
    const double n = r.n ? static_cast<double>(r.n) : 1.0;
    UE_LOGI("deer-ab: %s rel mean=%.2f max=%.2f tilt mean=%.2f max=%.2f n=%zu tick=%s", who, r.sumRel / n, r.maxRel, r.sumTilt / n,
            r.maxTilt, r.n, tick);
}

// The yaw gap of each client sample to the host's actor as it stood kPoseLag earlier, interpolated between the host's two samples
// that bracket that moment, 50 ms apart at most.
constexpr double kPoseLag = 0.150;

bool HostYawAt(const std::vector<Sample>& host, double t, double& yaw) {
    for (size_t i = 0; i + 1 < host.size(); ++i) {
        const Sample &lo = host[i], &hi = host[i + 1];
        if (t < lo.t || t > hi.t || hi.t - lo.t > 0.05) continue;
        const double f = hi.t > lo.t ? (t - lo.t) / (hi.t - lo.t) : 0.0;
        yaw = lo.act[1] + Wrap(hi.act[1] - lo.act[1]) * f;
        return true;
    }
    return false;
}

void PrintYawLagged(const std::vector<Sample>& host, const std::vector<Sample>& client, const char* tick) {
    double sum = 0, worst = 0, moved = 0;
    size_t n = 0;
    for (const Sample& c : client) {
        double yaw = 0;
        if (!HostYawAt(host, c.t - kPoseLag, yaw)) continue;
        const double d = std::fabs(Wrap(static_cast<float>(c.act[1] - yaw)));
        sum += d, worst = (std::max)(worst, d);
        ++n;
    }
    for (const Sample& s : host) {
        const float dx = s.at[0] - host.front().at[0], dy = s.at[1] - host.front().at[1], dz = s.at[2] - host.front().at[2];
        moved = (std::max)(moved, static_cast<double>(std::sqrt(dx * dx + dy * dy + dz * dz)));
    }
    UE_LOGI("deer-ab: actor yaw lagged max=%.2f mean=%.2f n=%zu tick=%s moved=%.0f", worst, n ? sum / static_cast<double>(n) : -1.0, n,
            tick, moved);
}

// Measurement only, no verdict term: five client samples spread over the window, each beside the host's yaw kPoseLag earlier.
void PrintYawPairs(const std::vector<Sample>& host, const std::vector<Sample>& client) {
    constexpr size_t kPairs = 5;
    if (client.empty()) return;
    for (size_t k = 0; k < kPairs; ++k) {
        const Sample& c = client[k * (client.size() - 1) / (kPairs - 1)];
        const double t = c.t - client.front().t;
        double yaw = 0;
        if (HostYawAt(host, c.t - kPoseLag, yaw))
            UE_LOGI("deer-ab: yaw pair t=%.2f host=%.2f client=%.2f", t, yaw, static_cast<double>(c.act[1]));
        else
            UE_LOGI("deer-ab: yaw pair t=%.2f host=none client=%.2f", t, static_cast<double>(c.act[1]));
    }
}

// One reflected bitfield bool of an object: 1 or 0, or -1 when the object or the property does not resolve.
int ReadFlag(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask)) return -1;
    return (*(static_cast<const uint8_t*>(obj) + off) & mask) != 0 ? 1 : 0;
}

// What could turn the deer's body besides the pose stream: its Controller, whether the pawn takes the controller's yaw, and
// whether its movement turns it toward its velocity. Game thread.
void PrintDriveFlags(const char* who, void* deer) {
    if (!deer || !R::IsLive(deer)) {
        UE_LOGW("deer-ab: %s drive flags unreadable: no live deer", who);
        return;
    }
    void* const cls = R::ClassOf(deer);
    const int32_t ctrlOff = R::FindPropertyOffset(cls, L"Controller");
    const int32_t cmcOff = R::FindPropertyOffset(cls, L"CharacterMovement");
    void* ctrl = ctrlOff >= 0 ? *reinterpret_cast<void**>(static_cast<uint8_t*>(deer) + ctrlOff) : nullptr;
    void* cmc = cmcOff >= 0 ? *reinterpret_cast<void**>(static_cast<uint8_t*>(deer) + cmcOff) : nullptr;
    if (ctrl && !R::IsLive(ctrl)) ctrl = nullptr;
    if (cmc && !R::IsLive(cmc)) cmc = nullptr;
    const std::wstring ctrlName = ctrl ? R::ClassNameOf(ctrl) : std::wstring();
    UE_LOGI("deer-ab: %s controller=%ls useCtrlYaw=%d orientToMove=%d", who, ctrl ? ctrlName.c_str() : L"null",
            ReadFlag(deer, L"bUseControllerRotationYaw"), ReadFlag(cmc, L"bOrientRotationToMovement"));
}

constexpr double kAbSeconds = 5.0;
constexpr double kTiltTolerance = 5.0;   // degrees between the mirror's mean lean and the host deer's
constexpr double kDiscriminates = 10.0;  // degrees the host deer's new slope must differ from the mirror's spawn floor
// deer_C's own tick dissolves a deer under 5000 uu from camera 0 (deer.cpp Label_1395), and the client stands beside the host: the
// window's deer must stand past that reach of both bodies, or it ends on both peers before the mirror is judged.
constexpr float  kDeerOutOfReachCm = 7000.f;  // the dissolve radius 5000 plus 2000
constexpr float  kOutwardCm = 8000.f;         // how far from the host's body the candidate headings are cast
constexpr int    kHeadings = 8;               // candidates at 0, 45 .. 315 degrees
constexpr float  kArrivedCm = 100.f;          // how near its sent place the mirror must stand before the window opens

float Dist3(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The slope under a navmesh point, degrees off flat from a line trace down onto it; 90 when the trace reads nothing.
double TiltUnder(void* pawn, const ue_wrap::FVector& nav) {
    ue_wrap::trace::Hit hit;
    if (!ue_wrap::trace::LineHitStatDyn(pawn, {nav.X, nav.Y, nav.Z + 100.f}, {nav.X, nav.Y, nav.Z - 400.f}, &hit) || !hit.blocked)
        return 90.0;
    return std::acos((std::max)(-1.0, (std::min)(1.0, static_cast<double>(hit.normal.Z)))) * kRad;
}

// The place the host puts its deer: the navmesh point, among the eight headings kOutwardCm from the host's body, whose slope differs
// most from `preTilt` (the slope the mirror spawned on), out of reach: kDeerOutOfReachCm from the host's body and from the client's
// puppet; the capsule's half height up. A candidate whose trace reads nothing is no candidate.
bool FarPlaceUnlikeSpawn(void* deer, double preTilt, ue_wrap::FVector& out) {
    void* const pawn = coop::players::Registry::Get().Local();
    coop::RemotePlayer* const peer = coop::players::Registry::Get().Puppet(1);
    ue_wrap::FVector me{}, other{}, ext{}, org{}, at{};
    if (!pawn || !peer || !peer->TryGetLocation(other) || !E::TryGetActorLocation(pawn, me) || !E::TryGetActorLocation(deer, at))
        return false;
    bool found = false;
    double bestGap = 0;
    int bestHeading = 0;
    ue_wrap::FVector best{};
    for (int i = 0; i < kHeadings; ++i) {
        const double a = i * (360.0 / kHeadings) / kRad;
        const ue_wrap::FVector want{me.X + static_cast<float>(std::cos(a)) * kOutwardCm,
                                    me.Y + static_cast<float>(std::sin(a)) * kOutwardCm, me.Z};
        ue_wrap::FVector nav{};
        if (!E::ProjectToNav(pawn, want, {1500.f, 1500.f, 500.f}, &nav)) continue;
        if (Dist3(nav, me) < kDeerOutOfReachCm || Dist3(nav, other) < kDeerOutOfReachCm) continue;
        const double tilt = TiltUnder(pawn, nav);
        UE_LOGI("deer-ab: candidate heading=%d tilt=%.1f", i * (360 / kHeadings), tilt);
        if (tilt >= 90.0) continue;
        const double gap = std::fabs(tilt - preTilt);
        if (!found || gap > bestGap) found = true, bestGap = gap, bestHeading = i * (360 / kHeadings), best = nav;
    }
    if (!found) return false;
    UE_LOGI("deer-ab: chose heading=%d, %.1f off the spawn slope %.1f", bestHeading, bestGap, preTilt);
    const float lift = E::GetActorBounds(deer, true, org, ext) ? at.Z - (org.Z - ext.Z) : 100.f;
    out = {best.X, best.Y, best.Z + lift + 5.f};
    return true;
}

void HostAb(void* deer) {
    // The mirror's spawn floor is the slope the host deer also stands on, so a mirror frozen there would pass: the deer is put on
    // other ground first, and the window opens when the mirror has come there.
    std::string preText;
    if (!AwaitText("deer_pre", preText, 60)) return;
    const double preTilt = std::strtod(preText.c_str(), nullptr);
    ue_wrap::FVector place{};
    bool placed = false;
    OnGameThread([&] {
        PrintDriveFlags("host", deer);
        placed = FarPlaceUnlikeSpawn(deer, preTilt, place) && E::TeleportTo(deer, place, {});
    });
    Check(placed, "the host put its deer on ground unlike its spawn, out of both players' reach");
    if (!placed) return;
    char at[96];
    std::snprintf(at, sizeof(at), "%.1f %.1f %.1f", place.X, place.Y, place.Z);
    Mark("deer_tp", at);
    if (!Await("deer_tp_arrived", 60)) return;
    Mark("ab_a_go");
    const std::vector<Sample> off = Window([&] { return deer; }, kAbSeconds);
    if (!Await("ab_b_go", 60)) return;
    const std::vector<Sample> on = Window([&] { return deer; }, kAbSeconds);
    std::string a, b;
    if (!AwaitText("ab_a_samples", a, 60) || !AwaitText("ab_b_samples", b, 60)) return;
    PrintRel("host", "off", off);  // the host's window beside the mirror's phase of that name
    PrintRel("host", "on", on);
    const std::vector<Sample> clientOff = Unpack(a), clientOn = Unpack(b);
    PrintRel("client", "off", clientOff);
    PrintRel("client", "on", clientOn);
    PrintYawLagged(off, clientOff, "off");
    PrintYawLagged(on, clientOn, "on");
    PrintYawPairs(on, clientOn);

    // The mirror's tick runs on a floor its pose drive refreshes, so its body leans as the host deer's does. The host's
    // window of the same name is the one concurrent with the client's tick-on window.
    const Rel host = Relative(on), mirror = Relative(clientOn);
    const double hostTilt = host.n ? host.sumTilt / static_cast<double>(host.n) : -1.0;
    const double mirrorTilt = mirror.n ? mirror.sumTilt / static_cast<double>(mirror.n) : -1.0;
    UE_LOGI("deer-ab: tilt term host mean=%.2f mirror mean=%.2f tolerance=%.1f", hostTilt, mirrorTilt, kTiltTolerance);
    Check(host.n > 0 && mirror.n > 0 && std::fabs(mirrorTilt - hostTilt) <= kTiltTolerance,
          "the deer mirror's lean is the host deer's");

    // A window where the host deer leans as the spawn floor did cannot fail a mirror frozen on it: that is no pass.
    UE_LOGI("deer-ab: discriminates pre=%.2f host=%.2f", preTilt, hostTilt);
    Check(preTilt >= 0.0 && hostTilt >= 0.0 && std::fabs(hostTilt - preTilt) >= kDiscriminates, "the window discriminates");
}

// The mirror is built with its actor tick on: the off window takes it off, and the on window leaves it as built.
void ClientAb(uint32_t eid) {
    // One sample of the mirror on the floor it spawned on, before the host moves its deer.
    Sample pre;
    bool sampled = false;
    OnGameThread([&] {
        PrintDriveFlags("mirror", Mirror(eid));
        sampled = SampleDeer(Mirror(eid), pre);
    });
    Check(sampled, "the mirror's spawn lean was sampled");
    const Rel spawn = Relative({pre});
    Mark("deer_pre", sampled ? std::to_string(spawn.sumTilt) : std::string("-1"));
    std::string at;
    if (!AwaitText("deer_tp", at, 60)) return;
    const char* cur = at.c_str();
    char* end = nullptr;
    const float x = std::strtof(cur, &end);
    const float y = std::strtof(end, &end);
    const float z = std::strtof(end, &end);
    float away = -1.f;
    const bool arrived = WaitGT("the mirror came to the host's new place", 20, [&] {
        ue_wrap::FVector now{};
        void* const m = Mirror(eid);
        if (!m || !E::TryGetActorLocation(m, now)) return false;
        away = std::sqrt((now.X - x) * (now.X - x) + (now.Y - y) * (now.Y - y) + (now.Z - z) * (now.Z - z));
        return away <= kArrivedCm;
    }, false);
    UE_LOGI("deer-ab: mirror arrival away=%.0f", static_cast<double>(away));
    Check(arrived, "the mirror came to the host's new place");
    Mark("deer_tp_arrived");
    if (!Await("ab_a_go", 60)) return;
    OnGameThread([&] {
        if (void* m = Mirror(eid)) E::SetActorTickEnabled(m, false);
    });
    const std::vector<Sample> off = Window([&] { return Mirror(eid); }, kAbSeconds);
    OnGameThread([&] {
        if (void* m = Mirror(eid)) E::SetActorTickEnabled(m, true);
    });
    Mark("ab_b_go");
    const std::vector<Sample> on = Window([&] { return Mirror(eid); }, kAbSeconds);
    PrintRel("client", "off", off);
    PrintRel("client", "on", on);
    Mark("ab_a_samples", Pack(off));
    Mark("ab_b_samples", Pack(on));
}

// ---- the host ----

void HostBody() {
    if (!Await("client_ready", 480)) return;

    // The tree: one real roll, then its fall and landing.
    std::vector<void*> had;
    OnGameThread([&] {
        had = Live(false);
        RunSpawner(L"ticker_treeSpawner_C", L"ReceiveTick", true);
    });
    void* tree = nullptr;
    WaitGT("the spawner's own body rolled a tree", 15, [&] { return (tree = Added(false, had)) != nullptr; }, false);
    Check(tree != nullptr, "the spawner's own body rolled a tree");
    if (!tree) return;
    uint32_t eid = 0;
    float scale = 0.f;
    bool landed = false;
    WaitGT("the tree has an element", 15, [&] {
        eid = coop::npc_sync::GetNpcIdForActor(tree);
        AC::ReadTree(tree, scale, landed);
        return Enrolled(eid);
    }, false);
    Check(Enrolled(eid), "the tree has an element");
    size_t trees = 0;
    OnGameThread([&] { trees = Live(false).size(); });
    Mark("tree", std::to_string(eid) + " " + std::to_string(scale) + " " + std::to_string(trees));
    WaitGT("the host's tree landed", 240, [&] {
        if (!R::IsLive(tree)) return true;
        AC::ReadTree(tree, scale, landed);
        return landed;
    }, false);
    Check(landed, "the host's tree landed");
    OnGameThread([&] { Check(coop::ambient_creature_sync::Read().sent >= 1, "the host sent its tree's size and landing"); });
    Mark("tree_landed", std::to_string(trees));
    if (!Await("tree_done", 120)) return;

    // The deer.
    void* deer = RollDeer();
    Check(deer != nullptr, "the spawner's own body rolled a deer");
    if (!deer) return;
    uint32_t deerEid = 0;
    WaitGT("the deer has an element", 15, [&] { return (deerEid = coop::npc_sync::GetNpcIdForActor(deer)), Enrolled(deerEid); }, false);
    Check(Enrolled(deerEid), "the deer has an element");
    size_t deers = 0;
    OnGameThread([&] { deers = Live(true).size(); });
    Mark("deer", std::to_string(deerEid) + " " + std::to_string(deers));
    if (!Await("deer_mirror", 120)) return;
    HostAb(deer);

    // Beside both players, with the host's own dissolve off: only the client's can end it. The host places a deer, not a peer.
    ue_wrap::FVector me{};
    OnGameThread([&] {
        void* pawn = coop::players::Registry::Get().Local();
        E::SetActorTickEnabled(deer, false);
        if (pawn && E::TryGetActorLocation(pawn, me)) E::TeleportTo(deer, {me.X + 2000.f, me.Y, me.Z}, {});
    });
    Mark("deer_near");
    if (!Await("deer_near_done", 120)) return;
    bool alive = false;
    OnGameThread([&] { alive = R::IsLive(deer); });
    Check(alive, "the client's nearness left the host's deer standing");

    // The host's own end of a second deer reaches the client as its end.
    void* second = RollDeer();
    Check(second != nullptr, "a second deer stands");
    if (!second) return;
    uint32_t secondEid = 0;
    WaitGT("the second deer has an element", 15, [&] { return (secondEid = coop::npc_sync::GetNpcIdForActor(second)), Enrolled(secondEid); }, false);
    Mark("deer2", std::to_string(secondEid));
    if (!Await("deer2_seen", 120)) return;
    if (!RedArm("no_deer_end")) OnGameThread([&] { AC::Dissolve(second); });
    Mark("deer2_end");
    UE_LOGI("ambient-creature: the host's second deer %hs", RedArm("no_deer_end") ? "was left standing (red arm)" : "dissolved");
}

void RunHost() {
    HostBody();
    std::string clientText;
    const int clientFails = AwaitText("client_done", clientText, 60) ? ParseFails(clientText) : 1;  // no word is no pass
    if (g_fails == 0 && clientFails == 0) UE_LOGI("ambient-creature: VERDICT PASS host=%d client=%d", g_fails, clientFails);
    else UE_LOGW("ambient-creature: VERDICT FAIL host=%d client=%d", g_fails, clientFails);
    UE_LOGI("ambient-creature: DONE on host fails=%d", g_fails);
}

// ---- the client ----

bool WorldReady() {
    return WaitGT("this client's world", 240, [] {
        return coop::players::Registry::Get().Local() && coop::net_pump::HasAnnouncedWorldReady();
    });
}

bool HostPuppetUp() {
    if (!WorldReady()) return false;
    return WaitGT("the host's puppet", 60, [] {
        coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(0);
        return p && p->valid();
    });
}

// The four ambient rows refuse on this client: each spawner's body is run here, and its refusal count must rise. A tree this
// client rolled itself must not stand.
void CheckRows() {
    struct Spawner {
        const wchar_t* cls;
        const wchar_t* function;
        bool           withDelta;
        const char*    tag;
    };
    const Spawner spawners[] = {
        {L"ticker_deerSpawner_C", L"ReceiveTick", true, "deerSpawner.ReceiveTick"},
        {L"ticker_treeSpawner_C", L"ReceiveTick", true, "treeSpawner.ReceiveTick"},
        {L"ticker_hexahiveSpawner_C", L"ReceiveTick", true, "hexahiveSpawner.ReceiveTick"},
        {L"ticker_beehiveSpawner_C", L"Spawn", false, "beehiveSpawner.spawn"},
    };
    for (const Spawner& s : spawners) {
        uint64_t before = 0, after = 0;
        OnGameThread([&] {
            before = coop::spawn_authority::RefusalCount(s.tag);
            RunSpawner(s.cls, s.function, s.withDelta);
            after = coop::spawn_authority::RefusalCount(s.tag);
        });
        char what[96];
        std::snprintf(what, sizeof(what), "the client's %s body is refused (%llu -> %llu)", s.tag, static_cast<unsigned long long>(before),
                      static_cast<unsigned long long>(after));
        Check(after > before, what);
    }
    size_t own = 99;
    OnGameThread([&] { own = Live(false).size(); });
    Check(own == 0, "a tree this client rolled itself does not stand");
}

bool ClientBody() {
    if (!HostPuppetUp()) return false;
    if (!meet::WalkToHost("ambient-creature")) return false;
    CheckRows();
    Mark("client_ready");

    // The tree: its mirror, its size and landing.
    std::string body;
    if (!AwaitText("tree", body, 240)) return false;
    std::istringstream tin(body);
    uint32_t treeEid = 0;
    float hostScale = 0.f;
    size_t hostTrees = 0;
    tin >> treeEid >> hostScale >> hostTrees;
    void* tree = nullptr;
    WaitGT("the host's tree has its mirror here", 30, [&] { return (tree = Mirror(treeEid)) != nullptr; }, false);
    Check(tree != nullptr, "the host's tree has its mirror here");
    if (!tree) return false;
    float scale = 0.f;
    bool landed = false;
    WaitGT("the tree's size is the host's", 20, [&] {
        tree = Mirror(treeEid);
        return tree && AC::ReadTree(tree, scale, landed) && std::fabs(scale - hostScale) < 0.01f;
    }, false);
    UE_LOGI("ambient-creature: tree scale here=%.2f host=%.2f", scale, hostScale);
    Check(std::fabs(scale - hostScale) < 0.01f, "the tree's size is the host's");
    size_t trees = 0;
    OnGameThread([&] { trees = Live(false).size(); });
    Check(trees == hostTrees, "one mirror of the host's tree stands here");
    if (!AwaitText("tree_landed", body, 240)) return false;
    WaitGT("the landing is the host's", 20, [&] {
        tree = Mirror(treeEid);
        return tree && AC::ReadTree(tree, scale, landed) && landed;
    }, false);
    Check(landed, "the host's tree landed here too");
    OnGameThread([&] {
        trees = Live(false).size();
        Check(trees == hostTrees, "still one mirror of the host's tree");
        Check(coop::ambient_creature_sync::Read().applied >= 1, "the lane wrote the host's tree state onto the mirror");
    });
    Mark("tree_done");

    // The deer.
    if (!AwaitText("deer", body, 240)) return false;
    std::istringstream din(body);
    uint32_t deerEid = 0;
    size_t hostDeer = 0;
    din >> deerEid >> hostDeer;
    void* deer = nullptr;
    WaitGT("the host's deer has its mirror here", 30, [&] { return (deer = Mirror(deerEid)) != nullptr; }, false);
    Check(deer != nullptr, "the host's deer has its mirror here");
    if (!deer) return false;
    size_t deers = 0;
    OnGameThread([&] { deers = Live(true).size(); });
    Check(deers == hostDeer, "one mirror of the host's deer stands here");
    Mark("deer_mirror");
    ClientAb(deerEid);

    // The host brings it near: it dissolves for this viewer alone, in this same world.
    if (!Await("deer_near", 240)) return false;
    void* const world = ue_wrap::world_identity::CurrentWorld();
    bool gone = false;
    WaitGT("the near deer dissolved here", 30, [&] { return (gone = Mirror(deerEid) == nullptr); }, false);
    Check(gone && ue_wrap::world_identity::CurrentWorld() == world, "the near deer dissolved for this viewer, in the same world");
    Mark("deer_near_done");

    // The host's second deer ends, and so does this client's mirror of it.
    if (!AwaitText("deer2", body, 240)) return false;
    const uint32_t secondEid = static_cast<uint32_t>(std::strtoul(body.c_str(), nullptr, 10));
    void* second = nullptr;
    WaitGT("the second deer has its mirror here", 30, [&] { return (second = Mirror(secondEid)) != nullptr; }, false);
    Check(second != nullptr, "the second deer has its mirror here");
    Mark("deer2_seen");
    if (!Await("deer2_end", 240)) return false;
    bool secondGone = false;
    WaitGT("the host's second deer ended here too", 20, [&] { return (secondGone = Mirror(secondEid) == nullptr); }, false);
    Check(second && secondGone, "the host's ended deer ended here too");

    // After the host's later steps (a second deer rolled and ended), the dissolved deer is still without a mirror: a pose or
    // snapshot for its eid, while the host's deer stands, must not bring a second one in.
    bool stillGone = false;
    size_t deersLeft = 99;
    OnGameThread([&] {
        stillGone = Mirror(deerEid) == nullptr;
        deersLeft = Live(true).size();
    });
    Check(stillGone, "the dissolved deer's eid has no second mirror");
    Check(deersLeft == hostDeer - 1, "no deer mirror stands here but the host's others");
    return true;
}

void RunClient() {
    const bool ran = ClientBody();
    Mark("client_done", std::to_string(ran ? g_fails : g_fails + 1));   // a client that stopped short is no pass
    UE_LOGI("ambient-creature: DONE on client fails=%d", g_fails);
}

}  // namespace

DWORD WINAPI AmbientCreatureThread(LPVOID /*arg*/) {
    if (!PS::Begin("ambient-creature")) return 0;
    if (IsClientRole()) RunClient();
    else RunHost();
    return 0;
}

}  // namespace harness::autotest

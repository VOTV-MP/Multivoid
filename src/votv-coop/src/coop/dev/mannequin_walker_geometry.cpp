// coop/dev/mannequin_walker_geometry.cpp -- see coop/dev/mannequin_walker_geometry.h.

#include "coop/dev/mannequin_walker_geometry.h"

#include "coop/dev/director/director.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/player/shelter_spot.h"
#include "coop/session/net_pump.h"
#include "coop/session/rig_verdict.h"

#include "ue_wrap/actors/ambient_creature.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/actors/vitals.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

namespace coop::dev::mannequin_walker_drill {
namespace {

namespace AC  = ue_wrap::ambient_creature;
namespace DIR = coop::director;
namespace E   = ue_wrap::engine;
namespace PS  = harness::autotest::peer_signal;
namespace R   = ue_wrap::reflection;
namespace V   = ue_wrap::vitals;
using ue_wrap::FRotator;
using ue_wrap::FVector;

constexpr float kAimUpCm = 90.f;   // the mesh's middle, over the actor's origin

// The game's ticker_foodSleeper_C drops a player under 20 food into a ragdoll at random; a ragdoll turns no view and walks nowhere.
void KeepFed() {
    V::Write(V::Field::Food, 100.f);
    V::Write(V::Field::Sleep, 100.f);
}

}  // namespace

bool Invalid(const char* why) {
    UE_LOGW("mannequin-drill: INVALID -- %s", why);
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Invalid, "%s", why);
    return false;
}

namespace {
int g_fails = 0;
}  // namespace

int FailCount() { return g_fails; }

bool Check(bool ok, const char* step, const char* what) {
    UE_LOGI("mannequin-drill: step %s %s %s", step, ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
    return ok;
}

bool Fail(const char* step, const char* what) {
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Fail, "step %s: %s", step, what);
    return false;
}

bool AwaitClient(const char* mark, int seconds, std::string* text) {
    std::string word;
    if (!PS::AwaitText(mark, word, seconds)) return false;
    if (text) *text = word;
    if (word.rfind("fail", 0) != 0) return true;
    UE_LOGW("mannequin-drill: the client measured a failure: %s", word.c_str());
    coop::rig_verdict::Say(kTag, coop::rig_verdict::Outcome::Fail, "client %s", word.c_str());
    return false;
}

bool FailedToHost(const char* mark, const char* step, const char* what) {
    PS::Mark(mark, std::string("fail step ") + step + " " + what);
    return false;
}

std::vector<void*> Walkers() {
    std::vector<void*> v;
    AC::ListWalkers(v);
    return v;
}

std::string KeyOf(void* walker) { return Narrow(ue_wrap::prop::GetInteractableKeyString(walker)); }

void* ByKey(const std::string& key, int* count) {
    void* found = nullptr;
    int n = 0;
    for (void* w : Walkers())
        if (KeyOf(w) == key) {
            found = w;
            ++n;
        }
    if (count) *count = n;
    return found;
}

std::string Narrow(const std::wstring& key) {
    std::string s;
    s.reserve(key.size());
    for (const wchar_t c : key) s.push_back(static_cast<char>(c));
    return s;
}

std::wstring Widen(const std::string& key) { return std::wstring(key.begin(), key.end()); }

bool HostPuppetUp() {
    const bool world = WaitGT("this client's world", 240, [] {
        return coop::players::Registry::Get().Local() && coop::net_pump::HasAnnouncedWorldReady();
    });
    return world && WaitGT("the host's puppet", kPuppetBoundS, [] {
        coop::RemotePlayer* p = coop::players::Registry::Get().Puppet(coop::players::kPeerIdHost);
        return p && p->valid() && p->HasPose();
    });
}

bool AngerWalker(void* walker) {
    void* fn = walker ? R::FindDispatchFunctionCached(R::ClassOf(walker), L"fireDamage") : nullptr;
    if (!fn) return false;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<float>(L"damage", 1.f);
    return ue_wrap::Call(walker, f);
}

bool ReadAngry(void* walker, bool& angry) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!walker || !R::FindBoolProperty(R::ClassOf(walker), L"angry", off, mask) || off < 0) return false;
    angry = (*(static_cast<const uint8_t*>(walker) + off) & mask) != 0;
    return true;
}

float Dist3(const FVector& a, const FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void Aim(void* walker, bool toward) {
    KeepFed();
    void* pawn = coop::players::Registry::Get().Local();
    FVector at{};
    if (!pawn || !walker || !R::IsLive(walker) || !E::TryGetActorLocation(walker, at)) return;
    FRotator rot = DIR::LookAt(E::GetCameraLocation(), FVector{at.X, at.Y, at.Z + kAimUpCm});
    if (!toward) {
        rot.Yaw += 180.f;
        rot.Pitch = 0.f;
    }
    while (rot.Yaw > 180.f) rot.Yaw -= 360.f;
    while (rot.Yaw < -180.f) rot.Yaw += 360.f;
    E::SetControlRotation(E::GetController(pawn), rot);
}

float PlayerTo(void* actor) {
    void* me = coop::players::Registry::Get().Local();
    FVector a{}, b{};
    return me && actor && E::TryGetActorLocation(me, a) && E::TryGetActorLocation(actor, b) ? Dist3(a, b) : -1.f;
}

float ClientTo(void* actor) {
    coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(kClientSlot);
    void* puppet = rp && rp->valid() && rp->HasPose() ? rp->GetActor() : nullptr;
    FVector a{}, b{};
    return puppet && actor && E::TryGetActorLocation(puppet, a) && E::TryGetActorLocation(actor, b) ? Dist3(a, b) : -1.f;
}

bool BedAnchor(FVector& out) {
    bool bed = false;
    OnGameThread([&] {
        void* me = coop::players::Registry::Get().Local();
        if (!me) return;
        const coop::shelter_spot::Target t = coop::shelter_spot::Choose(me);
        if (t.kind != coop::shelter_spot::Kind::Bed) return;
        out = FVector{t.x, t.y, t.z};
        bed = true;
    });
    return bed;
}

bool EnterRange(void* walker, const char* who, bool (*hunted)(void* walker)) {
    FVector me{}, at{};
    float   cm = -1.f;
    bool    taken = false;
    OnGameThread([&] {
        void* pawn = coop::players::Registry::Get().Local();
        cm = pawn && walker && E::TryGetActorLocation(pawn, me) && E::TryGetActorLocation(walker, at) ? Dist3(me, at) : -1.f;
        taken = hunted(walker) && cm <= kInRangeCm;
    });
    char why[200];
    if (cm < 0.f) {
        std::snprintf(why, sizeof(why), "%s: the walker's place or its own did not read before stepping into range", who);
        return Invalid(why);
    }
    if (taken) return true;
    // The walk goes toward the walker itself, along the NavMesh (a point on the straight line from the walker is not one: from the
    // bed it lies in the house's walls), and it ends as soon as the walker hunts this player from inside its own range: the hunt
    // alone is not that (walker_target gives the host a walker with nobody in range, measured: a hunt taken at 20122 cm), so the
    // 3D distance, the Blueprint's own GetDistanceTo measure, is read beside it. Its reach is only the walk's own end.
    char what[96];
    std::snprintf(what, sizeof(what), "%s toward the walker", who);
    std::shared_ptr<DIR::BackgroundWalk> walk;
    OnGameThread([&] { walk = DIR::StartBackgroundWalk(at, kStepReachCm, kWalkDeadlineS); });
    bool hunting = false;
    PS::WaitFor(what, kWalkWaitS, [&] {
        OnGameThread([&] { hunting = hunted(walker) && PlayerTo(walker) <= kInRangeCm; });
        return hunting || walk->state.load() != 0;
    }, false);
    if (!hunting) {
        // The walk ended at its reach, inside the walker's range: its next read of the range takes this player.
        PS::WaitFor(what, kHuntGraceS, [&] {
            OnGameThread([&] { hunting = hunted(walker) && PlayerTo(walker) <= kInRangeCm; });
            return hunting;
        }, false);
    }
    DIR::CancelBackgroundWalk(*walk);
    PS::WaitFor("the walk lets go", kHuntGraceS, [&] { return walk->state.load() != 0; }, false);
    float now = -1.f;
    OnGameThread([&] { now = PlayerTo(walker); });
    UE_LOGI("mannequin-drill: %s %s the walker as its target, %.0f cm from it", who, hunting ? "is taken by" : "was never taken by", now);
    if (hunting) return true;
    std::snprintf(why, sizeof(why), "the walker never took %s as its target: %.0f cm from it after its walk", who, now);
    return Invalid(why);
}

namespace {

// -1 for a part that was not read, else its value.
int Tri(bool read, bool value) { return read ? (value ? 1 : 0) : -1; }

std::string DescribeView(void* controller, const AC::WalkerView& v) {
    void* pawn = coop::players::Registry::Get().Local();
    void* own = pawn ? E::GetController(pawn) : nullptr;
    const FVector   cam = E::GetCameraLocation();
    const FRotator  camRot = E::GetCameraRotation();
    const FRotator  bearing = DIR::LookAt(cam, v.origin);
    char b[640];
    std::snprintf(b, sizeof(b),
                  "ctrl=%p own=%d cam=(%.0f,%.0f,%.0f) camrot=(p%.0f,y%.0f) bearing=(p%.0f,y%.0f) origin=(%.0f,%.0f,%.0f) "
                  "extent=(%.0f,%.0f,%.0f) bounds=%d frustum=%d rendered=%d visible=%d hiddenInGame=%d actorHidden=%d",
                  controller, controller && controller == own ? 1 : 0, cam.X, cam.Y, cam.Z, camRot.Pitch, camRot.Yaw, bearing.Pitch,
                  bearing.Yaw, v.origin.X, v.origin.Y, v.origin.Z, v.extent.X, v.extent.Y, v.extent.Z, v.boundsRead ? 1 : 0,
                  Tri(v.frustumRead, v.inFrustum), Tri(v.renderedRead, v.rendered), Tri(v.visibleRead, v.visible),
                  Tri(v.hiddenInGameRead, v.hiddenInGame), Tri(v.actorHiddenRead, v.actorHidden));
    return b;
}

}  // namespace

bool InViewReportingParts(void* copy) {
    void* controller = AC::WalkerGazeController(copy);
    const AC::WalkerView v = AC::WalkerViewParts(copy, controller, true);
    if (v.InView()) return true;
    static auto nextLog = std::chrono::steady_clock::time_point{};
    const auto  now = std::chrono::steady_clock::now();
    if (now >= nextLog) {
        nextLog = now + std::chrono::seconds(kPartsLogEveryS);
        UE_LOGI("mannequin-drill: view parts: %s", DescribeView(controller, v).c_str());
    }
    return false;
}

std::string ViewPartsOf(void* copy) {
    void* controller = AC::WalkerGazeController(copy);
    return DescribeView(controller, AC::WalkerViewParts(copy, controller, true));
}

bool CloseUntilSeen(const std::function<void*()>& copyOf, bool (*seen)(void* copy), int boundS, float& lastCm, bool& lastSeen) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(boundS);
    bool       logged = false;
    lastCm = -1.f;
    lastSeen = false;
    while (std::chrono::steady_clock::now() < end) {
        FVector to{};
        // Aim and read for kAimPollMs: the render that makes a copy seen comes a frame after the aim.
        const auto pollEnd = std::chrono::steady_clock::now() + std::chrono::milliseconds(kAimPollMs);
        do {
            bool gone = false;
            OnGameThread([&] {
                void* c = copyOf();
                if (!c) {
                    gone = true;
                    return;
                }
                Aim(c, true);
                lastCm = PlayerTo(c);
                lastSeen = seen(c);
                E::TryGetActorLocation(c, to);
                if (!lastSeen && !logged) {
                    logged = true;
                    FVector me{};
                    void* pawn = coop::players::Registry::Get().Local();
                    if (pawn) E::TryGetActorLocation(pawn, me);
                    UE_LOGI("mannequin-drill: the copy is not in view: this client at (%.0f,%.0f,%.0f), the copy at (%.0f,%.0f,%.0f), "
                            "%.0f cm", me.X, me.Y, me.Z, to.X, to.Y, to.Z, lastCm);
                }
            });
            if (gone) return false;
            if (lastSeen) return true;
            ::Sleep(100);
        } while (std::chrono::steady_clock::now() < pollEnd);
        if (lastCm >= 0.f && lastCm < kApproachEndCm) return false;   // at its shoulder: no view of it is a measurement
        if (lastCm >= 0.f && lastCm <= kCloseReachCm) continue;       // close enough: only the read, on the walker's own approach
        // The leg: one walk toward the copy's place that runs on, kCloseLegS at most, until the copy is within kCloseReachCm or seen
        // with the walk's own facing (read every poll); then it lets go, and the next round aims and confirms. A copy past the
        // mesh's draw distance has frustum=1 and rendered=0 whatever the aim (an inference from two runs: not drawn at 116 m,
        // drawn at 69 m), so it shows only once the walk has closed in, and the walk is continuous for that.
        std::shared_ptr<DIR::BackgroundWalk> walk;
        OnGameThread([&] { walk = DIR::StartBackgroundWalk(to, kCloseReachCm, kWalkDeadlineS); });
        using Clock = std::chrono::steady_clock;
        const Clock::time_point legEnd = Clock::now() + std::chrono::seconds(kCloseLegS);
        bool                    sawIt = false, closeEnough = false, atShoulder = false;
        while (!sawIt && !closeEnough && !atShoulder && walk->state.load() == 0 && Clock::now() < legEnd && Clock::now() < end) {
            OnGameThread([&] {
                void* c = copyOf();
                if (!c) return;
                sawIt = seen(c);
                const float d = PlayerTo(c);
                if (d >= 0.f) lastCm = d;
                closeEnough = d >= 0.f && d <= kCloseReachCm;
                atShoulder = d >= 0.f && d < kApproachEndCm;
            });
            ::Sleep(100);
        }
        UE_LOGI("mannequin-drill: the close-in leg ended %s%s%s, %.0f cm from the copy", sawIt ? "with the copy in view" : "",
                closeEnough ? "within reach" : "", walk->state.load() != 0 ? "as the walk ended" : "", lastCm);
        DIR::CancelBackgroundWalk(*walk);
        if (atShoulder) {
            PS::WaitFor("the close-in walk lets go", kHuntGraceS, [&] { return walk->state.load() != 0; }, false);
            return false;   // at its shoulder: no view of it is a measurement
        }
        PS::WaitFor("the close-in walk lets go", kHuntGraceS, [&] { return walk->state.load() != 0; }, false);
    }
    return false;
}

bool WalkTo(const FVector& to, float reachCm, const char* what) {
    std::shared_ptr<DIR::BackgroundWalk> walk;
    OnGameThread([&] { walk = DIR::StartBackgroundWalk(to, reachCm, kWalkDeadlineS); });
    const bool done = PS::WaitFor(what, kWalkWaitS, [&] { return walk->state.load() != 0; }, false);
    if (!done) DIR::CancelBackgroundWalk(*walk);
    UE_LOGI("mannequin-drill: walk '%s' %s", what, done && walk->state.load() == 1 ? "arrived" : "did not arrive");
    return done && walk->state.load() == 1;
}

}  // namespace coop::dev::mannequin_walker_drill

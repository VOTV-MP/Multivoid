// coop/dev/remover_drill_layout.cpp -- see coop/dev/remover_drill_layout.h.

#include "coop/dev/remover_drill_layout.h"

#include "coop/comms/chat_log.h"
#include "coop/comms/chat_sync.h"
#include "coop/dev/spawn_npc.h"
#include "coop/element/registry.h"
#include "coop/player/players_registry.h"
#include "coop/player/puppet_drive.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/trace.h"
#include "ue_wrap/engine/engine.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace coop::dev::remover_drill::layout {
namespace {

namespace E  = ue_wrap::engine;
namespace R  = ue_wrap::reflection;
namespace EL = coop::element;
using ue_wrap::FVector;

constexpr const wchar_t* kSinglePath = L"/Game/objects/prop_pointSack_m.prop_pointSack_m_C";
constexpr const wchar_t* kMashPath   = L"/Game/objects/prop_pointSack_s.prop_pointSack_s_C";
constexpr const wchar_t* kSackPath   = L"/Game/objects/prop_pointSack.prop_pointSack_C";

// The layout about the client's puppet, in centimetres along its facing: the single and the mashed props side by side
// ahead of it, the creature straight ahead beyond them, a ring of kRing about it clear of the cone it faces.
constexpr float kSingleAheadCm = 120.f;
constexpr float kMashAheadCm   = 190.f;
constexpr float kPairSideCm    = 45.f;   // either side of the facing line, so neither prop stands in the other's way
constexpr float kRingConeDeg   = 30.f;   // the ring keeps this far off the facing line, each side
constexpr float kRingNearCm    = 260.f;
constexpr float kRingStepCm    = 30.f;
constexpr float kNpcAheadCm    = 330.f;  // the creature stands straight ahead, beyond the pair and inside the ring-free cone
constexpr float kNpcUpCm       = 60.f;   // the height of its middle above the floor, for the clear-segment test
constexpr float kTargetUpCm    = 30.f;   // the height of a sack's middle above the floor, for the same test
constexpr float kDropCm        = 40.f;   // above the floor, so a prop settles
constexpr float kSightSlackCm  = 25.f;
constexpr float kNearCm        = 400.f;  // props this near the eye are listed once per layout
constexpr int   kNearSaid      = 12;     // ...at most this many

Laid g_laid;

// What testing one spot found: whether it stands, the floor under it, and, when it does not, the branch and the blocker.
struct Spot {
    bool         ok = false;
    float        floorZ = 0.f;
    const char*  why = "";
    std::wstring blocker;
    float        blockedAt = 0.f, toPoint = 0.f;
};

// How far under the eye a spot's floor is looked for.
constexpr float kFloorSearchCm = 500.f;

// The floor under the column at (x, y): the first WorldStatic or WorldDynamic thing a trace meets going down from `fromZ`,
// every player body ignored. False when none is met, or when the column starts inside geometry.
bool FloorAt(void* puppet, float x, float y, float fromZ, float* floorZ, const char** why) {
    const uint8_t types[2] = {0, 1};
    void* ignore[coop::players::kMaxPeers];
    const int nIgnore = coop::players::Registry::Get().LiveBodies(ignore, coop::players::kMaxPeers);
    ue_wrap::trace::Hit hit;
    if (!ue_wrap::trace::LineHitForObjects(puppet, {x, y, fromZ}, {x, y, fromZ - kFloorSearchCm}, types, 2, ignore,
                                           nIgnore, &hit)) {
        *why = "the floor trace did not run";
        return false;
    }
    if (!hit.blocked) { *why = "no floor within 500 cm under the spot"; return false; }
    if (Dist({x, y, fromZ}, hit.point) < 5.f) { *why = "the spot's column starts inside geometry"; return false; }
    *floorZ = hit.point.Z;
    return true;
}

// Can a prop stand at (x, y), `up` cm above the floor found there, and be seen from `eye`? The floor is found by a trace
// of its own, never read off the puppet's bounds, and the segment from the eye to the prop's middle is the judge's: the four
// object types, every player body ignored, nothing met before the point.
Spot CheckSpot(void* puppet, const FVector& eye, float x, float y, float up) {
    Spot s;
    if (!FloorAt(puppet, x, y, eye.Z, &s.floorZ, &s.why)) return s;
    const FVector pt{x, y, s.floorZ + up};
    const uint8_t types[4] = {0, 1, 2, 3};
    void* ignore[coop::players::kMaxPeers];
    const int nIgnore = coop::players::Registry::Get().LiveBodies(ignore, coop::players::kMaxPeers);
    ue_wrap::trace::Hit hit;
    if (!ue_wrap::trace::LineHitForObjects(puppet, eye, pt, types, 4, ignore, nIgnore, &hit)) {
        s.why = "the segment trace did not run";
        return s;
    }
    s.toPoint = Dist(eye, pt);
    if (hit.blocked && Dist(eye, hit.point) < s.toPoint - kSightSlackCm) {
        s.why = "the segment is blocked";
        s.blockedAt = Dist(eye, hit.point);
        s.blocker = hit.actor ? R::ClassNameOf(hit.actor) : std::wstring(L"(no actor)");
        return s;
    }
    s.ok = true;
    return s;
}

// The yaw steps, in degrees, a layout is tried at round the puppet's facing: straight ahead first, then alternating
// sides until the whole circle is covered.
constexpr float kFanDeg[] = {0.f, 25.f, -25.f, 50.f, -50.f, 75.f, -75.f, 100.f, -100.f, 130.f, -130.f, 160.f, -160.f, 180.f};
constexpr int kFanCount = kFanSteps;
static_assert(sizeof(kFanDeg) / sizeof(kFanDeg[0]) == kFanSteps, "kFanSteps names the fan's size");

// Says what one tested direction found, one line a direction: a layout is attempted once, so the lines are bounded.
void SayDirection(float deg, const char* which, const Spot& s, const FVector& eye) {
    if (s.blocker.empty()) {
        UE_LOGI("[REMOVER-DRILL] host: direction %.0f deg, %s: %s (floor %.0f, eye %.0f)", deg, which,
                s.ok ? "clear" : s.why, s.floorZ, eye.Z);
        return;
    }
    UE_LOGI("[REMOVER-DRILL] host: direction %.0f deg, %s: %s -- %ls stands %.0f cm from the eye, the point is %.0f cm away "
            "(floor %.0f, eye %.0f)", deg, which, s.why, s.blocker.c_str(), s.blockedAt, s.toPoint, s.floorZ, eye.Z);
}

// Spawns the host's layout about the client's puppet, by its facing: the single and the mashed props side by side ahead
// of it (the single nearer), the creature straight ahead beyond them, and the ring about it clear of the cone it faces.
// The pair and the creature are laid only where each spot has a floor of its own and the segment from the puppet's eye
// to it is clear -- the trace the judge runs -- trying the facing and then a fan round it, so the drill measures the
// lane and not the map. The fan is walked from step `start` (a re-lay passes the step after the last), and `*chosen` is the
// step that was laid. Null on success; else the reason nothing was laid.
const char* SpawnLayout(coop::RemotePlayer& rp, const FVector& at, bool granted, int start, int* chosen) {
    void* puppet = rp.GetActor();
    void* singleCls = ue_wrap::asset_load::LoadObjectByPath(kSinglePath);
    void* mashCls = ue_wrap::asset_load::LoadObjectByPath(kMashPath);
    void* ringCls = ue_wrap::asset_load::LoadObjectByPath(kSackPath);
    if (!singleCls || !mashCls || !ringCls) return "a prop class of the layout did not load";
    FVector eye{};
    if (!rp.TryGetEyePosition(eye)) return "the sender's eye did not read on the host";
    FVector fwd0 = E::GetActorForwardVector(puppet);
    const float flat = std::sqrt(fwd0.X * fwd0.X + fwd0.Y * fwd0.Y);
    if (flat < 0.01f) fwd0 = {1.f, 0.f, 0.f};
    else fwd0 = {fwd0.X / flat, fwd0.Y / flat, 0.f};
    UE_LOGI("[REMOVER-DRILL] host: laying the layout -- puppet at (%.0f, %.0f, %.0f), its eye at (%.0f, %.0f, %.0f)", at.X,
            at.Y, at.Z, eye.X, eye.Y, eye.Z);

    FVector fwd{}, right{};
    Spot single, mash, npc;
    bool found = false;
    for (int fan = start; fan < kFanCount; ++fan) {
        const float deg = kFanDeg[fan];
        const float rad = deg * 0.0174532925f;
        fwd = {fwd0.X * std::cos(rad) - fwd0.Y * std::sin(rad), fwd0.X * std::sin(rad) + fwd0.Y * std::cos(rad), 0.f};
        right = {-fwd.Y, fwd.X, 0.f};
        const auto where = [&](float ahead, float side) {
            return FVector{at.X + fwd.X * ahead + right.X * side, at.Y + fwd.Y * ahead + right.Y * side, 0.f};
        };
        const FVector p1 = where(kSingleAheadCm, kPairSideCm);
        single = CheckSpot(puppet, eye, p1.X, p1.Y, kTargetUpCm);
        SayDirection(deg, "the single prop", single, eye);
        if (!single.ok) continue;
        if (granted) {
            const FVector p2 = where(kMashAheadCm, -kPairSideCm);
            mash = CheckSpot(puppet, eye, p2.X, p2.Y, kTargetUpCm);
            SayDirection(deg, "the mashed prop", mash, eye);
            if (!mash.ok) continue;
            const FVector p3 = where(kNpcAheadCm, 0.f);
            npc = CheckSpot(puppet, eye, p3.X, p3.Y, kNpcUpCm);
            SayDirection(deg, "the creature", npc, eye);
            if (!npc.ok) continue;
        }
        found = true;
        *chosen = fan;
        break;
    }
    if (!found) return "no direction round the puppet has a floor and a clear segment from its eye to the layout";

    // The props the host holds within 400 cm of the eye, once per layout: what the client's own trace can be compared with
    // when it meets something the host's did not.
    {
        std::vector<EL::Registry::ActorIdPair> rows;
        EL::Registry::Get().SnapshotActorsByType(EL::ElementType::Prop, rows);
        int said = 0;
        for (const auto& r : rows) {
            FVector p{};
            if (said >= kNearSaid || !r.actor || !R::IsLiveByIndex(r.actor, r.internalIdx) ||
                !E::TryGetActorLocation(r.actor, p) || Dist(p, eye) > kNearCm)
                continue;
            ++said;
            UE_LOGI("[REMOVER-DRILL] host: near the eye: %ls, element %u, at (%.0f, %.0f, %.0f), %.0f cm away",
                    R::ClassNameOf(r.actor).c_str(), static_cast<unsigned>(r.id), p.X, p.Y, p.Z, Dist(p, eye));
        }
    }
    // The ring sits on the floor under the puppet's own column, found the same way.
    float ringFloor = single.floorZ;
    const char* ringWhy = "";
    if (granted && !FloorAt(puppet, at.X, at.Y, eye.Z, &ringFloor, &ringWhy)) ringFloor = single.floorZ;
    const auto spawn = [&](void* cls, float ahead, float side, float floorZ, ue_wrap::CachedObjRef& into) {
        void* p = E::SpawnActor(cls, {at.X + fwd.X * ahead + right.X * side, at.Y + fwd.Y * ahead + right.Y * side,
                                      floorZ + kDropCm});
        into.Set(p);
        return p != nullptr;
    };
    if (!spawn(singleCls, kSingleAheadCm, kPairSideCm, single.floorZ, g_laid.single)) return "the single prop did not spawn";
    if (!granted) return nullptr;
    if (!spawn(mashCls, kMashAheadCm, -kPairSideCm, mash.floorZ, g_laid.mash)) return "the mashed prop did not spawn";
    for (int i = 0; i < kRing; ++i) {
        const float a = (kRingConeDeg + (360.f - 2.f * kRingConeDeg) * static_cast<float>(i) /
                                            static_cast<float>(kRing - 1)) * 0.0174532925f;
        const float r = kRingNearCm + kRingStepCm * static_cast<float>(i % 3);
        if (!spawn(ringCls, std::cos(a) * r, std::sin(a) * r, ringFloor, g_laid.ring[i])) return "a ring prop did not spawn";
    }
    void* npcActor = coop::dev::spawn_npc::SpawnAt(kNpcClass, {at.X + fwd.X * kNpcAheadCm, at.Y + fwd.Y * kNpcAheadCm,
                                                               npc.floorZ + kDropCm});
    g_laid.npc.Set(npcActor);
    return npcActor ? nullptr : "the creature did not spawn";
}

// The handshake that lets the client judge its own sight. The client says "remover-drill relay n cam x y z eid E at x y z"
// when its own camera's trace meets something before a target; the host answers "remover-drill laid n=<n>;" once it has
// laid the layout again in the next fan direction, or "remover-drill relay-failed;" when the fan has none left.
constexpr const char* kRelayPrefix = "remover-drill relay ";
int g_fanIdx = 0;       // the fan step the layout stands at
int g_relaysDone = 0;   // the relay numbers the host has answered

void Destroy(ue_wrap::CachedObjRef& ref) {
    if (void* a = ref.Get()) E::DestroyActor(a);
    ref.Reset();
}

}  // namespace

// Takes the next relay line the client said, if any: lays the layout again from the step after the last. The numbers in
// the line are the client's camera and the element its trace met with where it stands there, set here against the host's
// own eye and the host's position of that element: the measurement of whether the two peers agree.
Relayed HandleRelay(bool granted) {
    char text[256] = "";
    const std::string want = std::string(kRelayPrefix) + std::to_string(g_relaysDone + 1);
    coop::chat_log::ForEach([&](const coop::chat_log::Row& r) {
        if (r.slot == 1 && r.text.compare(0, want.size(), want) == 0 && (r.text.size() == want.size() || r.text[want.size()] == ' '))
            std::snprintf(text, sizeof(text), "%s", r.text.c_str());
    });
    if (!text[0]) return Relayed::None;
    ++g_relaysDone;
    float cx = 0, cy = 0, cz = 0, bx = 0, by = 0, bz = 0;
    unsigned eid = 0;
    int n = 0;
    const int got = sscanf_s(text, "remover-drill relay %d cam %f %f %f eid %u at %f %f %f", &n, &cx, &cy, &cz, &eid, &bx,
                                &by, &bz);
    coop::RemotePlayer& rp = coop::puppet_drive::Puppet(1);
    FVector eye{}, at{};
    const bool haveEye = rp.valid() && rp.HasPose() && rp.TryGetEyePosition(eye) && E::TryGetActorLocation(rp.GetActor(), at);
    if (got == 8 && haveEye) {
        FVector hostAt{};
        const EL::Element* el = EL::Registry::Get().Get(eid);
        void* a = el ? el->LiveActor() : nullptr;
        const bool held = a && E::TryGetActorLocation(a, hostAt);
        UE_LOGI("[REMOVER-DRILL] host: relay %d -- the client's camera is %.0f cm from the host's eye: (%.0f, %.0f, %.0f) "
                "against (%.0f, %.0f, %.0f)", n, Dist({cx, cy, cz}, eye), cx, cy, cz, eye.X, eye.Y, eye.Z);
        if (held)
            UE_LOGI("[REMOVER-DRILL] host: relay %d -- the blocker, element %u, stands %.0f cm from where the client holds it: "
                    "(%.0f, %.0f, %.0f) here against (%.0f, %.0f, %.0f) there", n, eid, Dist({bx, by, bz}, hostAt), hostAt.X,
                    hostAt.Y, hostAt.Z, bx, by, bz);
        else
            UE_LOGI("[REMOVER-DRILL] host: relay %d -- the blocker, element %u, is not a live element here; the client holds "
                    "it at (%.0f, %.0f, %.0f)", n, eid, bx, by, bz);
    }
    if (!haveEye) {
        coop::chat_sync::QueueSend("remover-drill relay-failed;");
        return Relayed::NoEye;
    }
    Destroy(g_laid.single);
    Destroy(g_laid.mash);
    Destroy(g_laid.npc);
    for (auto& r : g_laid.ring) Destroy(r);
    int chosen = g_fanIdx;
    if (const char* why = SpawnLayout(rp, at, granted, g_fanIdx + 1, &chosen)) {
        UE_LOGW("[REMOVER-DRILL] host: relay %d -- no further direction: %s", n, why);
        coop::chat_sync::QueueSend("remover-drill relay-failed;");
        return Relayed::Failed;
    }
    g_fanIdx = chosen;
    UE_LOGI("[REMOVER-DRILL] host: relay %d -- the layout is laid again at fan step %d", n, chosen);
    coop::chat_sync::QueueSend("remover-drill laid n=" + std::to_string(n) + ";");
    return Relayed::Laid;
}

Laid& Targets() { return g_laid; }

const char* Lay(coop::RemotePlayer& rp, const FVector& at, bool granted) {
    return SpawnLayout(rp, at, granted, 0, &g_fanIdx);
}

void Reset() {
    g_laid = Laid{};
    g_fanIdx = 0;
    g_relaysDone = 0;
}

}  // namespace coop::dev::remover_drill::layout

// harness/autotest/autotest_broomstroke_world.cpp -- see harness/autotest/broomstroke_world.h.

#include "broomstroke_world.h"   // co-located private header (src tree, not include/)

#include "coop/element/registry.h"          // an actor's element id, and an id's actor, on either role
#include "coop/player/hand_item.h"          // the hand axis no census may count
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"
#include "coop/session/teleport_client.h"   // the game's own teleport, which its anti-clip trace accepts
#include "ue_wrap/actors/broom.h"
#include "ue_wrap/actors/prop.h"
#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/sdk_profile.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_attach.h"   // SetActorRootPhysicsVelocity

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace harness::autotest::broom_world {

namespace GT = ue_wrap::game_thread;
namespace {

namespace R  = ue_wrap::reflection;
namespace E  = ue_wrap::engine;
namespace P  = ue_wrap::profile;
namespace UP = ue_wrap::prop;
namespace OI = ue_wrap::object_index;
namespace sg = ue_wrap::script_gate;

// The broom's cooked class, for a peer whose world has not loaded one yet.
constexpr const wchar_t* kBroomClassPath = L"/Game/objects/prop_broom.prop_broom_C";
// Where a striker stands from its subject, horizontally: inside the arm's reach from eye height.
constexpr float kStandCm = 70.f;
// The drill's own watch tag, apart from the lane's.
constexpr int kNotifyTag = 0x6D0;
// Below this a clump is not moving: the rest the trash channel's own close reads is 5 cm/s.
constexpr float kMovingCmS = 1.f;

const char* g_who = "?";
// Strokes that are over on this peer. Where the stroke runs, it is over when its body returns; on a
// client, whose lane refuses the body, no body runs and no post callback fires, so it is over at its
// entry.
std::atomic<uint32_t> g_strokesDone{0};
bool g_bodiesRunHere = true;
// The ids the next stroke of this peer's own broom freezes, and the clumps a freeze silenced, by
// the index each had; game thread.
std::vector<uint32_t> g_freezeIds;
bool g_freezeArmed = false;
struct Silenced { void* clump; int32_t idx; uint32_t eid; };
std::vector<Silenced> g_silenced;

void* LocalPlayer() {
    void* p = coop::players::Registry::Get().Local();
    return (p && R::IsLive(p)) ? p : nullptr;
}

void* Holding(void* player) {
    E::MainPlayerGrabState gs{};
    if (!player || !E::ReadMainPlayerGrabState(player, gs)) return nullptr;
    return gs.holdingActor;
}

uint32_t EidOf(void* actor) {
    const auto eid = coop::element::Registry::Get().EidForActor(actor);
    return eid == coop::element::kInvalidId ? 0u : static_cast<uint32_t>(eid);
}

ue_wrap::FRotator RotationTo(const ue_wrap::FVector& from, const ue_wrap::FVector& to) {
    const float dx = to.X - from.X, dy = to.Y - from.Y, dz = to.Z - from.Z;
    const float kRad2Deg = 180.f / 3.14159265358979323846f;
    ue_wrap::FRotator r{};
    r.Yaw   = std::atan2(dy, dx) * kRad2Deg;
    r.Pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy)) * kRad2Deg;
    return r;
}

// Every live instance, class default objects aside, of the classes `classPred` accepts, the
// instances of each class walked through the object index. Game thread.
template <class Pred, class Fn>
void ForEachInstanceWhere(Pred classPred, Fn fn) {
    std::vector<void*> classes;
    auto collect = [&](void* cls) { if (classPred(cls)) classes.push_back(cls); };
    OI::ForEachClass([](void* ctx, void* cls, void*) { (*static_cast<decltype(collect)*>(ctx))(cls); },
                     &collect);
    auto visit = [&](void* obj) {
        if (obj && R::IsLive(obj) && !R::NameStartsWith(R::NameOf(obj), L"Default__")) fn(obj);
    };
    for (void* cls : classes)
        OI::ForEachInstance(cls, [](void* ctx, void* obj, int32_t) { (*static_cast<decltype(visit)*>(ctx))(obj); },
                            &visit);
}

bool IsChipPileClass(void* cls) { return UP::WalksToBase(cls, UP::ChipPileClass()); }
bool IsClumpClass(void* cls) { return UP::WalksToBase(cls, UP::GarbageClumpClass()); }

float Within2(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

bool IsStroke(const sg::Call& call) {
    if (!ue_wrap::broom::IsBroom(call.object) || !call.function || !call.locals) return false;
    const int32_t off = R::FindParamOffset(call.function, P::name::BroomNotifyNameParam);
    if (off < 0) return false;
    R::FName name{};
    std::memcpy(&name, call.locals + off, sizeof(name));
    return R::NameEquals(name, P::name::BroomStrokeNotifyName);
}

void* ClumpOf(uint32_t eid) {
    coop::element::Element* e = coop::element::Registry::Get().Get(static_cast<coop::element::ElementId>(eid));
    void* a = e ? e->GetActor() : nullptr;
    return (a && R::IsLiveByIndex(a, e->GetInternalIdx()) && UP::IsGarbageClump(a)) ? a : nullptr;
}

// The stroke's push has set each clump's velocity and no physics step has run since, so zeroing it
// here is the push not happening to that clump; its hit notification is what the clump's own
// re-pile is bound to.
void FreezeStruck() {
    g_freezeArmed = false;
    const unsigned long tick = static_cast<unsigned long>(::GetTickCount());
    for (uint32_t eid : g_freezeIds) {
        void* clump = ClumpOf(eid);
        if (!clump) continue;
        const ue_wrap::FVector v = E::GetActorVelocity(clump);
        if (std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z) < kMovingCmS) continue;   // lying there before the stroke
        E::SetActorRootPhysicsVelocity(clump, ue_wrap::FVector{}, ue_wrap::FVector{});
        const bool silenced = E::SetActorRootNotifyRigidBodyCollision(clump, false);
        if (silenced) g_silenced.push_back(Silenced{clump, R::InternalIndexOf(clump), eid});
        UE_LOGI("broom_drill: FREEZE role=%s tick=%lu eid=%u was=(%.0f,%.0f,%.0f) silenced=%d", g_who, tick, eid,
                v.X, v.Y, v.Z, silenced ? 1 : 0);
    }
}

void OnNotifyDone(const sg::Call& call) {
    if (!g_bodiesRunHere || !IsStroke(call)) return;
    if (g_freezeArmed && call.object == Holding(LocalPlayer())) FreezeStruck();
    g_strokesDone.fetch_add(1);
}

sg::Verdict OnNotify(const sg::Call& call) {
    if (!IsStroke(call)) return sg::Verdict::Run;
    if (!g_bodiesRunHere) g_strokesDone.fetch_add(1);
    void* holder = ue_wrap::broom::ReadHolder(call.object);
    const bool own = call.object == Holding(LocalPlayer());
    ue_wrap::FRotator hr{};
    const bool yawRead = holder && E::TryGetActorRotation(holder, hr);
    const ue_wrap::FVector v = holder ? E::GetActorVelocity(holder) : ue_wrap::FVector{};
    const unsigned long tick = static_cast<unsigned long>(::GetTickCount());
    if (yawRead)
        UE_LOGI("broom_drill: NOTIFY role=%s tick=%lu own=%d holderYaw=%.1f holderVel=(%.0f,%.0f,%.0f)", g_who, tick,
                own ? 1 : 0, hr.Yaw, v.X, v.Y, v.Z);
    else   // no number: the judge reads the yaw as the stroke's heading
        UE_LOGI("broom_drill: NOTIFY role=%s tick=%lu own=%d holderYaw=(%s) holderVel=(%.0f,%.0f,%.0f)", g_who, tick,
                own ? 1 : 0, holder ? "unread" : "none", v.X, v.Y, v.Z);
    return sg::Verdict::Run;
}

}  // namespace

float Dist(const ue_wrap::FVector& a, const ue_wrap::FVector& b) { return std::sqrt(Within2(a, b)); }

bool EquipBroom(const char* who) {
    auto ok = std::make_shared<std::atomic<int>>(0);
    GT::RunAndWait([who, ok](std::atomic<int>& d) {
        void* player = LocalPlayer();
        if (!player) { UE_LOGW("broom_drill: %s EQUIP -- no local player", who); d.store(1); return; }
        if (!ue_wrap::broom::ResolveNames()) { UE_LOGW("broom_drill: %s EQUIP -- no broom names", who); d.store(1); return; }
        if (ue_wrap::broom::IsBroom(Holding(player))) { ok->store(1); d.store(1); return; }
        void* cls = R::FindClass(P::name::BroomClass);
        if (!cls) {
            ue_wrap::asset_load::LoadObjectByPath(kBroomClassPath);
            cls = R::FindClass(P::name::BroomClass);
        }
        if (!cls) { UE_LOGW("broom_drill: %s EQUIP -- the broom class did not load", who); d.store(1); return; }
        ue_wrap::FVector at{};
        if (!E::TryGetActorLocation(player, at)) {
            UE_LOGW("broom_drill: %s EQUIP -- the player's location could not be read", who); d.store(1); return; }
        at.X += 60.f;
        at.Z += 40.f;
        void* broom = E::BeginDeferredSpawn(cls, at, ue_wrap::FRotator{});
        if (!broom || !E::FinishDeferredSpawn(broom, at, ue_wrap::FRotator{})) {
            UE_LOGW("broom_drill: %s EQUIP -- the broom did not spawn", who);
            d.store(1); return;
        }
        // `Hold Object` takes a manual target, ends in the hotbar's equip and destroys the world copy.
        void* fn = R::FindDispatchFunction(R::ClassOf(player), L"Hold Object", nullptr);
        ue_wrap::ParamFrame f(fn);
        bool collected = false;
        if (f.valid()) {
            f.Set<bool>(L"useHold", false);   // take `manual`, not what the camera looks at
            f.Set<void*>(L"manual", broom);
            if (ue_wrap::Call(player, f)) collected = f.Get<bool>(L"collected");
        }
        void* h = Holding(player);
        const bool isBroom = ue_wrap::broom::IsBroom(h);
        UE_LOGI("broom_drill: %s EQUIP -- spawned %p, Hold Object collected=%d, hand=%p broom=%d",
                who, broom, collected ? 1 : 0, h, isBroom ? 1 : 0);
        ok->store(isBroom ? 1 : 0);
        d.store(1);
    });
    return ok->load() == 1;
}

bool AimAt(const ue_wrap::FVector& target, float bodyTurnDeg, const char* who, const char* phase) {
    const bool stood = GT::RunAndWait([target, bodyTurnDeg, who, phase](std::atomic<int>& d) {
        void* player = LocalPlayer();
        ue_wrap::FVector from{};
        if (!player || !E::TryGetActorLocation(player, from)) {
            UE_LOGW("broom_drill: %s %s AIM -- no stand: %s; the stroke is skipped", who, phase,
                    player ? "the player's location could not be read" : "no player");
            d.store(2);
            return;
        }
        float ax = from.X - target.X, ay = from.Y - target.Y;
        const float h = std::sqrt(ax * ax + ay * ay);
        if (h < 1.f) { ax = 1.f; ay = 0.f; } else { ax /= h; ay /= h; }
        const ue_wrap::FVector stand{target.X + ax * kStandCm, target.Y + ay * kStandCm, target.Z + 90.f};
        ue_wrap::FRotator facing = RotationTo(stand, target);
        facing.Pitch = 0.f;
        facing.Yaw += bodyTurnDeg;
        coop::teleport_client::ApplyLocally(coop::teleport_client::ApplyArgs{
            stand.X, stand.Y, stand.Z, facing.Pitch, facing.Yaw, facing.Roll});
        d.store(1);
    }) == 1;
    if (!stood) return false;
    ::Sleep(600);   // the body settles and the teleport's facing reaches the other peers
    LookAt(target, who, phase);
    return true;
}

void StandAt(const ue_wrap::FVector& at, float yawDeg) {
    GT::RunAndWait([at, yawDeg](std::atomic<int>& d) {
        coop::teleport_client::ApplyLocally(coop::teleport_client::ApplyArgs{at.X, at.Y, at.Z, 0.f, yawDeg, 0.f});
        d.store(1);
    });
    ::Sleep(600);
}

bool LocalBody(ue_wrap::FVector& at, float& yawDeg) {
    auto out = std::make_shared<std::pair<ue_wrap::FVector, float>>(ue_wrap::FVector{}, 0.f);
    const bool found = GT::RunAndWait([out](std::atomic<int>& d) {
        void* player = LocalPlayer();
        ue_wrap::FRotator rot{};
        const bool placed = player && E::TryGetActorLocation(player, out->first) && E::TryGetActorRotation(player, rot);
        out->second = rot.Yaw;
        d.store(placed ? 1 : 2);
    }) == 1;
    at = out->first;
    yawDeg = out->second;
    return found;
}

void LookAt(const ue_wrap::FVector& target, const char* who, const char* phase) {
    GT::RunAndWait([target](std::atomic<int>& d) {
        if (void* player = LocalPlayer())
            E::SetControlRotation(E::GetController(player), RotationTo(E::GetCameraLocation(), target));
        d.store(1);
    });
    ::Sleep(600);
    GT::RunAndWait([target, who, phase](std::atomic<int>& d) {
        void* player = LocalPlayer();
        ue_wrap::FVector start{}, end{};
        ue_wrap::FRotator rot{};
        const bool rotRead = player && E::TryGetActorRotation(player, rot);
        if (player && E::ReadMainPlayerArm(player, start, end))
            UE_LOGI("broom_drill: %s %s AIM segment (%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f), end %.0fcm from the "
                    "subject, heading yaw %.1f%s", who, phase, start.X, start.Y, start.Z, end.X, end.Y, end.Z,
                    Dist(end, target), rot.Yaw, rotRead ? "" : " (unread)");
        else
            UE_LOGW("broom_drill: %s %s AIM -- no segment to report", who, phase);
        d.store(1);
    });
}

bool WatchNotifies(const char* who, bool bodiesRunHere) {
    g_who = who;
    g_bodiesRunHere = bodiesRunHere;
    const bool ok = sg::WatchName(P::name::BroomStrokeNotifyFn, kNotifyTag, &OnNotify, &OnNotifyDone);
    if (!ok) UE_LOGW("broom_drill: %s -- the gate refused the notify watch; no NOTIFY rows", who);
    return ok;
}

int Swing(const char* who, const char* phase, int strokes, DWORD holdMs) {
    const uint32_t before = g_strokesDone.load();
    auto pressed = std::make_shared<std::atomic<int>>(0);
    GT::RunAndWait([who, phase, pressed](std::atomic<int>& d) {
        void* player = LocalPlayer();
        void* broom = Holding(player);
        const bool ok = ue_wrap::broom::IsBroom(broom) && ue_wrap::broom::PressUse(broom, player);
        UE_LOGI("broom_drill: %s %s PRESS tick=%lu broom=%p pressed=%d", who, phase,
                static_cast<unsigned long>(::GetTickCount()), broom, ok ? 1 : 0);
        pressed->store(ok ? 1 : 0);
        d.store(1);
    });
    const DWORD t0 = ::GetTickCount();
    while (pressed->load() == 1 && static_cast<int>(g_strokesDone.load() - before) < strokes &&
           ::GetTickCount() - t0 < holdMs)
        ::Sleep(10);
    const int seen = static_cast<int>(g_strokesDone.load() - before);
    // A posted task runs at an outermost dispatch, which a stroke's body never is, so the release
    // cannot null the holder between the stroke's loops. It could still land at a notify's own
    // dispatch, ahead of its body: a held button notifies once a second, a hold that ends at a
    // stroke's end is a second clear of the next, and phase M's timeout fell 800 ms before it in every
    // run.
    GT::RunAndWait([who, phase, seen](std::atomic<int>& d) {
        void* player = LocalPlayer();
        void* broom = Holding(player);
        const bool ok = ue_wrap::broom::IsBroom(broom) && ue_wrap::broom::ReleaseUse(broom, player);
        UE_LOGI("broom_drill: %s %s RELEASE tick=%lu notifies=%d released=%d", who, phase,
                static_cast<unsigned long>(::GetTickCount()), seen, ok ? 1 : 0);
        d.store(1);
    });
    return seen;
}

void FreezeNextStroke(const std::vector<uint32_t>& ids) {
    GT::RunAndWait([ids](std::atomic<int>& d) {
        g_freezeIds = ids;
        g_freezeArmed = true;
        d.store(1);
    });
}

// Game thread.
void ThawWhere(const char* who, uint32_t onlyEid) {
    for (auto it = g_silenced.begin(); it != g_silenced.end();) {
        if (onlyEid != 0 && it->eid != onlyEid) { ++it; continue; }
        const bool live = R::IsLiveByIndex(it->clump, it->idx);
        const bool restored = live && E::SetActorRootNotifyRigidBodyCollision(it->clump, true);
        UE_LOGI("broom_drill: %s THAW eid=%u live=%d restored=%d", who, it->eid, live ? 1 : 0, restored ? 1 : 0);
        it = g_silenced.erase(it);
    }
}

void Thaw(uint32_t eid, const char* who) {
    GT::RunAndWait([eid, who](std::atomic<int>& d) { ThawWhere(who, eid); d.store(1); });
}

void ThawAll(const char* who) {
    GT::RunAndWait([who](std::atomic<int>& d) { ThawWhere(who, 0); d.store(1); });
}

bool Knock(uint32_t eid, const ue_wrap::FVector& velocity, const char* who, const char* phase) {
    auto ok = std::make_shared<std::atomic<int>>(0);
    GT::RunAndWait([eid, velocity, who, phase, ok](std::atomic<int>& d) {
        void* clump = ClumpOf(eid);
        const bool set = clump && E::SetActorRootPhysicsVelocity(clump, velocity, ue_wrap::FVector{});
        ue_wrap::FVector at{};
        const unsigned long tick = static_cast<unsigned long>(::GetTickCount());
        if (clump && E::TryGetActorLocation(clump, at))
            UE_LOGI("broom_drill: %s %s KNOCK tick=%lu eid=%u at(%.1f,%.1f,%.1f) vel=(%.0f,%.0f,%.0f) set=%d", who,
                    phase, tick, eid, at.X, at.Y, at.Z, velocity.X, velocity.Y, velocity.Z, set ? 1 : 0);
        else   // no numbers: an unread place is no place
            UE_LOGI("broom_drill: %s %s KNOCK tick=%lu eid=%u at(unread) vel=(%.0f,%.0f,%.0f) set=%d", who, phase,
                    tick, eid, velocity.X, velocity.Y, velocity.Z, set ? 1 : 0);
        ok->store(set ? 1 : 0);
        d.store(1);
    });
    return ok->load() == 1;
}

bool PickChipPile(const char* who, const char* phase, ue_wrap::FVector& outPos, float aloneCm,
                  const ue_wrap::FVector* floorAt, float withinCm) {
    auto found = std::make_shared<std::pair<bool, ue_wrap::FVector>>(false, ue_wrap::FVector{});
    const bool bounded = floorAt != nullptr;
    const ue_wrap::FVector centre = bounded ? *floorAt : ue_wrap::FVector{};
    GT::RunAndWait([who, phase, aloneCm, bounded, centre, withinCm, found](std::atomic<int>& d) {
        void* player = LocalPlayer();
        ue_wrap::FVector me{};
        if (!player || !E::TryGetActorLocation(player, me)) {
            UE_LOGW("broom_drill: %s %s SUBJECT chip pile NOT found -- %s", who, phase,
                    player ? "the player's location could not be read" : "no player");
            d.store(1);
            return;
        }
        // Every pile counts as a neighbour, owned or not; the candidates are the owned ones, nearest first.
        struct Pile { void* obj; ue_wrap::FVector loc; float d2; };
        std::vector<Pile> piles;
        int unread = 0;
        ForEachInstanceWhere(&IsChipPileClass, [&](void* obj) {
            ue_wrap::FVector loc{};
            if (!E::TryGetActorLocation(obj, loc)) { ++unread; return; }   // no distance: no neighbour, no candidate
            piles.push_back(Pile{obj, loc, Within2(loc, me)});
        });
        std::sort(piles.begin(), piles.end(), [](const Pile& a, const Pile& b) { return a.d2 < b.d2; });
        const float alone2 = aloneCm * aloneCm;
        const float within2 = withinCm * withinCm;
        float best = -1.f;
        for (const Pile& p : piles) {
            if (EidOf(p.obj) == 0) continue;
            if (bounded && Within2(p.loc, centre) > within2) continue;
            bool alone = true;
            for (const Pile& other : piles)
                if (other.obj != p.obj && Within2(other.loc, p.loc) < alone2) { alone = false; break; }
            if (!alone) continue;
            found->first = true;
            found->second = p.loc;
            best = p.d2;
            break;
        }
        if (found->first)
            UE_LOGI("broom_drill: %s %s SUBJECT chip pile found at(%.0f,%.0f,%.0f), %.0fcm from the body, no other "
                    "pile within %.0fcm%s unreadAnywhere=%d", who, phase, found->second.X, found->second.Y,
                    found->second.Z, std::sqrt(best), aloneCm, bounded ? ", on the floor the drill stands on" : "",
                    unread);
        else   // no place: the origin is not one
            UE_LOGI("broom_drill: %s %s SUBJECT chip pile NOT found with no other pile within %.0fcm%s "
                    "unreadAnywhere=%d", who, phase, aloneCm, bounded ? ", on the floor the drill stands on" : "",
                    unread);
        d.store(1);
    });
    outPos = found->second;
    return found->first;
}

int SpawnPilesAt(const std::vector<ue_wrap::FVector>& at, const char* who, const char* tag) {
    auto made = std::make_shared<std::atomic<int>>(0);
    GT::RunAndWait([&at, who, tag, made](std::atomic<int>& d) {
        void* cls = UP::ChipPileClass();
        for (size_t i = 0; cls && i < at.size(); ++i) {
            void* pile = E::BeginDeferredSpawn(cls, at[i], ue_wrap::FRotator{});
            if (pile && E::FinishDeferredSpawn(pile, at[i], ue_wrap::FRotator{})) made->fetch_add(1);
        }
        UE_LOGI("broom_drill: %s %s spawned %d of %zu chip piles, the first at (%.0f,%.0f,%.0f)", who, tag,
                made->load(), at.size(), at.empty() ? 0.f : at[0].X, at.empty() ? 0.f : at[0].Y,
                at.empty() ? 0.f : at[0].Z);
        d.store(1);
    });
    return made->load();
}

int SpawnHeap(const ue_wrap::FVector& center, int count, float radiusCm, const char* who) {
    std::vector<ue_wrap::FVector> ring;
    for (int i = 0; i < count; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(count);
        ring.push_back(ue_wrap::FVector{center.X + std::cos(a) * radiusCm, center.Y + std::sin(a) * radiusCm, center.Z});
    }
    return SpawnPilesAt(ring, who, "HEAP");
}

int ChipPilesNear(const ue_wrap::FVector& center, float radiusCm, int& unreadAnywhere) {
    auto n = std::make_shared<std::pair<int, int>>(0, 0);   // named near, unread anywhere
    GT::RunAndWait([center, radiusCm, n](std::atomic<int>& d) {
        const float r2 = radiusCm * radiusCm;
        ForEachInstanceWhere(&IsChipPileClass, [&](void* obj) {
            if (EidOf(obj) == 0) return;
            ue_wrap::FVector loc{};
            if (!E::TryGetActorLocation(obj, loc)) ++n->second;
            else if (Within2(loc, center) <= r2) ++n->first;
        });
        d.store(1);
    });
    unreadAnywhere = n->second;
    return n->first;
}

void CensusPiles(const ue_wrap::FVector& center, float radiusCm, const char* who, const char* phase,
                 const char* tag, std::vector<uint32_t>* outIds) {
    GT::RunAndWait([center, radiusCm, who, phase, tag, outIds](std::atomic<int>& d) {
        const float r2 = radiusCm * radiusCm;
        int piles = 0, clumps = 0, unnamedClumps = 0, unread = 0;
        ForEachInstanceWhere([](void* cls) { return IsChipPileClass(cls) || IsClumpClass(cls); }, [&](void* obj) {
            ue_wrap::FVector loc{};
            if (!E::TryGetActorLocation(obj, loc)) { ++unread; return; }   // anywhere: counted, never placed
            if (Within2(loc, center) > r2) return;
            const bool clump = UP::IsGarbageClump(obj);
            const uint32_t eid = EidOf(obj);
            (clump ? clumps : piles)++;
            if (clump && eid == 0) ++unnamedClumps;
            if (outIds && !clump && eid != 0) outIds->push_back(eid);
        });
        UE_LOGI("broom_drill: CENSUS role=%s phase=%s tag=%s tick=%lu piles=%d clumps=%d unnamedClumps=%d "
                "unreadAnywhere=%d",
                who, phase, tag, static_cast<unsigned long>(::GetTickCount()), piles, clumps, unnamedClumps, unread);
        d.store(1);
    });
}

void TrackStep(std::vector<TrackState>& states, const ue_wrap::FVector& center, float radiusCm,
               UnnamedState& unnamed, const char* who, const char* phase) {
    GT::RunAndWait([&states, &unnamed, center, radiusCm, who, phase](std::atomic<int>& d) {
        const unsigned long tick = static_cast<unsigned long>(::GetTickCount());
        for (TrackState& s : states) {
            coop::element::Element* e = coop::element::Registry::Get().Get(
                static_cast<coop::element::ElementId>(s.eid));
            void* a = e ? e->GetActor() : nullptr;
            const bool live = a && R::IsLiveByIndex(a, e->GetInternalIdx());
            if (live && s.unreadable.Is(a)) continue;   // its one unread row is out; this actor is not read again
            const int form = !live ? 0 : (UP::IsGarbageClump(a) ? 2 : 1);
            ue_wrap::FVector got{};
            const bool read = live && E::TryGetActorLocation(a, got);
            if (live && !read) s.unreadable.Set(a);
            // Gone keeps the last place this tracker read, if it read one.
            const bool known = read || (!live && s.known);
            const ue_wrap::FVector pos = read ? got : s.pos;
            if (form == s.form && known == s.known && (!known || Dist(pos, s.pos) <= 5.f)) continue;
            s.form = form;
            s.known = known;
            s.pos = pos;
            const char* formName = form == 2 ? "clump" : (form == 1 ? "pile" : "gone");
            if (known)
                UE_LOGI("broom_drill: TRACK role=%s phase=%s tick=%lu eid=%u form=%s at(%.1f,%.1f,%.1f)", who, phase,
                        tick, s.eid, formName, pos.X, pos.Y, pos.Z);
            else
                UE_LOGI("broom_drill: TRACK role=%s phase=%s tick=%lu eid=%u form=%s at(unread)", who, phase, tick,
                        s.eid, formName);
        }
        const float r2 = radiusCm * radiusCm;
        auto& latched = unnamed.unreadable;
        latched.erase(std::remove_if(latched.begin(), latched.end(),
                                     [](const ue_wrap::CachedObjRef& u) { return !u.Get(); }),
                      latched.end());
        int inRadius = 0, unread = 0;
        ForEachInstanceWhere(&IsClumpClass, [&](void* obj) {
            if (EidOf(obj) != 0) return;
            for (const ue_wrap::CachedObjRef& u : latched)
                if (u.Is(obj)) { ++unread; return; }
            ue_wrap::FVector loc{};
            if (!E::TryGetActorLocation(obj, loc)) {
                ++unread;
                latched.emplace_back();
                latched.back().Set(obj);
                return;
            }
            if (Within2(loc, center) <= r2) ++inRadius;
        });
        if (inRadius != unnamed.last || unread != unnamed.unreadLast) {
            unnamed.last = inRadius;
            unnamed.unreadLast = unread;
            UE_LOGI("broom_drill: UNNAMED role=%s phase=%s tick=%lu clumps=%d unreadAnywhere=%d", who, phase, tick,
                    inRadius, unread);
        }
        d.store(1);
    });
}

void TrackFinal(const std::vector<TrackState>& states, const char* who, const char* phase) {
    GT::RunAndWait([&states, who, phase](std::atomic<int>& d) {
        const unsigned long tick = static_cast<unsigned long>(::GetTickCount());
        for (const TrackState& s : states) {
            coop::element::Element* e = coop::element::Registry::Get().Get(
                static_cast<coop::element::ElementId>(s.eid));
            void* a = e ? e->GetActor() : nullptr;
            const bool live = a && R::IsLiveByIndex(a, e->GetInternalIdx());
            ue_wrap::FVector got{};
            const bool read = live && !s.unreadable.Is(a) && E::TryGetActorLocation(a, got);
            const char* formName = !live ? "gone" : (UP::IsGarbageClump(a) ? "clump" : "pile");
            if (read || (!live && s.known)) {
                const ue_wrap::FVector pos = read ? got : s.pos;   // gone: the last place the tracker read
                UE_LOGI("broom_drill: FINAL role=%s phase=%s tick=%lu eid=%u form=%s at(%.1f,%.1f,%.1f)", who, phase,
                        tick, s.eid, formName, pos.X, pos.Y, pos.Z);
            } else {
                UE_LOGI("broom_drill: FINAL role=%s phase=%s tick=%lu eid=%u form=%s at(unread)", who, phase, tick,
                        s.eid, formName);
            }
        }
        d.store(1);
    });
}

bool StrikerBody(bool striking, uint8_t strikerSlot, ue_wrap::FVector& at) {
    auto out = std::make_shared<ue_wrap::FVector>();
    const bool placed = GT::RunAndWait([striking, strikerSlot, out](std::atomic<int>& d) {
        void* body = nullptr;
        if (striking) {
            body = LocalPlayer();
        } else if (coop::RemotePlayer* rp = coop::players::Registry::Get().Puppet(strikerSlot);
                   rp && rp->valid()) {
            body = rp->GetActor();
        }
        d.store(body && E::TryGetActorLocation(body, *out) ? 1 : 2);
    }) == 1;
    at = *out;
    return placed;
}

int CountTrashNear(const ue_wrap::FVector& at, float radiusCm, int& unreadAnywhere) {
    const float r2 = radiusCm * radiusCm;
    int n = 0;
    unreadAnywhere = 0;
    ForEachInstanceWhere(&UP::IsClassDescendantOfProp, [&](void* obj) {
        if (coop::hand_item::IsHandAxisActor(obj)) return;   // a held broom, or its mirror, is no trash
        ue_wrap::FVector loc{};
        if (!E::TryGetActorLocation(obj, loc)) ++unreadAnywhere;
        else if (Within2(loc, at) <= r2) ++n;
    });
    return n;
}

bool PickDispenser(const std::shared_ptr<Dispenser>& out, const char* who, int& trashOut, int& unreadOut) {
    auto trash = std::make_shared<std::pair<int, int>>(0, 0);   // trash near, unread anywhere
    const bool ok = GT::RunAndWait([out, who, trash](std::atomic<int>& d) {
        struct Found { std::wstring key; void* obj; ue_wrap::FVector loc; float d2; };
        std::vector<Found> piles;
        const ue_wrap::FVector anchor{P::name::kKPPSpawnX, P::name::kKPPSpawnY, P::name::kKPPSpawnZ};
        ForEachInstanceWhere([](void* cls) { return UP::WalksToBase(cls, UP::TrashBitsPileClass()); },
                             [&](void* obj) {
            std::wstring k = UP::GetInteractableKeyString(obj);
            if (k.empty() || k == L"None") return;
            ue_wrap::FVector loc{};
            if (!E::TryGetActorLocation(obj, loc)) { ++trash->second; return; }   // no distance to sort by
            piles.push_back(Found{std::move(k), obj, loc, Within2(loc, anchor)});
        });
        std::sort(piles.begin(), piles.end(), [](const Found& x, const Found& y) {
            return x.d2 != y.d2 ? x.d2 < y.d2 : x.key < y.key;
        });
        if (piles.size() > 20) piles.resize(20);
        std::sort(piles.begin(), piles.end(), [](const Found& x, const Found& y) { return x.key < y.key; });
        if (piles.empty()) {
            UE_LOGW("broom_drill: %s C -- no keyed dispenser pile unreadAnywhere=%d", who, trash->second);
            d.store(2);
            return;
        }
        out->key = piles[0].key;
        out->pile = piles[0].obj;
        out->idx = R::InternalIndexOf(out->pile);
        out->pos = piles[0].loc;   // read in the census above
        int trashUnread = 0;
        trash->first = CountTrashNear(out->pos, 300.f, trashUnread);
        trash->second += trashUnread;
        UE_LOGI("broom_drill: %s C SUBJECT key='%ls' at(%.0f,%.0f,%.0f) trashNear=%d unreadAnywhere=%d", who,
                out->key.c_str(), out->pos.X, out->pos.Y, out->pos.Z, trash->first, trash->second);
        d.store(1);
    }) == 1;
    trashOut = trash->first;
    unreadOut = trash->second;
    return ok;
}

bool DispenserAlive(const std::shared_ptr<Dispenser>& d) {
    auto alive = std::make_shared<std::atomic<int>>(0);
    GT::RunAndWait([d, alive](std::atomic<int>& done) {
        alive->store(R::IsLiveByIndex(d->pile, d->idx) ? 1 : 0);
        done.store(1);
    });
    return alive->load() == 1;
}

void CensusProps(const ue_wrap::FVector& center, float radiusCm, const char* who, const char* phase,
                 const char* tag) {
    GT::RunAndWait([center, radiusCm, who, phase, tag](std::atomic<int>& d) {
        const float r2 = radiusCm * radiusCm;
        const unsigned long tick = static_cast<unsigned long>(::GetTickCount());
        int n = 0, unread = 0;
        ForEachInstanceWhere(&UP::IsClassDescendantOfProp, [&](void* obj) {
            if (coop::hand_item::IsHandAxisActor(obj)) return;
            const uint32_t eid = EidOf(obj);
            if (eid == 0) return;
            ue_wrap::FVector loc{};
            if (!E::TryGetActorLocation(obj, loc)) { ++unread; return; }
            if (Within2(loc, center) > r2) return;
            ++n;
            UE_LOGI("broom_drill: PROP role=%s phase=%s tag=%s tick=%lu eid=%u at(%.1f,%.1f,%.1f)", who, phase,
                    tag, tick, eid, loc.X, loc.Y, loc.Z);
        });
        UE_LOGI("broom_drill: CENSUS role=%s phase=%s tag=%s tick=%lu props=%d unreadAnywhere=%d", who, phase, tag,
                tick, n, unread);
        d.store(1);
    });
}

bool PickProp(const ue_wrap::FVector& center, float radiusCm, const char* who, const char* phase,
              ue_wrap::FVector& outPos) {
    auto found = std::make_shared<std::pair<bool, ue_wrap::FVector>>(false, ue_wrap::FVector{});
    GT::RunAndWait([center, radiusCm, who, phase, found](std::atomic<int>& d) {
        float best = radiusCm * radiusCm;
        int unread = 0;
        ForEachInstanceWhere(&UP::IsClassDescendantOfProp, [&](void* obj) {
            if (EidOf(obj) == 0 || coop::hand_item::IsHandAxisActor(obj) || UP::IsStatic(obj) || UP::IsFrozen(obj))
                return;
            ue_wrap::FVector loc{};
            if (!E::TryGetActorLocation(obj, loc)) { ++unread; return; }   // no distance: no candidate
            const float d2 = Within2(loc, center);
            if (d2 < best) { best = d2; found->first = true; found->second = loc; }
        });
        if (found->first)
            UE_LOGI("broom_drill: %s %s SUBJECT prop found at(%.0f,%.0f,%.0f) unreadAnywhere=%d", who, phase,
                    found->second.X, found->second.Y, found->second.Z, unread);
        else   // no place: the origin is not one
            UE_LOGI("broom_drill: %s %s SUBJECT prop NOT found unreadAnywhere=%d", who, phase, unread);
        d.store(1);
    });
    outPos = found->second;
    return found->first;
}

}  // namespace harness::autotest::broom_world

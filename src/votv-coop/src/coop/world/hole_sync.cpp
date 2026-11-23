// coop/world/hole_sync.cpp -- see coop/world/hole_sync.h.
//
// 0.9.0n. event_bottomHoleController_C's box, while its `room` is unset: the room (holeBottom_C) at a fixed place, the
// device (holeDevice_C, static), the patrol controller (kavotiaPatrolController_C, `eventObject` itself); the save's
// `holegrate` replaced by a static prop_C named holegrate_0, the save's `holefence` freed and pushed; four
// prop_rozitalBeacon_C traced down around the device. Its end destroys the device, the patrol and the beacons.

#include "coop/world/hole_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/player_hit.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/engine/engine_pawn.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_instances.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>

namespace coop::hole_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

constexpr const wchar_t* kRoom = L"holeBottom_C";
constexpr const wchar_t* kController = L"event_bottomHoleController_C";
UE_FACT_FN(kRowBox, "hole_sync", L"event_bottomHoleController_C",
           L"BndEvt__event_bottomHoleController_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowSense, "hole_sync", L"kavotia_C", L"sensePawns", Declared);
UE_FACT_FN(kRowAddDamage, "hole_sync", L"mainPlayer_C", L"addDamage", Declared);
UE_FACT_FN(kRowBeginDeferredSpawn, "hole_sync", L"GameplayStatics", L"BeginDeferredActorSpawnFromClass", Declared);
UE_FACT_FN(kRowLineTrace, "hole_sync", L"KismetSystemLibrary", L"LineTraceSingleForObjects", Declared);
constexpr int kTagBox = 0x484F4C58;     // 'HOLX'
constexpr int kTagSense = 0x484F4C53;   // 'HOLS'
constexpr int kTagDamage = 0x484F4C44;  // 'HOLD'
constexpr const wchar_t* kGuard = L"kavotia_C";
constexpr const wchar_t* kShot = L"kavotiaShot_C";
constexpr float kSenseRange = 5000.f, kVisitRange = 500.f;
constexpr uint64_t kRoomMs = 5000, kLiveStaleMs = 15000;

std::atomic<N::Session*> g_session{nullptr};
bool g_box = false, g_sense = false, g_damage = false, g_hooked = false, g_traceHooked = false, g_traceArmed = false;
uint64_t g_nextArm = 0;
uint32_t g_cleared = 0, g_hits = 0;
void* g_igetis = nullptr;     // HOST: the Igetis whose montage's start is watched, and its `anim` last seen
bool g_igetisAnim = false;
std::vector<void*> g_shots;   // HOST: shots made this tick, read on the next (their transform is set at their finish)
constexpr float kBodyRadius = 70.f, kShotRange = 50000.f;
ue_wrap::FRotator g_shotRot{};   // CLIENT: the next shot's rotation, which its 119 brings before its place
uint32_t g_handed = 0, g_visits = 0, g_shotsSent = 0, g_shotsMade = 0, g_refused = 0;
uint64_t g_nextRoom = 0;
uint64_t g_connectedAt = 0, g_liveAt = 0;   // CLIENT: this session's first connected tick; the host's last 118

bool IsClient() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    return s && s->role() == N::Role::Client;
}

// CLIENT: the controller's box opens the Hole on the host alone.
sg::Verdict OnBoxPre(const sg::Call& call) {
    return call.tag == kTagBox && IsClient() ? sg::Verdict::Cancel : sg::Verdict::Run;
}

void* Field(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return nullptr;
    void* v = nullptr;
    std::memcpy(&v, static_cast<const uint8_t*>(obj) + off, sizeof(v));
    return v && R::IsLive(v) ? v : nullptr;
}

float Dist2(const ue_wrap::FVector& a, const ue_wrap::FVector& b) {
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return dx * dx + dy * dy + dz * dz;
}

void Send(uint32_t cue, float x, float y, float z) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected() || s->role() != N::Role::Host) return;
    N::EventCuePayload p{};
    p.cueId = cue;
    p.x = x;
    p.y = y;
    p.z = z;
    s->SendReliable(N::ReliableKind::EventCue, &p, sizeof(p));
}

// HOST: a guard's sense found nobody (it skips a puppet). While its Igetis is not visited, a client's body within 5 m of
// the Igetis visits it, as the host's player does; otherwise the guard is handed the nearest client's body in range.
void OnSensePost(const sg::Call& call) {
    if (call.tag != kTagSense || IsClient() || !call.object) return;
    static int32_t s_off = -2;
    if (s_off == -2) s_off = R::FindParamOffset(call.function, L"OutputPin");
    uint8_t* const out = s_off >= 0 ? sg::OutParamPtr(call, s_off) : nullptr;
    if (!out) return;
    void* target = nullptr;
    std::memcpy(&target, out, sizeof(target));
    if (target) return;
    void* const igetis = Field(Field(call.object, L"patrolController"), L"igetis");
    int32_t vOff = -1;
    uint8_t vMask = 0;
    if (!igetis || !R::FindBoolProperty(R::ClassOf(igetis), L"visited", vOff, vMask) || vOff < 0) return;
    uint8_t& visited = static_cast<uint8_t*>(igetis)[vOff];
    if (visited & vMask) return;
    ue_wrap::FVector guardAt{}, igetisAt{};
    if (!ue_wrap::engine::TryGetActorLocation(call.object, guardAt) ||
        !ue_wrap::engine::TryGetActorLocation(igetis, igetisAt))
        return;
    auto& reg = coop::players::Registry::Get();
    void* best = nullptr;
    float bestD = kSenseRange * kSenseRange;
    for (uint8_t slot = 1; slot < N::kMaxPeers; ++slot) {
        coop::RemotePlayer* const rp = reg.Puppet(slot);
        ue_wrap::FVector p{};
        if (!rp || !rp->valid() || !rp->HasPose() || !rp->GetActor() || !rp->TryGetLocation(p)) continue;
        if (Dist2(p, igetisAt) <= kVisitRange * kVisitRange) {
            visited = static_cast<uint8_t>(visited | vMask);
            ++g_visits;
            UE_LOGI("hole: slot %u's player stands by the Igetis -- the guards are calmed, as the host's player calms them",
                    static_cast<unsigned>(slot));
            return;
        }
        const float d = Dist2(p, guardAt);
        if (d < bestD) {
            bestD = d;
            best = rp->GetActor();
        }
    }
    if (!best) return;
    std::memcpy(out, &best, sizeof(best));
    if (++g_handed == 1 || g_handed % 50 == 0)
        UE_LOGI("hole: a guard is handed a client's body as its target (%u so far)", g_handed);
}

// HOST: a guard's trace to its target, which a client's body blocks by being there, reads clear while it targets one.
void OnLineTracePost(void* /*context*/, void* source, void* /*result*/) {
    if (!source || IsClient() || R::ClassNameOf(source) != kGuard) return;
    void* const target = Field(source, L"lookingAt");
    const uint8_t slot = target ? coop::players::Registry::Get().PeerIdOfActor(target) : 0;
    if (slot == 0 || slot >= N::kMaxPeers) return;
    if (void* r = ue_wrap::ufunction_hook::CurrentResult()) {
        *static_cast<bool*>(r) = false;
        if (++g_cleared == 1 || g_cleared % 100 == 0)
            UE_LOGI("hole: a guard's sight of a client's body reads clear (%u so far)", g_cleared);
    }
}

// HOST: a shot traces 500 m from its place along its forward and hits what blocks it, which a client's body does not:
// the client whose body the line crosses first takes the shot's point damage on its own machine.
void DeliverCrossing(void* shot, const ue_wrap::FVector& a, const ue_wrap::FRotator& rot) {
    const float pitch = rot.Pitch * 0.0174532925f, yaw = rot.Yaw * 0.0174532925f;
    const ue_wrap::FVector dir{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
    auto& reg = coop::players::Registry::Get();
    uint8_t victim = 0;
    float bestT = kShotRange + 1.f;
    for (uint8_t slot = 1; slot < N::kMaxPeers; ++slot) {
        coop::RemotePlayer* const rp = reg.Puppet(slot);
        ue_wrap::FVector p{};
        if (!rp || !rp->valid() || !rp->HasPose() || !rp->GetActor() || !rp->TryGetLocation(p)) continue;
        float t = (p.X - a.X) * dir.X + (p.Y - a.Y) * dir.Y + (p.Z - a.Z) * dir.Z;
        if (t < 0.f || t > kShotRange) continue;
        const ue_wrap::FVector c{a.X + dir.X * t, a.Y + dir.Y * t, a.Z + dir.Z * t};
        if (Dist2(c, p) <= kBodyRadius * kBodyRadius && t < bestT) {
            bestT = t;
            victim = slot;
        }
    }
    if (!victim) return;
    const int32_t off = R::FindPropertyOffset(R::ClassOf(shot), L"pointDamage");
    float damage = 0.f;
    if (off >= 0) std::memcpy(&damage, static_cast<const uint8_t*>(shot) + off, sizeof(damage));
    if (damage <= 0.f) return;
    N::PlayerHitPayload hit{};
    hit.verb = N::kPlayerHitVerbAddDamage;
    hit.damage = damage;
    hit.v0[0] = dir.X;
    hit.v0[1] = dir.Y;
    hit.v0[2] = dir.Z;
    if (coop::player_hit::Deliver(victim, hit) && (++g_hits == 1 || g_hits % 25 == 0))
        UE_LOGI("hole: a guard's shot crosses slot %u's body -- its %.0f damage delivered (%u so far)",
                static_cast<unsigned>(victim), damage, g_hits);
}

// HOST: a guard fired; the shot goes to the clients, rotation first.
void OnBeginDeferred(void* /*context*/, void* srcObj, void* spawned) {
    if (IsClient() || !spawned || !srcObj || R::ClassNameOf(srcObj) != kGuard ||
        R::ClassNameOf(spawned) != kShot)
        return;
    g_shots.push_back(spawned);
}

// HOST: the Igetis's `anim` turns true as its montage starts (the walk to the device, the capsule's take and drop).
void WatchIgetis() {
    void* one[1];
    void* const ig = ue_wrap::world_instances::Find(L"igetis_C", one, 1) > 0 && one[0] && R::IsLive(one[0]) ? one[0]
                                                                                                      : nullptr;
    int32_t off = -1;
    uint8_t mask = 0;
    if (!ig || !R::FindBoolProperty(R::ClassOf(ig), L"anim", off, mask) || off < 0) {
        g_igetis = nullptr;
        return;
    }
    const bool anim = (static_cast<const uint8_t*>(ig)[off] & mask) != 0;
    if (ig == g_igetis && anim && !g_igetisAnim) {
        ue_wrap::FVector at{};
        ue_wrap::engine::TryGetActorLocation(ig, at);
        Send(kCueIgetisPlay, at.X, at.Y, at.Z);
        UE_LOGI("hole: the host's Igetis plays its montage -- sent");
    }
    g_igetis = ig;
    g_igetisAnim = anim;
}

// CLIENT: the copy plays the host's montage; its notifies run the copy's beams, and its throw is refused (the
// capsule is the host's, on the prop lanes).
void PlayIgetis() {
    void* one[1];
    void* const ig = ue_wrap::world_instances::Find(L"igetis_C", one, 1) > 0 ? one[0] : nullptr;
    void* mesh = ig && R::IsLive(ig) ? Field(ig, L"igetis1_animHole7") : nullptr;
    static void* s_montage = nullptr;
    static void* s_play = nullptr;
    static void* s_getAnim = nullptr;
    if (!s_montage) s_montage = R::FindObject(L"igetis1_animHole7_Skeleton_Montage", L"AnimMontage");
    if (!s_play)
        if (void* ai = R::FindClass(L"AnimInstance")) s_play = R::FindFunction(ai, L"Montage_Play");
    if (!s_getAnim)
        if (void* sk = R::FindClass(L"SkeletalMeshComponent")) s_getAnim = R::FindFunction(sk, L"GetAnimInstance");
    void* anim = nullptr;
    if (mesh && s_getAnim) {
        ue_wrap::ParamFrame g(s_getAnim);
        if (g.valid() && ue_wrap::Call(mesh, g)) anim = g.Get<void*>(L"ReturnValue");
    }
    bool played = false;
    if (anim && s_montage && s_play) {
        ue_wrap::ParamFrame f(s_play);
        if (f.valid()) {
            f.Set<void*>(L"MontageToPlay", s_montage);
            f.Set<float>(L"InPlayRate", 1.0f);
            played = ue_wrap::Call(anim, f);
        }
    }
    UE_LOGI("hole: the host's Igetis plays its montage, and this copy plays it (mesh=%d anim=%d montage=%d played=%d)",
            mesh ? 1 : 0, anim ? 1 : 0, s_montage ? 1 : 0, played ? 1 : 0);
}

// HOST: the shots made since the last tick, at their place now.
void SendShots() {
    std::vector<void*> shots;
    shots.swap(g_shots);
    for (void* shot : shots) {
        ue_wrap::FVector at{};
        ue_wrap::FRotator rot{};
        if (!R::IsLive(shot) || !ue_wrap::engine::TryGetActorLocation(shot, at) ||
            !ue_wrap::engine::TryGetActorRotation(shot, rot))
            continue;
        Send(kCueShotRot, rot.Pitch, rot.Yaw, rot.Roll);
        Send(kCueShotAt, at.X, at.Y, at.Z);
        DeliverCrossing(shot, at, rot);
        if (++g_shotsSent == 1 || g_shotsSent % 50 == 0) UE_LOGI("hole: a guard's shot is sent (%u so far)", g_shotsSent);
    }
}

// CLIENT: a Rozital's shot here is a copy for its beam; the host's hit on this player is delivered.
sg::Verdict OnDamagePre(const sg::Call& call) {
    if (call.tag != kTagDamage || !IsClient() || !call.callerObject) return sg::Verdict::Run;
    int hops = 0;
    for (void* cls = R::ClassOf(call.callerObject); cls && hops < 4; cls = R::SuperStructOf(cls), ++hops)
        if (R::ToString(R::NameOf(cls)) == L"tentacleBallShot_C") {
            ++g_refused;
            return sg::Verdict::Cancel;
        }
    return sg::Verdict::Run;
}

void MakeShot(const N::EventCuePayload& p) {
    void* cls = ue_wrap::object_index::ClassByName(kShot);
    if (!cls) cls = R::FindClass(kShot);
    if (!cls) return;
    const ue_wrap::FVector at{p.x, p.y, p.z};
    void* const shot = ue_wrap::engine::BeginDeferredSpawn(cls, at, g_shotRot);
    if (!shot) return;
    // Its beam is the point; the damage is the host's shot's. A thousandth of a point keeps the shot on the same
    // branch (a point hit, no explosion).
    const int32_t off = R::FindPropertyOffset(R::ClassOf(shot), L"pointDamage");
    float* const dmg = off >= 0 ? reinterpret_cast<float*>(static_cast<uint8_t*>(shot) + off) : nullptr;
    if (dmg && *dmg > 0.f) *dmg = 0.001f;
    ue_wrap::engine::FinishDeferredSpawn(shot, at, g_shotRot);
    if (++g_shotsMade == 1 || g_shotsMade % 50 == 0)
        UE_LOGI("hole: a guard's shot fires here too (%u so far; %u Rozital hits on this player refused)", g_shotsMade,
                g_refused);
}

void SendRooms(N::Session& s) {
    void* rooms[4];
    const int32_t n = ue_wrap::world_instances::Find(kRoom, rooms, 4);
    for (int32_t i = 0; i < n; ++i) {
        ue_wrap::FVector at{};
        if (!rooms[i] || !R::IsLive(rooms[i]) || !ue_wrap::engine::TryGetActorLocation(rooms[i], at)) continue;
        N::EventCuePayload p{};
        p.cueId = kCueHoleRoom;
        p.x = at.X;
        p.y = at.Y;
        p.z = at.Z;
        s.SendReliable(N::ReliableKind::EventCue, &p, sizeof(p));
    }
    void* ctl[4];
    if (ue_wrap::world_instances::Find(kController, ctl, 4) > 0 && ctl[0] && R::IsLive(ctl[0])) {
        N::EventCuePayload p{};
        p.cueId = kCueHoleLive;
        s.SendReliable(N::ReliableKind::EventCue, &p, sizeof(p));
    }
}

// CLIENT: a controller this machine loaded from its save outlives the host's, which ended; it ends too.
void EndStaleControllers(uint64_t now) {
    if (now - g_connectedAt < kLiveStaleMs || now - g_liveAt < kLiveStaleMs) return;
    void* ctl[4];
    const int32_t n = ue_wrap::world_instances::Find(kController, ctl, 4);
    for (int32_t i = 0; i < n; ++i)
        if (ctl[i] && R::IsLive(ctl[i]) && ue_wrap::engine::DestroyActor(ctl[i]))
            UE_LOGI("hole: this machine's own Hole controller from its save ended -- the host has none");
}

void MakeRoom(const N::EventCuePayload& p) {
    void* rooms[4];
    const int32_t n = ue_wrap::world_instances::Find(kRoom, rooms, 4);
    for (int32_t i = 0; i < n; ++i) {
        ue_wrap::FVector at{};
        if (!rooms[i] || !R::IsLive(rooms[i]) || !ue_wrap::engine::TryGetActorLocation(rooms[i], at)) continue;
        const float dx = at.X - p.x, dy = at.Y - p.y, dz = at.Z - p.z;
        if (dx * dx + dy * dy + dz * dz < 500.f * 500.f) return;   // this machine has it (a joiner's, from its save)
    }
    void* cls = ue_wrap::object_index::ClassByName(kRoom);
    if (!cls) cls = R::FindClass(kRoom);
    // Never spawned in this world, its class is not loaded yet: the game's own load of it.
    if (!cls) cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/holeBottom.holeBottom_C");
    const ue_wrap::FVector at{p.x, p.y, p.z};
    void* const room = cls ? ue_wrap::engine::SpawnActor(cls, at) : nullptr;
    UE_LOGI("hole: the host's Hole is open, and its bottom room is made here at (%.0f,%.0f,%.0f) (%s)", p.x, p.y, p.z,
            room ? "made" : cls ? "spawn failed" : "class not found");
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!g_box && (g_box = sg::WatchName(kRowBox, kTagBox, &OnBoxPre, nullptr)))
        UE_LOGI("hole: the Hole controller's box is watched");
    const bool host = s->role() == N::Role::Host;
    if (!g_sense && host && (g_sense = sg::WatchName(kRowSense, kTagSense, nullptr, &OnSensePost)))
        UE_LOGI("hole: a guard's sense is watched");
    if (!g_damage && !host && (g_damage = sg::WatchName(kRowAddDamage, kTagDamage, &OnDamagePre, nullptr)))
        UE_LOGI("hole: a Rozital shot's hit on this player is watched");
    if (!g_hooked && host &&
        (g_hooked = ue_wrap::ufunction_hook::InstallPostHook(ue_wrap::facts::Resolve(kRowBeginDeferredSpawn),
                                                             &OnBeginDeferred)))
        UE_LOGI("hole: a guard's shots are caught");
    if (!g_traceHooked && host &&
        (g_traceHooked = ue_wrap::ufunction_hook::InstallPostHook(ue_wrap::facts::Resolve(kRowLineTrace),
                                                                  &OnLineTracePost, /*armed=*/false)))
        UE_LOGI("hole: a guard's trace is hooked, disarmed");
    // Armed while guards stand: the trace is the game's most common, and the hook reads its caller.
    if (g_traceHooked && host && ::GetTickCount64() >= g_nextArm) {
        g_nextArm = ::GetTickCount64() + 1000;
        void* one[1];
        const bool want = ue_wrap::world_instances::Find(kGuard, one, 1) > 0;
        if (want != g_traceArmed &&
            ue_wrap::ufunction_hook::SetArmed(ue_wrap::facts::Resolve(kRowLineTrace).Fn(), &OnLineTracePost, want)) {
            g_traceArmed = want;
            UE_LOGI("hole: a guard's trace hook %s", want ? "armed" : "disarmed");
        }
    }
    if (!s->connected()) return;
    if (host) {
        SendShots();
        WatchIgetis();
    }
    const uint64_t now = ::GetTickCount64();
    if (now < g_nextRoom) return;
    g_nextRoom = now + kRoomMs;
    if (s->role() == N::Role::Host) {
        SendRooms(*s);
        return;
    }
    if (!g_connectedAt) g_connectedAt = now;
    EndStaleControllers(now);
}

bool OnCue(const N::EventCuePayload& p) {
    if (p.cueId != kCueHoleRoom && p.cueId != kCueHoleLive && p.cueId != kCueShotRot && p.cueId != kCueShotAt &&
        p.cueId != kCueIgetisPlay)
        return false;
    if (!IsClient()) return true;
    if (p.cueId == kCueHoleLive) g_liveAt = ::GetTickCount64();
    else if (p.cueId == kCueShotRot) g_shotRot = ue_wrap::FRotator{p.x, p.y, p.z};
    else if (p.cueId == kCueShotAt) MakeShot(p);
    else if (p.cueId == kCueIgetisPlay) PlayIgetis();
    else MakeRoom(p);
    return true;
}

void OnDisconnect() {
    g_shots.clear();
    g_igetis = nullptr;
    g_igetisAnim = false;
    if (g_traceHooked && g_traceArmed)
        ue_wrap::ufunction_hook::SetArmed(ue_wrap::facts::Resolve(kRowLineTrace).Fn(), &OnLineTracePost, false);
    g_traceArmed = false;
    g_nextRoom = g_connectedAt = g_liveAt = 0;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::hole_sync

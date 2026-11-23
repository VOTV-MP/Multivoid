// coop/world/soltomia_sync.cpp -- see coop/world/soltomia_sync.h.
//
// soltomiaCleaning_C (0.9.0n): its begin play sets its event, finds the ATV and the door keys and clears the ATV's dirt.
// Its box's begin overlap, once, unhides it, plays the ATV's damage sound and its dust, turns the ATV's physics off and
// teleports it to the Soltomia, shuts the garage door, unjams three doors, sets `active`, drops the door's locks and
// talks. `opened`, while `active`, plays its montage after 0.5 s: the notify `throw` hides its mesh, spawns
// prop_sponge_solto_C with a push, turns the ATV's physics on and stops its squeak and dust; the montage's end destroys
// it, and its end cancels the jams and clears its event.

#include "coop/world/soltomia_sync.h"

#include "world_actor_detail.h"  // the world-actor lane's one-spawn pass for a client

#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/asset_load.h"
#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/object_index.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/core/ufunction_hook.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace coop::soltomia_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

constexpr const wchar_t* kClass = L"soltomiaCleaning_C";
UE_FACT_FN(kRowBeginPlay, "soltomia_sync", L"soltomiaCleaning_C", L"ReceiveBeginPlay", Declared);
UE_FACT_FN(kRowBox, "soltomia_sync", L"soltomiaCleaning_C",
           L"BndEvt__soltomiaCleaning_Box_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
UE_FACT_FN(kRowOpened, "soltomia_sync", L"soltomiaCleaning_C", L"opened", Declared);
UE_FACT_FN(kRowDestroyed, "soltomia_sync", L"soltomiaCleaning_C", L"ReceiveDestroyed", Declared);
UE_FACT_FN(kRowBeginDeferredSpawn, "soltomia_sync", L"GameplayStatics", L"BeginDeferredActorSpawnFromClass", Declared);
constexpr int kTagBegin = 0x534F4C42;    // 'SOLB'
constexpr int kTagBox = 0x534F4C58;      // 'SOLX'
constexpr int kTagOpened = 0x534F4C4F;   // 'SOLO'
constexpr int kTagEnd = 0x534F4C45;      // 'SOLE'

std::atomic<N::Session*> g_session{nullptr};
bool g_begin = false, g_box = false, g_opened = false, g_end = false, g_hooked = false;
std::atomic<void*> g_copy{nullptr};   // CLIENT: the copy this lane spawned
float g_yaw = 0.f;                    // CLIENT: the host's Soltomia's yaw, which its cue 111 brings before its place
std::mutex g_bornMx;
std::vector<void*> g_born;            // CLIENT: what the copy's throw made, ended next tick

bool IsClient() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    return s && s->role() == N::Role::Client;
}

int ReadBool(void* obj, const wchar_t* name) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask) || off < 0) return -1;
    return (static_cast<const uint8_t*>(obj)[off] & mask) != 0 ? 1 : 0;
}

void WriteBool(void* obj, const wchar_t* name, bool v) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!obj || !R::FindBoolProperty(R::ClassOf(obj), name, off, mask) || off < 0) return;
    uint8_t& b = static_cast<uint8_t*>(obj)[off];
    b = v ? static_cast<uint8_t>(b | mask) : static_cast<uint8_t>(b & ~mask);
}

void* Field(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return nullptr;
    void* v = nullptr;
    std::memcpy(&v, static_cast<const uint8_t*>(obj) + off, sizeof(v));
    return v && R::IsLive(v) ? v : nullptr;
}

// A function of `obj` (its own or an ancestor's) with no parameters, or with its one bool parameter `bReset`.
bool Run(void* obj, const wchar_t* fnName, int reset = -1) {
    void* const fn = obj ? R::FindDispatchFunction(R::ClassOf(obj), fnName, nullptr) : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    if (reset >= 0) f.Set<bool>(L"bReset", reset != 0);
    return ue_wrap::Call(obj, f);
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

// CLIENT: the copy's own begin play, box and end: the ATV, the doors and the event are the host's.
sg::Verdict OnClientRefusePre(const sg::Call& call) {
    if ((call.tag != kTagBegin && call.tag != kTagBox && call.tag != kTagEnd) || !IsClient())
        return sg::Verdict::Run;
    return sg::Verdict::Cancel;
}

// HOST: its box ran; once shown it is sent, yaw first.
void OnBoxPost(const sg::Call& call) {
    if (call.tag != kTagBox || IsClient() || !call.object || ReadBool(call.object, L"active") != 1) return;
    ue_wrap::FVector at{};
    ue_wrap::FRotator rot{};
    if (!ue_wrap::engine::TryGetActorLocation(call.object, at)) return;
    ue_wrap::engine::TryGetActorRotation(call.object, rot);
    Send(kCueSoltomiaYaw, rot.Yaw, 0.f, 0.f);
    Send(kCueSoltomiaShow, at.X, at.Y, at.Z);
    UE_LOGI("soltomia: the host's Soltomia shows at (%.0f,%.0f,%.0f) yaw %.0f -- sent", at.X, at.Y, at.Z, rot.Yaw);
}

void OnOpenedPost(const sg::Call& call) {
    if (call.tag != kTagOpened || IsClient() || !call.object || ReadBool(call.object, L"active") != 1) return;
    Send(kCueSoltomiaOpened, 0.f, 0.f, 0.f);
    UE_LOGI("soltomia: the host's Soltomia is opened on -- sent");
}

void OnBeginDeferred(void* /*context*/, void* srcObj, void* spawned) {
    void* const copy = g_copy.load(std::memory_order_acquire);
    if (!copy || srcObj != copy || !spawned) return;
    std::lock_guard<std::mutex> lk(g_bornMx);
    g_born.push_back(spawned);
}

void EndBorn() {
    std::vector<void*> born;
    {
        std::lock_guard<std::mutex> lk(g_bornMx);
        born.swap(g_born);
    }
    int ended = 0;
    for (void* o : born)
        if (o && R::IsLive(o) && ue_wrap::engine::DestroyActor(o)) ++ended;
    if (ended) UE_LOGI("soltomia: %d prop(s) this copy's throw made ended -- the host's cross", ended);
}

void Show(const N::EventCuePayload& p) {
    void* cls = ue_wrap::object_index::ClassByName(kClass);
    if (!cls) cls = R::FindClass(kClass);
    // Never spawned in this world, its class is not loaded yet: the game's own load of it.
    if (!cls) cls = ue_wrap::asset_load::LoadObjectByPath(L"/Game/objects/soltomiaCleaning.soltomiaCleaning_C");
    const ue_wrap::FVector at{p.x, p.y, p.z};
    const ue_wrap::FRotator rot{0.f, g_yaw, 0.f};
    // Its class is on the world-actor lane's list, which refuses a client's own spawn of it: this one is let through.
    if (cls) coop::world_actor_sync::detail::SetIncomingClass(cls);
    void* const copy = cls ? ue_wrap::engine::BeginDeferredSpawn(cls, at, rot) : nullptr;
    coop::world_actor_sync::detail::ClearIncomingClass();
    if (!copy) {
        UE_LOGW("soltomia: the host's Soltomia shows, and this world could not make a copy (class %s)", cls ? "found" : "not found");
        return;
    }
    // Known before its begin play runs, which the gate refuses for it.
    g_copy.store(copy, std::memory_order_release);
    ue_wrap::engine::FinishDeferredSpawn(copy, at, rot);
    if (!R::IsLive(copy)) {
        g_copy.store(nullptr, std::memory_order_release);
        return;
    }
    // As its box's run shows it: its squeak, itself, its dust, `active` and its talk.
    Run(Field(copy, L"squeak"), L"Activate", 0);
    ue_wrap::engine::SetActorHiddenInGame(copy, false);
    Run(Field(copy, L"ParticleSystem"), L"Activate", 1);
    WriteBool(copy, L"active", true);
    Run(copy, L"talking");
    UE_LOGI("soltomia: the host's Soltomia shows here too, at (%.0f,%.0f,%.0f) yaw %.0f", p.x, p.y, p.z, g_yaw);
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    if (!g_begin && (g_begin = sg::WatchName(kRowBeginPlay, kTagBegin, &OnClientRefusePre, nullptr)))
        UE_LOGI("soltomia: its begin play is watched");
    if (!g_box && (g_box = sg::WatchName(kRowBox, kTagBox, &OnClientRefusePre, &OnBoxPost)))
        UE_LOGI("soltomia: its box is watched");
    if (!g_opened && (g_opened = sg::WatchName(kRowOpened, kTagOpened, nullptr, &OnOpenedPost)))
        UE_LOGI("soltomia: its opened is watched");
    if (!g_end && (g_end = sg::WatchName(kRowDestroyed, kTagEnd, &OnClientRefusePre, nullptr)))
        UE_LOGI("soltomia: its end is watched");
    if (!g_hooked && s->role() == N::Role::Client &&
        (g_hooked = ue_wrap::ufunction_hook::InstallPostHook(ue_wrap::facts::Resolve(kRowBeginDeferredSpawn),
                                                             &OnBeginDeferred)))
        UE_LOGI("soltomia: a client's copy's births are caught");
    EndBorn();
}

bool OnCue(const N::EventCuePayload& p) {
    if (p.cueId != kCueSoltomiaShow && p.cueId != kCueSoltomiaYaw && p.cueId != kCueSoltomiaOpened) return false;
    if (!IsClient()) return true;
    if (p.cueId == kCueSoltomiaYaw) {
        g_yaw = p.x;
    } else if (p.cueId == kCueSoltomiaShow) {
        Show(p);
    } else {
        void* const copy = g_copy.load(std::memory_order_acquire);
        const bool ran = copy && R::IsLive(copy) && Run(copy, L"opened");
        UE_LOGI("soltomia: the host's Soltomia is opened on, and this copy plays it (ran=%d)", ran ? 1 : 0);
    }
    return true;
}

void OnDisconnect() {
    g_copy.store(nullptr, std::memory_order_release);
    std::lock_guard<std::mutex> lk(g_bornMx);
    g_born.clear();
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::soltomia_sync

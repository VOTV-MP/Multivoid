// coop/creatures/firetank_sync.cpp -- see coop/creatures/firetank_sync.h.
//
// 0.9.0n. flamethrower_C::Activate(bNewActive): when it changes, `isActive`, its own tick, `eff_flamethrower` and `loop`
// set to it, and on `fire` plays. firetank_C's `fire` timer switches its `flamethrowerComponent` on while its chased
// actor is in its attack zone with a clear line, and off 3 s later.

#include "coop/creatures/firetank_sync.h"

#include "coop/net/protocol.h"
#include "coop/net/session.h"

#include "ue_wrap/core/call.h"
#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/engine/engine.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_instances.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace coop::firetank_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

constexpr const wchar_t* kTank = L"firetank_C";
UE_FACT_FN(kRowActivate, "firetank_sync", L"flamethrower_C", L"Activate", Declared);
constexpr int kTagActivate = 0x46545241;   // 'FTRA'
constexpr float kNearCm = 600.f;

std::atomic<N::Session*> g_session{nullptr};
bool g_watched = false;
uint32_t g_sent = 0, g_shown = 0;

bool IsRole(N::Role r) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    return s && s->role() == r;
}

void* Field(void* obj, const wchar_t* name) {
    const int32_t off = obj ? R::FindPropertyOffset(R::ClassOf(obj), name) : -1;
    if (off < 0) return nullptr;
    void* v = nullptr;
    std::memcpy(&v, static_cast<const uint8_t*>(obj) + off, sizeof(v));
    return v && R::IsLive(v) ? v : nullptr;
}

// The live Firetank nearest `at` within 6 m, or null.
void* TankNear(const ue_wrap::FVector& at) {
    void* found[8];
    const int32_t n = ue_wrap::world_instances::Find(kTank, found, 8);
    void* best = nullptr;
    float bestD = kNearCm * kNearCm;
    for (int32_t i = 0; i < n; ++i) {
        ue_wrap::FVector p{};
        if (!found[i] || !R::IsLive(found[i]) || !ue_wrap::engine::TryGetActorLocation(found[i], p)) continue;
        const float dx = p.X - at.X, dy = p.Y - at.Y, dz = p.Z - at.Z;
        const float d = dx * dx + dy * dy + dz * dz;
        if (d < bestD) {
            bestD = d;
            best = found[i];
        }
    }
    return best;
}

// HOST: a Firetank's flamethrower switched; the clients' copies show it.
void OnActivatePost(const sg::Call& call) {
    if (call.tag != kTagActivate || !IsRole(N::Role::Host) || !call.object) return;
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->connected()) return;
    ue_wrap::FVector at{};
    if (!ue_wrap::engine::TryGetActorLocation(call.object, at)) return;
    void* const tank = TankNear(at);
    if (!tank || !ue_wrap::engine::TryGetActorLocation(tank, at)) return;   // a player's flamethrower: not this lane's
    int32_t off = -1;
    uint8_t mask = 0;
    if (!R::FindBoolProperty(R::ClassOf(call.object), L"isActive", off, mask) || off < 0) return;
    const bool on = (static_cast<const uint8_t*>(call.object)[off] & mask) != 0;
    N::EventCuePayload p{};
    p.cueId = on ? kCueFlameOn : kCueFlameOff;
    p.x = at.X;
    p.y = at.Y;
    p.z = at.Z;
    s->SendReliable(N::ReliableKind::EventCue, &p, sizeof(p));
    if (++g_sent == 1 || g_sent % 25 == 0)
        UE_LOGI("firetank: the host's Firetank's flame %s -- sent (%u so far)", on ? "on" : "off", g_sent);
}

// ActorComponent::SetActive(bNewActive, bReset) on `component`.
bool SetActive(void* component, bool on) {
    void* const fn = component ? R::FindDispatchFunction(R::ClassOf(component), L"SetActive", nullptr) : nullptr;
    ue_wrap::ParamFrame f(fn);
    if (!f.valid()) return false;
    f.Set<bool>(L"bNewActive", on);
    f.Set<bool>(L"bReset", true);
    return ue_wrap::Call(component, f);
}

// CLIENT: the copy's flame shows or stops; its tick, which burns, stays off.
void Show(const N::EventCuePayload& p, bool on) {
    void* const tank = TankNear({p.x, p.y, p.z});
    void* flame = tank ? Field(tank, L"flamethrowerComponent") : nullptr;
    if (flame && R::ClassNameOf(flame).find(L"ChildActorComponent") != std::wstring::npos) flame = Field(flame, L"ChildActor");
    const bool eff = flame && SetActive(Field(flame, L"eff_flamethrower"), on);
    const bool loop = flame && SetActive(Field(flame, L"loop"), on);
    if (++g_shown == 1 || g_shown % 25 == 0)
        UE_LOGI("firetank: the host's Firetank's flame %s here too (tank=%d flame=%d effect=%d loop=%d, %u so far)",
                on ? "on" : "off", tank ? 1 : 0, flame ? 1 : 0, eff ? 1 : 0, loop ? 1 : 0, g_shown);
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != N::Role::Host) return;
    if (!g_watched && (g_watched = sg::WatchName(kRowActivate, kTagActivate, nullptr, &OnActivatePost)))
        UE_LOGI("firetank: a flamethrower's Activate is watched");
}

bool OnCue(const N::EventCuePayload& p) {
    if (p.cueId != kCueFlameOn && p.cueId != kCueFlameOff) return false;
    if (IsRole(N::Role::Client)) Show(p, p.cueId == kCueFlameOn);
    return true;
}

void OnDisconnect() { g_session.store(nullptr, std::memory_order_release); }

}  // namespace coop::firetank_sync

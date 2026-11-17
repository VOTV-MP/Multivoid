// coop/world/arir_ship_sync.cpp -- see coop/world/arir_ship_sync.h.

#include "coop/world/arir_ship_sync.h"

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

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace coop::arir_ship_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;

constexpr const wchar_t* kShipClass = L"arirShipAppear_C";
UE_FACT_FN(kRowAppear, "arir_ship_sync", L"arirShipAppear_C", L"appear", Declared);
constexpr int kTag = 0x41525300;   // 'ARS'

std::atomic<N::Session*> g_session{nullptr};
bool g_registered = false;
int g_applying = 0;

uint64_t Now() { return ::GetTickCount64(); }

bool Visible(void* ship) {
    int32_t off = -1;
    uint8_t mask = 0;
    if (!ship || !R::FindBoolProperty(R::ClassOf(ship), L"visible", off, mask)) return false;
    return (static_cast<const uint8_t*>(ship)[off] & mask) != 0;
}

bool Cue(void* ship, N::EventCuePayload& p) {
    ue_wrap::FVector at{};
    if (!ue_wrap::engine::TryGetActorLocation(ship, at)) return false;
    p = N::EventCuePayload{};
    p.cueId = kCueArirShip;
    p.x = at.X;
    p.y = at.Y;
    p.z = at.Z;
    return true;
}

void OnPost(const sg::Call& call) {
    if (call.tag != kTag || g_applying) return;
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != N::Role::Host || !Visible(call.object)) return;
    N::EventCuePayload p;
    if (!Cue(call.object, p)) return;
    const bool sent = s->connected() && s->SendReliable(N::ReliableKind::EventCue, &p, sizeof(p));
    UE_LOGI("arir_ship: the Ariral ship appeared at (%.0f,%.0f,%.0f) -- %s", p.x, p.y, p.z,
            sent ? "EventCue sent" : "no peer to send it to");
}

void* ShipNear(float x, float y, float z) {
    void* best = nullptr;
    float bestD = 1e18f;
    for (void* o : R::FindObjectsByClass(kShipClass)) {
        ue_wrap::FVector at{};
        if (!o || !R::IsLive(o) || R::NameStartsWith(R::NameOf(o), L"Default__") ||
            !ue_wrap::engine::TryGetActorLocation(o, at))
            continue;
        const float d = (at.X - x) * (at.X - x) + (at.Y - y) * (at.Y - y) + (at.Z - z) * (at.Z - z);
        if (d < bestD) { bestD = d; best = o; }
    }
    return best;
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    if (g_registered) return;
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running()) return;
    static uint64_t nextTry = 0;
    if (Now() < nextTry) return;
    nextTry = Now() + 1000;
    if ((g_registered = sg::WatchName(kRowAppear, kTag, nullptr, &OnPost)))
        UE_LOGI("arir_ship: the Ariral ship's appear is watched");
}

bool OnCue(const N::EventCuePayload& p) {
    if (p.cueId != kCueArirShip) return false;
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != N::Role::Client) return true;
    void* const ship = ShipNear(p.x, p.y, p.z);
    if (!ship) {
        UE_LOGW("arir_ship: the host's Ariral ship appeared, and this copy has none -- not shown");
        return true;
    }
    if (Visible(ship)) return true;
    ue_wrap::ParamFrame f(R::FindDispatchFunctionCached(R::ClassOf(ship), L"appear"));
    ++g_applying;
    const bool ran = f.valid() && f.Set<bool>(L"dir", true) && ue_wrap::Call(ship, f);
    --g_applying;
    UE_LOGI("arir_ship: the host's Ariral ship appears here too (ran=%d)", ran ? 1 : 0);
    return true;
}

void SendJoin(int slot) {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || s->role() != N::Role::Host) return;
    for (void* o : R::FindObjectsByClass(kShipClass)) {
        if (!o || !R::IsLive(o) || R::NameStartsWith(R::NameOf(o), L"Default__") || !Visible(o)) continue;
        N::EventCuePayload p;
        if (Cue(o, p)) s->SendReliableToSlot(slot, N::ReliableKind::EventCue, &p, sizeof(p));
    }
}

}  // namespace coop::arir_ship_sync

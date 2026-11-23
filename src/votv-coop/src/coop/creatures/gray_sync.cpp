// coop/creatures/gray_sync.cpp -- see coop/creatures/gray_sync.h.
//
// 0.9.0n. grayTest_C's Sphere begin overlap: a mainPlayer_C other actor calls its grayController's Despawn, whose first
// step is lib_C::getMainPlayer and that player's punch (its forward times a constant, the knock-down), then, 1 s on,
// the Grays' destroy and, 60 s on, the controller's. The controller's ReceiveDestroyed clears gamemode.car.zapped.

#include "coop/creatures/gray_sync.h"

#include "coop/creatures/served_player.h"
#include "coop/net/protocol.h"
#include "coop/net/session.h"
#include "coop/player/players_registry.h"
#include "coop/player/remote_player.h"

#include "ue_wrap/core/log.h"
#include "ue_wrap/core/reflection.h"
#include "ue_wrap/core/reflection_props.h"
#include "ue_wrap/core/script_gate.h"
#include "ue_wrap/core/types.h"
#include "ue_wrap/facts/function_row.h"
#include "ue_wrap/world/world_instances.h"
#include "ue_wrap/world/world_singleton.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace coop::gray_sync {
namespace {

namespace N  = coop::net;
namespace R  = ue_wrap::reflection;
namespace sg = ue_wrap::script_gate;
namespace SP = coop::served_player;

UE_FACT_FN(kRowSphere, "gray_sync", L"grayTest_C",
           L"BndEvt__grayTest_Sphere_K2Node_ComponentBoundEvent_0_ComponentBeginOverlapSignature__DelegateSignature",
           Declared);
constexpr int kTagSphere = 0x47525953;   // 'GRYS'
constexpr uint64_t kPollMs = 500, kRepeatMs = 5000;

std::atomic<N::Session*> g_session{nullptr};
bool g_watched = false;
void* g_served = nullptr;   // HOST: the controller served while its Grays stand, so its getMainPlayer watch is live
int g_zapped = -1;          // HOST: the ATV's zapped as last sent
uint64_t g_nextPoll = 0, g_lastSent = 0;

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

// The ATV's zapped bit: its byte and its mask, or null.
uint8_t* ZappedBit(uint8_t& mask) {
    void* const car = Field(ue_wrap::world_singleton::Gamemode(), L"car");
    int32_t off = -1;
    if (!car || !R::FindBoolProperty(R::ClassOf(car), L"zapped", off, mask) || off < 0) return nullptr;
    return static_cast<uint8_t*>(car) + off;
}

// HOST: a Gray's sphere is taken; a client's body has its controller serve that client to the punch.
sg::Verdict OnSpherePre(const sg::Call& call) {
    if (call.tag != kTagSphere || !IsRole(N::Role::Host) || !call.object) return sg::Verdict::Run;
    static int32_t s_off = -2;
    if (s_off == -2) s_off = R::FindParamOffset(call.function, L"OtherActor");
    void* other = nullptr;
    if (s_off >= 0 && call.locals) std::memcpy(&other, call.locals + s_off, sizeof(other));
    if (!other || !R::IsLive(other) || R::ClassNameOf(other) != L"mainPlayer_C") return sg::Verdict::Run;
    const uint8_t slot = coop::players::Registry::Get().PeerIdOfActor(other);
    void* const controller = Field(call.object, L"grayController");
    if (!controller) return sg::Verdict::Run;
    // The body that took it is the one knocked down: a client's through the served answer, the host's natively.
    if (slot > 0 && slot < N::kMaxPeers) {
        if (SP::Serve(controller, slot, SP::kMainPlayer)) g_served = controller;
        UE_LOGI("gray: slot %u's body takes a Gray -- the knock-down is its own", static_cast<unsigned>(slot));
    } else {
        SP::Unserve(controller);
        g_served = nullptr;
    }
    return sg::Verdict::Run;
}

// HOST: while Grays stand, their controller is served a client (its watch must be live before a touch's own call),
// the client nearest the first Gray; none connected, or no Grays, it is not.
void KeepServed() {
    void* gray[1];
    void* const g = ue_wrap::world_instances::Find(L"grayTest_C", gray, 1) > 0 && gray[0] && R::IsLive(gray[0]) ? gray[0]
                                                                                                         : nullptr;
    void* const controller = g ? Field(g, L"grayController") : nullptr;
    if (!controller) {
        if (g_served) SP::Unserve(g_served);
        g_served = nullptr;
        return;
    }
    if (controller == g_served && SP::ServedSlot(controller)) return;
    auto& reg = coop::players::Registry::Get();
    for (uint8_t slot = 1; slot < N::kMaxPeers; ++slot) {
        coop::RemotePlayer* const rp = reg.Puppet(slot);
        if (!rp || !rp->valid() || !rp->GetActor()) continue;
        if (SP::Serve(controller, slot, SP::kMainPlayer)) g_served = controller;
        return;
    }
}

void PollZapped(N::Session& s) {
    uint8_t mask = 0;
    uint8_t* const bit = ZappedBit(mask);
    if (!bit) return;
    const int zapped = (*bit & mask) ? 1 : 0;
    const uint64_t now = ::GetTickCount64();
    if (zapped == g_zapped && !(zapped && now - g_lastSent >= kRepeatMs)) return;
    N::EventCuePayload p{};
    p.cueId = kCueAtvZapped;
    p.x = static_cast<float>(zapped);
    s.SendReliable(N::ReliableKind::EventCue, &p, sizeof(p));
    if (zapped != g_zapped) UE_LOGI("gray: the host's ATV is %s -- sent", zapped ? "zapped" : "free again");
    g_zapped = zapped;
    g_lastSent = now;
}

}  // namespace

void Install(N::Session* session) { g_session.store(session, std::memory_order_release); }

void Tick() {
    N::Session* const s = g_session.load(std::memory_order_acquire);
    if (!s || !s->running() || s->role() != N::Role::Host) return;
    if (!g_watched && (g_watched = sg::WatchName(kRowSphere, kTagSphere, &OnSpherePre, nullptr)))
        UE_LOGI("gray: a Gray's sphere is watched");
    const uint64_t now = ::GetTickCount64();
    if (!s->connected() || now < g_nextPoll) return;
    g_nextPoll = now + kPollMs;
    PollZapped(*s);
    KeepServed();
}

bool OnCue(const N::EventCuePayload& p) {
    if (p.cueId != kCueAtvZapped) return false;
    if (!IsRole(N::Role::Client)) return true;
    uint8_t mask = 0;
    if (uint8_t* const bit = ZappedBit(mask)) {
        const bool on = p.x > 0.5f;
        const bool was = (*bit & mask) != 0;
        *bit = on ? static_cast<uint8_t>(*bit | mask) : static_cast<uint8_t>(*bit & ~mask);
        if (on != was) UE_LOGI("gray: the host's ATV is %s, and this machine's copy too", on ? "zapped" : "free again");
    }
    return true;
}

void OnDisconnect() {
    if (g_served) SP::Unserve(g_served);
    g_served = nullptr;
    g_zapped = -1;
    g_nextPoll = g_lastSent = 0;
    g_session.store(nullptr, std::memory_order_release);
}

}  // namespace coop::gray_sync
